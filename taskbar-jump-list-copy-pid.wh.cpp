// ==WindhawkMod==
// @id              taskbar-jump-list-copy-pid
// @name            Taskbar Jump List Copy PID
// @description     Adds a "Copy PID" item to the taskbar jump list
// @version         1.0
// @author          toshiotomikawa
// @license         GPL-3.0-only
// @include         explorer.exe
// @include         ShellExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// Based on the "Taskbar Jump List Volume Control" mod by Ara
// (https://github.com/ara-hwang/windhawk-taskbar-jump-list-volume-control),
// which is based on the "Taskbar Volume Control Per-App" mod by m417z.

// ==WindhawkModReadme==
/*
# Taskbar Jump List Copy PID

Adds a "Copy PID" item to the jump list that opens when you right-click a
taskbar button on Windows 11. Clicking it copies the process ID of the app to
the clipboard. If the app has windows in several processes, all of their PIDs
are copied, separated by commas.

## How it works

The jump list is rendered by `ShellExperienceHost.exe`, not by `explorer.exe`,
so this mod is loaded into both processes. In `explorer.exe` it finds the
processes of the right-clicked button and copies to the clipboard, and in
`ShellExperienceHost.exe` it inserts the item into the jump list.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- label: Copiar PID
  $name: Item label
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include <commctrl.h>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

using namespace winrt::Windows::UI::Xaml;

////////////////////////////////////////////////////////////////////////////////
// Definitions shared by both processes.

// Window in explorer.exe. ShellExperienceHost asks it to copy the PIDs.
constexpr WCHAR kHostWndClass[] = L"WhJumpListCopyPid_Host";
// Window in ShellExperienceHost.exe. explorer sends the target app info to it.
constexpr WCHAR kShellWndClass[] = L"WhJumpListCopyPid_Shell";

constexpr UINT kMsgCopyPids = WM_APP + 0x61;

constexpr ULONG_PTR kCopyDataMagic = 0x4A4C5049;  // 'JLPI'

constexpr DWORD kMaxPids = 16;

struct JumpListPidInfo {
    ULONGLONG hostWnd;
    DWORD count;
    DWORD pids[kMaxPids];
};

bool g_isShellExperienceHost;

// The module handle of the mod itself, as opposed to the host exe. Window
// classes are registered with it, so that they're owned by the mod.
HINSTANCE GetModInstance() {
    static HINSTANCE instance = []() {
        HMODULE module = nullptr;
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)&GetModInstance, &module);
        return (HINSTANCE)module;
    }();
    return instance;
}

using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         void* procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        void* procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    RUN_FROM_WINDOW_THREAD_PARAM* param =
                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }

            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param{
        .proc = proc,
        .procParam = procParam,
    };
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

std::wstring FormatPids(const DWORD* pids, DWORD count) {
    std::wstring text;
    for (DWORD i = 0; i < count; i++) {
        if (i) {
            text += L", ";
        }
        text += std::to_wstring(pids[i]);
    }
    return text;
}

////////////////////////////////////////////////////////////////////////////////
// explorer.exe: taskbar integration.

// PIDs of the app whose jump list was opened last. Only accessed from the
// taskbar thread.
std::vector<DWORD> g_targetPids;

HWND g_hostWnd;

void* QueryViaVtable(void* object, void* vtable) {
    void* ptr = object;
    while (*(void**)ptr != vtable) {
        ptr = (void**)ptr + 1;
    }
    return ptr;
}

using CWindowTaskItem_GetWindow_t = HWND(WINAPI*)(void* pThis);
CWindowTaskItem_GetWindow_t CWindowTaskItem_GetWindow;

using CImmersiveTaskItem_GetAppWindow_t = HWND(WINAPI*)(void* pThis);
CImmersiveTaskItem_GetAppWindow_t CImmersiveTaskItem_GetAppWindow;

void* CImmersiveTaskItem_vftable;
void* CImmersiveTaskItem_vftable_ITaskItem;

using CTaskGroup_GetNumItems_t = int(WINAPI*)(void* pThis);
CTaskGroup_GetNumItems_t CTaskGroup_GetNumItems;

HDPA GetTaskItemsArray(void* taskGroup) {
    // Relies on CTaskGroup::GetNumItems being just
    // `return DPA_GetPtrCount(this->taskItemsArray);`. Calling it with an array
    // of pointers to ints returns the offset of the array member.
    static size_t offset = []() {
        constexpr int kIntArraySize = 256;
        int arrayOfInts[kIntArraySize];
        int* arrayOfIntPtrs[kIntArraySize];
        for (int i = 0; i < kIntArraySize; i++) {
            arrayOfInts[i] = i;
            arrayOfIntPtrs[i] = &arrayOfInts[i];
        }

        return CTaskGroup_GetNumItems(arrayOfIntPtrs);
    }();

    return (HDPA)((void**)taskGroup)[offset];
}

HWND GetWindowFromTaskItem(void* taskItem) {
    if (!taskItem) {
        return nullptr;
    }

    if (*(void**)taskItem == CImmersiveTaskItem_vftable_ITaskItem) {
        void* immersiveTaskItem =
            QueryViaVtable(taskItem, CImmersiveTaskItem_vftable);
        return CImmersiveTaskItem_GetAppWindow(immersiveTaskItem);
    }

    return CWindowTaskItem_GetWindow(taskItem);
}

bool IsProcessNamed(DWORD pid, PCWSTR name) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }

    WCHAR path[MAX_PATH];
    DWORD size = ARRAYSIZE(path);
    bool result = false;
    if (QueryFullProcessImageName(process, 0, path, &size)) {
        PCWSTR fileName = wcsrchr(path, L'\\');
        fileName = fileName ? fileName + 1 : path;
        result = _wcsicmp(fileName, name) == 0;
    }

    CloseHandle(process);
    return result;
}

// UWP apps are hosted by ApplicationFrameHost.exe, the app itself owns a child
// window of the frame.
DWORD GetAppProcessId(HWND hWnd) {
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(hWnd, &pid) || !pid) {
        return 0;
    }

    if (!IsProcessNamed(pid, L"ApplicationFrameHost.exe")) {
        return pid;
    }

    struct Param {
        DWORD framePid;
        DWORD appPid;
    } param{pid, 0};

    EnumChildWindows(
        hWnd,
        [](HWND hChild, LPARAM lParam) -> BOOL {
            auto* param = (Param*)lParam;
            DWORD childPid = 0;
            if (GetWindowThreadProcessId(hChild, &childPid) && childPid &&
                childPid != param->framePid) {
                param->appPid = childPid;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&param);

    return param.appPid ? param.appPid : pid;
}

std::vector<DWORD> GetProcessIdsFromTaskGroup(void* taskGroup) {
    std::vector<DWORD> pids;
    if (!taskGroup) {
        return pids;
    }

    HDPA taskItemsArray = GetTaskItemsArray(taskGroup);
    if (!taskItemsArray) {
        return pids;
    }

    int count = DPA_GetPtrCount(taskItemsArray);
    for (int i = 0; i < count; i++) {
        HWND hWnd = GetWindowFromTaskItem(DPA_GetPtr(taskItemsArray, i));
        DWORD pid = hWnd ? GetAppProcessId(hWnd) : 0;
        if (pid && std::find(pids.begin(), pids.end(), pid) == pids.end()) {
            pids.push_back(pid);
        }
    }

    return pids;
}

void CopyTextToClipboard(HWND hWnd, const std::wstring& text) {
    if (!OpenClipboard(hWnd)) {
        Wh_Log(L"OpenClipboard failed: %u", GetLastError());
        return;
    }

    EmptyClipboard();

    size_t bytes = (text.size() + 1) * sizeof(WCHAR);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* data = GlobalLock(memory);
        memcpy(data, text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) {
            GlobalFree(memory);
        }
    }

    CloseClipboard();
}

void CloseJumpList() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_ESCAPE;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_ESCAPE;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
}

LRESULT CALLBACK HostWndProc(HWND hWnd,
                             UINT uMsg,
                             WPARAM wParam,
                             LPARAM lParam) {
    switch (uMsg) {
        case kMsgCopyPids:
            if (!g_targetPids.empty()) {
                std::wstring text =
                    FormatPids(g_targetPids.data(), (DWORD)g_targetPids.size());
                CopyTextToClipboard(hWnd, text);
                Wh_Log(L"Copied: %s", text.c_str());
            }
            CloseJumpList();
            return 0;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool g_hostWndClassRegistered;

void EnsureHostWindow() {
    if (g_hostWnd) {
        return;
    }

    if (!g_hostWndClassRegistered) {
        WNDCLASS wc{};
        wc.lpfnWndProc = HostWndProc;
        wc.hInstance = GetModInstance();
        wc.lpszClassName = kHostWndClass;
        if (!RegisterClass(&wc)) {
            Wh_Log(L"RegisterClass failed: %u", GetLastError());
            return;
        }

        g_hostWndClassRegistered = true;
    }

    g_hostWnd = CreateWindowEx(0, kHostWndClass, nullptr, 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, GetModInstance(),
                               nullptr);
    if (!g_hostWnd) {
        Wh_Log(L"CreateWindowEx failed: %u", GetLastError());
        return;
    }

    // ShellExperienceHost runs in an AppContainer, allow its messages.
    ChangeWindowMessageFilterEx(g_hostWnd, kMsgCopyPids, MSGFLT_ALLOW,
                                nullptr);
}

void OnShowJumpView(void* taskGroup) {
    EnsureHostWindow();
    if (!g_hostWnd) {
        return;
    }

    g_targetPids = GetProcessIdsFromTaskGroup(taskGroup);

    JumpListPidInfo info{};
    info.hostWnd = (ULONGLONG)(ULONG_PTR)g_hostWnd;
    info.count = (DWORD)std::min<size_t>(g_targetPids.size(), kMaxPids);
    for (DWORD i = 0; i < info.count; i++) {
        info.pids[i] = g_targetPids[i];
    }

    Wh_Log(L"Jump list: pids=%s",
           FormatPids(info.pids, info.count).c_str());

    HWND shellWnd =
        FindWindowEx(HWND_MESSAGE, nullptr, kShellWndClass, nullptr);
    if (!shellWnd) {
        Wh_Log(L"ShellExperienceHost window not found");
        return;
    }

    // Sent synchronously so that the item is in place before the jump list is
    // measured and shown.
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataMagic;
    cds.cbData = sizeof(info);
    cds.lpData = &info;
    DWORD_PTR result = 0;
    if (!SendMessageTimeout(shellWnd, WM_COPYDATA, (WPARAM)g_hostWnd,
                            (LPARAM)&cds, SMTO_ABORTIFHUNG, 500, &result)) {
        Wh_Log(L"SendMessageTimeout failed: %u", GetLastError());
    }
}

using CTaskListWnd_ShowJumpView_t = HRESULT(WINAPI*)(void* pThis,
                                                     void* taskGroup,
                                                     void* taskItem,
                                                     bool param3);
CTaskListWnd_ShowJumpView_t CTaskListWnd_ShowJumpView_Original;
HRESULT WINAPI CTaskListWnd_ShowJumpView_Hook(void* pThis,
                                              void* taskGroup,
                                              void* taskItem,
                                              bool param3) {
    Wh_Log(L">");

    OnShowJumpView(taskGroup);

    return CTaskListWnd_ShowJumpView_Original(pThis, taskGroup, taskItem,
                                              param3);
}

bool HookTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Couldn't load taskbar.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {
            {LR"(public: virtual long __cdecl CTaskListWnd::ShowJumpView(struct ITaskGroup *,struct ITaskItem *,bool))"},
            &CTaskListWnd_ShowJumpView_Original,
            CTaskListWnd_ShowJumpView_Hook,
        },
        {
            {LR"(public: virtual int __cdecl CTaskGroup::GetNumItems(void))"},
            &CTaskGroup_GetNumItems,
        },
        {
            {LR"(public: virtual struct HWND__ * __cdecl CWindowTaskItem::GetWindow(void))"},
            &CWindowTaskItem_GetWindow,
        },
        {
            {LR"(public: virtual struct HWND__ * __cdecl CImmersiveTaskItem::GetAppWindow(void))"},
            &CImmersiveTaskItem_GetAppWindow,
        },
        {
            {LR"(const CImmersiveTaskItem::`vftable')"},
            &CImmersiveTaskItem_vftable,
        },
        {
            {LR"(const CImmersiveTaskItem::`vftable'{for `ITaskItem'})"},
            &CImmersiveTaskItem_vftable_ITaskItem,
        },
    };

    return WindhawkUtils::HookSymbols(module, taskbarDllHooks,
                                      ARRAYSIZE(taskbarDllHooks));
}

////////////////////////////////////////////////////////////////////////////////
// ShellExperienceHost.exe: jump list UI.

constexpr WCHAR kGlyphCopy[] = L"";

constexpr float kIconSize = 16;
constexpr float kIconToTextSpacing = 12;

constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT kPollIntervalMs = 50;
constexpr int kPollCount = 40;

HANDLE g_shellThread;
std::atomic<HWND> g_shellWnd;
std::atomic<HWND> g_pidHostWnd;

// Written on the Windhawk engine thread, read on the jump list UI thread.
std::atomic<bool> g_labelChanged;
WCHAR g_label[64] = L"Copiar PID";

// Accessed from the mod's window thread, and from the jump list UI thread
// while the former is blocked waiting for it.
struct {
    DWORD count = 0;
    DWORD pids[kMaxPids]{};
    DWORD seq = 0;
} g_info;

// Only accessed from the mod's window thread.
HWND g_jumpListWnd;
int g_pollsLeft;
bool g_appliedForSeq;

enum class AttachMode {
    None,
    Header,  // Set as the header of the system items list.
    Panel,   // Inserted next to the list in its parent panel.
};

// Only accessed from the jump list UI thread. Holds strong XAML references,
// which are released on the UI thread by RemovePidUi(), never by the automatic
// destructor at process exit.
struct UiState {
    AttachMode mode = AttachMode::None;
    winrt::weak_ref<Controls::ListViewBase> list;
    winrt::event_token layoutUpdatedToken;
    int layoutCount = 0;
    Controls::ListViewItem root = nullptr;
    Controls::FontIcon icon = nullptr;
    Controls::Grid content = nullptr;
    Controls::TextBlock labelText = nullptr;
    winrt::event_token tappedToken;
    winrt::event_token keyDownToken;
    // Panel mode.
    Controls::Panel parentPanel = nullptr;
    Controls::StackPanel wrapper = nullptr;
    bool styled = false;
    bool aligned = false;
    int alignTries = 0;
    DWORD appliedSeq = 0;
};
[[clang::no_destroy]] UiState g_ui;

// The thread which created the elements of g_ui, 0 if there are none. Only
// written on that thread, but read by the mod's window thread on unload.
std::atomic<DWORD> g_uiThreadId;

bool g_headerUnsupported;

void RequestCopy() {
    if (!PostMessage(g_pidHostWnd, kMsgCopyPids, 0, 0)) {
        Wh_Log(L"PostMessage failed: %u", GetLastError());
    }
}

void CreatePidUi() {
    Controls::FontIcon icon;
    icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
    icon.FontSize(kIconSize);
    icon.Width(kIconSize);
    icon.Glyph(kGlyphCopy);
    icon.VerticalAlignment(VerticalAlignment::Center);

    Controls::TextBlock labelText;
    labelText.VerticalAlignment(VerticalAlignment::Center);
    labelText.TextTrimming(TextTrimming::CharacterEllipsis);

    Controls::Grid content;
    content.ColumnSpacing(kIconToTextSpacing);
    content.HorizontalAlignment(HorizontalAlignment::Stretch);
    Controls::ColumnDefinition column0;
    column0.Width(GridLengthHelper::Auto());
    Controls::ColumnDefinition column1;
    column1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    content.ColumnDefinitions().Append(column0);
    content.ColumnDefinitions().Append(column1);
    Controls::Grid::SetColumn(icon, 0);
    Controls::Grid::SetColumn(labelText, 1);
    content.Children().Append(icon);
    content.Children().Append(labelText);

    Controls::ListViewItem root;
    root.Content(content);
    root.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    root.IsTabStop(true);

    g_ui.tappedToken = root.Tapped([](auto&&, Input::TappedRoutedEventArgs const& e) {
        e.Handled(true);
        RequestCopy();
    });

    g_ui.keyDownToken = root.KeyDown(
        [](auto&&, Input::KeyRoutedEventArgs const& e) {
            if (e.Key() == winrt::Windows::System::VirtualKey::Enter ||
                e.Key() == winrt::Windows::System::VirtualKey::Space) {
                e.Handled(true);
                RequestCopy();
            }
        });

    g_ui.root = root;
    g_ui.icon = icon;
    g_ui.content = content;
    g_ui.labelText = labelText;
    g_ui.styled = false;
    g_ui.aligned = false;
    g_ui.alignTries = 0;
    g_uiThreadId = GetCurrentThreadId();
}

template <typename T>
T FindDescendant(DependencyObject object, int depth = 0) {
    if (!object || depth > 16) {
        return nullptr;
    }

    if (auto found = object.try_as<T>()) {
        return found;
    }

    int count = Media::VisualTreeHelper::GetChildrenCount(object);
    for (int i = 0; i < count; i++) {
        if (auto found = FindDescendant<T>(
                Media::VisualTreeHelper::GetChild(object, i), depth + 1)) {
            return found;
        }
    }

    return nullptr;
}

bool HasLetter(winrt::hstring const& text) {
    for (wchar_t c : text) {
        if (iswalpha(c)) {
            return true;
        }
    }
    return false;
}

Controls::TextBlock FindItemNameText(DependencyObject object, int depth = 0) {
    if (!object || depth > 16) {
        return nullptr;
    }

    if (auto text = object.try_as<Controls::TextBlock>()) {
        if (HasLetter(text.Text())) {
            return text;
        }
    }

    int count = Media::VisualTreeHelper::GetChildrenCount(object);
    for (int i = 0; i < count; i++) {
        if (auto found = FindItemNameText(
                Media::VisualTreeHelper::GetChild(object, i), depth + 1)) {
            return found;
        }
    }

    return nullptr;
}

// Copies the look of the list's own items, so that the item blends in.
void StyleLikeListItems(Controls::ListViewBase list) {
    if (g_ui.styled || list.Items().Size() == 0) {
        return;
    }

    auto container =
        list.ContainerFromIndex(0).try_as<Controls::ListViewItem>();
    if (!container) {
        return;
    }

    if (auto style = container.Style()) {
        g_ui.root.Style(style);
    }
    g_ui.root.Padding(container.Padding());
    g_ui.root.MinHeight(container.MinHeight());
    g_ui.root.Height(container.ActualHeight() > 0 ? container.ActualHeight()
                                                  : container.Height());
    g_ui.root.Margin(container.Margin());

    // The item's name, not a glyph of an icon font, which would turn the text
    // into boxes.
    auto text = FindItemNameText(container);
    if (!text || text.ActualWidth() <= 0) {
        // Not laid out yet, try again on the next layout pass.
        return;
    }

    g_ui.labelText.FontSize(text.FontSize());
    g_ui.labelText.FontWeight(text.FontWeight());

    g_ui.styled = true;
}

float LeftInList(UIElement element, Controls::ListViewBase const& list) {
    return element.TransformToVisual(list).TransformPoint({0, 0}).X;
}

// Finds, in the list's own items, where the icons are centered and where the
// names start.
bool FindItemLayout(Controls::ListViewBase const& list,
                    float* iconCenterX,
                    float* textX) {
    std::optional<float> foundIconCenterX;
    std::optional<float> foundTextX;

    uint32_t count = list.Items().Size();
    for (uint32_t i = 0; i < count; i++) {
        auto container =
            list.ContainerFromIndex(i).try_as<Controls::ListViewItem>();
        if (!container) {
            continue;
        }

        auto text = FindItemNameText(container);
        if (!text || text.ActualWidth() <= 0) {
            continue;
        }

        float x = LeftInList(text, list);
        if (!foundTextX) {
            foundTextX = x;
        }

        FrameworkElement icon = FindDescendant<Controls::IconElement>(container);
        if (!icon || icon.ActualWidth() <= 0) {
            icon = FindDescendant<Controls::Image>(container);
        }
        if (icon && icon.ActualWidth() > 0) {
            float iconX = LeftInList(icon, list);
            if (iconX < x) {
                foundIconCenterX = iconX + (float)icon.ActualWidth() / 2;
                foundTextX = x;
                break;
            }
        }
    }

    if (!foundTextX) {
        return false;
    }

    *textX = *foundTextX;
    *iconCenterX = foundIconCenterX
                       ? *foundIconCenterX
                       : *foundTextX - kIconToTextSpacing - kIconSize / 2;
    return true;
}

// Moves the icon and the label onto the positions of the list's own items.
// Measured after layout, since the item's template adds its own offsets.
void AlignWithListItems(Controls::ListViewBase const& list) {
    if (!g_ui.styled || g_ui.aligned || g_ui.alignTries >= 10 ||
        g_ui.root.Visibility() != Visibility::Visible ||
        g_ui.icon.ActualWidth() <= 0 || g_ui.labelText.ActualWidth() <= 0) {
        return;
    }

    float iconCenterX;
    float textX;
    if (!FindItemLayout(list, &iconCenterX, &textX)) {
        return;
    }

    g_ui.alignTries++;

    float ourIconCenterX =
        LeftInList(g_ui.icon, list) + (float)g_ui.icon.ActualWidth() / 2;
    float ourTextX = LeftInList(g_ui.labelText, list);

    float iconDelta = iconCenterX - ourIconCenterX;
    float textDelta = textX - ourTextX;
    if (std::abs(iconDelta) < 0.5f && std::abs(textDelta) < 0.5f) {
        g_ui.aligned = true;
        return;
    }

    auto margin = g_ui.icon.Margin();
    float iconMargin = std::max(0.0f, (float)margin.Left + iconDelta);
    g_ui.icon.Margin(ThicknessHelper::FromLengths(iconMargin, 0, 0, 0));
    g_ui.content.ColumnSpacing(std::max(
        0.0f, (float)g_ui.content.ColumnSpacing() + textDelta - iconDelta));
}

void DetachPidUi() {
    auto list = g_ui.list.get();

    if (list && g_ui.layoutUpdatedToken) {
        list.LayoutUpdated(g_ui.layoutUpdatedToken);
    }

    if (g_ui.mode == AttachMode::Header) {
        if (list && list.Header() == g_ui.root) {
            list.Header(nullptr);
        }
    } else if (g_ui.mode == AttachMode::Panel && g_ui.parentPanel) {
        auto children = g_ui.parentPanel.Children();
        uint32_t index = 0;
        if (g_ui.wrapper) {
            // Put the list back in place of the wrapper.
            if (list && children.IndexOf(g_ui.wrapper, index)) {
                g_ui.wrapper.Children().Clear();
                children.RemoveAt(index);
                Controls::Grid::SetRow(list,
                                       Controls::Grid::GetRow(g_ui.wrapper));
                Controls::Grid::SetColumn(
                    list, Controls::Grid::GetColumn(g_ui.wrapper));
                Controls::Grid::SetRowSpan(
                    list, Controls::Grid::GetRowSpan(g_ui.wrapper));
                Controls::Grid::SetColumnSpan(
                    list, Controls::Grid::GetColumnSpan(g_ui.wrapper));
                children.InsertAt(index, list);
            }
        } else if (children.IndexOf(g_ui.root, index)) {
            children.RemoveAt(index);
        }
    }

    g_ui.mode = AttachMode::None;
    g_ui.list = nullptr;
    g_ui.layoutUpdatedToken = {};
    g_ui.layoutCount = 0;
    g_ui.parentPanel = nullptr;
    g_ui.wrapper = nullptr;
}

// Must be called on the thread that created the elements, see g_uiThreadId.
void RemovePidUi() {
    DetachPidUi();

    if (g_ui.root && g_ui.tappedToken) {
        g_ui.root.Tapped(g_ui.tappedToken);
    }
    if (g_ui.root && g_ui.keyDownToken) {
        g_ui.root.KeyDown(g_ui.keyDownToken);
    }

    g_ui.tappedToken = {};
    g_ui.keyDownToken = {};
    g_ui.root = nullptr;
    g_ui.icon = nullptr;
    g_ui.content = nullptr;
    g_ui.labelText = nullptr;
    g_ui.styled = false;
    g_ui.aligned = false;
    g_ui.alignTries = 0;
    g_ui.appliedSeq = 0;
    g_uiThreadId = 0;
}

bool AttachPidUiToPanel(Controls::ListViewBase list) {
    auto parentPanel =
        Media::VisualTreeHelper::GetParent(list).try_as<Controls::Panel>();
    if (!parentPanel) {
        return false;
    }

    auto children = parentPanel.Children();
    uint32_t index = 0;
    if (!children.IndexOf(list, index)) {
        return false;
    }

    auto stackPanel = parentPanel.try_as<Controls::StackPanel>();
    if (stackPanel &&
        stackPanel.Orientation() == Controls::Orientation::Vertical) {
        children.InsertAt(index, g_ui.root);
    } else {
        // Wrap the list and the item with a panel which takes the place of the
        // list.
        Controls::StackPanel wrapper;
        Controls::Grid::SetRow(wrapper, Controls::Grid::GetRow(list));
        Controls::Grid::SetColumn(wrapper, Controls::Grid::GetColumn(list));
        Controls::Grid::SetRowSpan(wrapper, Controls::Grid::GetRowSpan(list));
        Controls::Grid::SetColumnSpan(wrapper,
                                      Controls::Grid::GetColumnSpan(list));

        children.RemoveAt(index);
        wrapper.Children().Append(g_ui.root);
        wrapper.Children().Append(list);
        children.InsertAt(index, wrapper);

        g_ui.wrapper = wrapper;
    }

    g_ui.parentPanel = parentPanel;
    g_ui.mode = AttachMode::Panel;
    return true;
}

void AttachPidUi(Controls::ListViewBase list) {
    g_ui.list = list;

    if (!g_headerUnsupported && !list.Header()) {
        list.Header(g_ui.root);
        g_ui.mode = AttachMode::Header;
        g_ui.layoutCount = 0;
        g_ui.layoutUpdatedToken =
            list.LayoutUpdated([](auto&&, auto&&) { g_ui.layoutCount++; });
        Wh_Log(L"Attached as list header");
        return;
    }

    if (AttachPidUiToPanel(list)) {
        Wh_Log(L"Attached to the parent panel (%s)",
               winrt::get_class_name(g_ui.parentPanel).c_str());
    } else {
        Wh_Log(L"Couldn't attach, parent is %s",
               winrt::get_class_name(Media::VisualTreeHelper::GetParent(list))
                   .c_str());
    }
}

Controls::ListViewBase FindSystemItemList(DependencyObject object, int depth) {
    if (!object || depth > 24) {
        return nullptr;
    }

    if (winrt::get_class_name(object) == L"JumpViewUI.SystemItemListView") {
        return object.try_as<Controls::ListViewBase>();
    }

    int count = Media::VisualTreeHelper::GetChildrenCount(object);
    for (int i = 0; i < count; i++) {
        if (auto found = FindSystemItemList(
                Media::VisualTreeHelper::GetChild(object, i), depth + 1)) {
            return found;
        }
    }

    return nullptr;
}

Controls::ListViewBase FindSystemItemListInCurrentWindow() {
    auto window = Window::Current();
    if (!window) {
        return nullptr;
    }

    if (auto found = FindSystemItemList(window.Content(), 0)) {
        return found;
    }

    for (auto popup : Media::VisualTreeHelper::GetOpenPopups(window)) {
        if (auto found = FindSystemItemList(popup.Child(), 0)) {
            return found;
        }
    }

    return nullptr;
}

// Returns true if the current thread hosts the jump list.
bool ApplyOnUiThread() {
    auto list = FindSystemItemListInCurrentWindow();
    if (!list) {
        return false;
    }

    // The elements can't be touched from another thread, and can't be released
    // from it either. Leave them as they are, rather than crash.
    DWORD uiThreadId = g_uiThreadId;
    if (uiThreadId && uiThreadId != GetCurrentThreadId()) {
        Wh_Log(L"Jump list is on thread %u, the PID UI is on thread %u",
               GetCurrentThreadId(), uiThreadId);
        return false;
    }

    // The header is not shown by this list's template, use the other way.
    if (g_ui.mode == AttachMode::Header && g_ui.layoutCount >= 3 &&
        !Media::VisualTreeHelper::GetParent(g_ui.root)) {
        Wh_Log(L"List header isn't displayed, switching to panel mode");
        g_headerUnsupported = true;
        DetachPidUi();
    }

    bool attached = g_ui.mode != AttachMode::None && g_ui.list.get() == list &&
                    (g_ui.mode != AttachMode::Header ||
                     list.Header() == g_ui.root);
    if (!attached) {
        if (g_ui.root) {
            DetachPidUi();
        } else {
            CreatePidUi();
        }

        AttachPidUi(list);
        g_ui.appliedSeq = 0;
    }

    StyleLikeListItems(list);

    if (g_ui.appliedSeq != g_info.seq || g_labelChanged.exchange(false)) {
        g_ui.appliedSeq = g_info.seq;
        g_ui.labelText.Text(g_label);
        g_ui.root.Visibility(g_info.count ? Visibility::Visible
                                          : Visibility::Collapsed);
    }

    AlignWithListItems(list);

    return true;
}

bool ApplyOnWindowThread(HWND hWnd) {
    bool applied = false;
    RunFromWindowThread(
        hWnd,
        [](void* param) {
            try {
                *(bool*)param = ApplyOnUiThread();
            } catch (winrt::hresult_error const& e) {
                Wh_Log(L"Error %08X: %s", (UINT)e.code(), e.message().c_str());
            } catch (...) {
                Wh_Log(L"Unknown error");
            }
        },
        &applied);
    return applied;
}

std::vector<HWND> GetCoreWindows() {
    std::vector<HWND> windows;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto& windows = *(std::vector<HWND>*)lParam;

            DWORD pid = 0;
            WCHAR className[64];
            DWORD threadId = GetWindowThreadProcessId(hWnd, &pid);
            if (!threadId || pid != GetCurrentProcessId() ||
                !GetClassName(hWnd, className, ARRAYSIZE(className)) ||
                _wcsicmp(className, L"Windows.UI.Core.CoreWindow") != 0) {
                return TRUE;
            }

            // One window per UI thread is enough.
            for (HWND other : windows) {
                if (GetWindowThreadProcessId(other, nullptr) == threadId) {
                    return TRUE;
                }
            }

            windows.push_back(hWnd);
            return TRUE;
        },
        (LPARAM)&windows);

    return windows;
}

bool ApplyToJumpList() {
    if (g_jumpListWnd && IsWindow(g_jumpListWnd) &&
        ApplyOnWindowThread(g_jumpListWnd)) {
        return true;
    }

    g_jumpListWnd = nullptr;

    for (HWND hWnd : GetCoreWindows()) {
        if (ApplyOnWindowThread(hWnd)) {
            g_jumpListWnd = hWnd;
            return true;
        }
    }

    return false;
}

LRESULT CALLBACK ShellWndProc(HWND hWnd,
                              UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam) {
    switch (uMsg) {
        case WM_COPYDATA: {
            auto* cds = (const COPYDATASTRUCT*)lParam;
            if (cds->dwData != kCopyDataMagic ||
                cds->cbData != sizeof(JumpListPidInfo)) {
                return FALSE;
            }

            auto* info = (const JumpListPidInfo*)cds->lpData;
            g_pidHostWnd = (HWND)(ULONG_PTR)info->hostWnd;
            g_info.count = std::min(info->count, kMaxPids);
            for (DWORD i = 0; i < g_info.count; i++) {
                g_info.pids[i] = info->pids[i];
            }
            g_info.seq++;

            // The jump list might be recreated when shown, keep applying for
            // a while.
            g_appliedForSeq = ApplyToJumpList();
            g_pollsLeft = kPollCount;
            SetTimer(hWnd, kPollTimerId, kPollIntervalMs, nullptr);
            return TRUE;
        }

        case WM_TIMER:
            if (wParam == kPollTimerId) {
                if (ApplyToJumpList()) {
                    g_appliedForSeq = true;
                }

                if (--g_pollsLeft <= 0) {
                    KillTimer(hWnd, kPollTimerId);
                    if (!g_appliedForSeq) {
                        Wh_Log(L"Jump list not found");
                    }
                }
            }
            return 0;

        case WM_CLOSE:
            KillTimer(hWnd, kPollTimerId);

            // Clean up on the thread which owns the elements.
            if (DWORD uiThreadId = g_uiThreadId) {
                for (HWND coreWnd : GetCoreWindows()) {
                    if (GetWindowThreadProcessId(coreWnd, nullptr) ==
                        uiThreadId) {
                        RunFromWindowThread(
                            coreWnd,
                            [](void*) {
                                try {
                                    RemovePidUi();
                                } catch (...) {
                                    Wh_Log(L"Removing the PID UI failed");
                                }
                            },
                            nullptr);
                        break;
                    }
                }
            }

            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

// The parameter is an event which is signaled once the window is created, or
// once it's known that it can't be.
DWORD WINAPI ShellThreadProc(LPVOID readyEvent) {
    WNDCLASS wc{};
    wc.lpfnWndProc = ShellWndProc;
    wc.hInstance = GetModInstance();
    wc.lpszClassName = kShellWndClass;
    if (!RegisterClass(&wc)) {
        Wh_Log(L"RegisterClass failed: %u", GetLastError());
        SetEvent((HANDLE)readyEvent);
        return 1;
    }

    HWND hWnd = CreateWindowEx(0, kShellWndClass, nullptr, 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hWnd) {
        Wh_Log(L"CreateWindowEx failed: %u", GetLastError());
        UnregisterClass(kShellWndClass, wc.hInstance);
        SetEvent((HANDLE)readyEvent);
        return 1;
    }

    g_shellWnd = hWnd;
    SetEvent((HANDLE)readyEvent);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_shellWnd = nullptr;
    UnregisterClass(kShellWndClass, wc.hInstance);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////

void LoadSettings() {
    PCWSTR label = Wh_GetStringSetting(L"label");
    if (label && *label) {
        wcsncpy_s(g_label, label, _TRUNCATE);
    } else {
        wcscpy_s(g_label, L"Copiar PID");
    }
    Wh_FreeStringSetting(label);
    g_labelChanged = true;
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    WCHAR path[MAX_PATH];
    if (GetModuleFileName(nullptr, path, ARRAYSIZE(path))) {
        PCWSTR name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        g_isShellExperienceHost =
            _wcsicmp(name, L"ShellExperienceHost.exe") == 0;
    }

    if (g_isShellExperienceHost) {
        HANDLE readyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!readyEvent) {
            return FALSE;
        }

        g_shellThread =
            CreateThread(nullptr, 0, ShellThreadProc, readyEvent, 0, nullptr);
        if (g_shellThread) {
            WaitForSingleObject(readyEvent, INFINITE);
        }

        CloseHandle(readyEvent);

        if (g_shellThread && !g_shellWnd) {
            WaitForSingleObject(g_shellThread, INFINITE);
            CloseHandle(g_shellThread);
            g_shellThread = nullptr;
        }

        return g_shellThread != nullptr;
    }

    return HookTaskbarDllSymbols();
}

void Wh_ModUninit() {
    Wh_Log(L">");

    if (g_isShellExperienceHost) {
        if (HWND shellWnd = g_shellWnd) {
            PostMessage(shellWnd, WM_CLOSE, 0, 0);
        }

        if (g_shellThread) {
            // No timeout: the mod's code must not be unloaded while the thread
            // is still running.
            WaitForSingleObject(g_shellThread, INFINITE);
            CloseHandle(g_shellThread);
            g_shellThread = nullptr;
        }

        return;
    }

    if (g_hostWnd) {
        SendMessage(g_hostWnd, WM_CLOSE, 0, 0);
        g_hostWnd = nullptr;
    }

    if (g_hostWndClassRegistered) {
        UnregisterClass(kHostWndClass, GetModInstance());
        g_hostWndClassRegistered = false;
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();
}
