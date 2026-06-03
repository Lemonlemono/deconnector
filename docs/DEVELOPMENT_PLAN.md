# Development Plan

## Goal

Build a lightweight Windows tool that lets the user choose a process, bind a keyboard or gamepad shortcut, and temporarily disconnect that process for a user-selected duration.

The first implementation is a native Win32 single-exe application using Windows Filtering Platform.

## Phase 1: WFP MVP

Status: implemented.

- Native Win32 GUI
- Process list with PID and executable path
- Saved target process path
- Locked target mode with automatic reselection after process restart
- Saved presets for quick target switching
- Transparent countdown overlay with configurable screen position
- Two independent disconnect actions with separate duration, block mode, and binding
- Global keyboard hotkey binding per action
- XInput and Raw Input HID gamepad button binding per action
- Temporary WFP filters for a configurable number of seconds
- Dynamic WFP session for cleanup on process exit
- Lightweight local install script

## Phase 2: Usability

- System tray mode
- Optional start on login
- Better hotkey/gamepad editor
- Device-specific presets for gamepads with unusual HID layouts
- Search/filter process list
- Visual countdown in tray tooltip
- One-click clear saved config

## Phase 3: Reliability

- Add structured logs under `%LOCALAPPDATA%\Deconnector\logs`
- Add guard against duplicate app instances
- Add a recovery action to remove Deconnector WFP filters
- Add integration test notes using a local HTTP/UDP test app

## Phase 4: Stronger Disconnect Mode

The WFP MVP works at ALE authorization layers and is intentionally simple. If strict packet-level behavior is needed later, add a second engine:

- WFP callout driver, or
- WinDivert-based packet dropper

That mode should remain optional because it increases install friction and may trigger endpoint security tools.

## Distribution

Recommended MVP distribution:

1. Build `Release|x64`.
2. Zip `Deconnector.exe`, `install.ps1`, and `uninstall.ps1`.
3. On the target PC, run `install.ps1`.
4. Launch from Start Menu and approve the UAC prompt.

For a polished installer later:

- MSIX if enterprise deployment is desired
- Inno Setup if simple consumer installation is desired
- Signed exe/installer before wider distribution
