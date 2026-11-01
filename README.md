# Taskbar Jump List Copy PID

A Windhawk mod that adds a "Copy PID" item to the jump list shown when you right-click a taskbar button on Windows 11 (64-bit). Clicking it copies the process ID of the app to the clipboard. If the app has windows in several processes, all of their PIDs are copied, separated by a comma and a space.

The item label defaults to "Copy PID". It can be changed in the mod settings, under Item label.

## Install

Windhawk 1.7 has no "Install from file" action. The mod is compiled from this source:

1. Install [Windhawk](https://windhawk.net/).
2. Open Settings and turn on Developer mode.
3. Click Create a New Mod. If Windhawk asks before opening the editor, choose Begin coding.
4. Replace the template with the contents of `taskbar-jump-list-copy-pid.wh.cpp`. Leave the mod id as `taskbar-jump-list-copy-pid`.
5. Click Compile Mod.
6. Enable the mod.

## Use

Right-click a taskbar button and click Copy PID. The jump list closes after the PIDs are on the clipboard, for example `1234` or `1234, 5678`.

An elevated Win32 window still contributes its PID. A packaged app contributes the application process, not `ApplicationFrameHost.exe`, when that process can be identified.

## Remove

Disable or remove the mod from its page in Windhawk. No system files are changed.

## License

GPL-3.0-only. See [LICENSE](LICENSE).

The source is based on the "Taskbar Jump List Volume Control" mod by Ara and the "Taskbar Volume Control Per-App" mod by m417z.
