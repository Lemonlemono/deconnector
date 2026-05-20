# Deconnector

Deconnector is a lightweight Windows desktop tool that blocks network access for a selected process for a user-selected number of seconds when a bound keyboard hotkey or gamepad button is pressed.

This first version uses Windows Filtering Platform (WFP) from user mode. It adds temporary dynamic filters for the selected executable path and removes them automatically after the timer expires.

## Features

- Native Win32 UI, no .NET runtime required
- Lists running processes with PID and executable path
- Saves the selected target process path
- Locks the target executable so it remains selected after the process exits and starts again
- Saves process presets for quick target switching
- Shows a transparent, borderless countdown overlay while disconnected
- Lets the user edit the overlay screen position
- Binds a global keyboard hotkey with `RegisterHotKey`
- Binds XInput and generic HID gamepad buttons
- Blocks the selected process for a configurable duration through WFP
- Uses a dynamic WFP session so filters are cleaned up if the process exits unexpectedly
- Stores config in `%LOCALAPPDATA%\Deconnector\config.ini`

## Requirements

- Windows 10 or newer
- Administrator privileges at runtime
- Visual Studio 2026 or Build Tools for Visual Studio 2026 to build

## Build

Open PowerShell in this directory:

```powershell
.\build.ps1
```

The release exe will be created at:

```text
build\x64\Release\Deconnector.exe
```

You can also open `Deconnector.sln` in Visual Studio and build the `Release|x64` configuration.

## Install Locally

After building:

```powershell
.\install.ps1
```

This copies the exe to:

```text
%LOCALAPPDATA%\Programs\Deconnector\Deconnector.exe
```

and creates a Start Menu shortcut.

## Usage

1. Start Deconnector as administrator.
2. Select a process in the list.
3. Click `Save preset` if you want to reuse that target later.
4. Use the preset dropdown to quickly switch saved targets.
5. Keep `Lock target` checked if you want that executable to stay selected after restarts.
6. Set the number of seconds and the overlay X/Y screen position.
7. Click `Bind Hotkey`.
8. Press the desired keyboard combination, for example `Ctrl + Alt + F8`, or press a connected gamepad button.
9. Press the binding or click `Disconnect`.

## Gamepad Binding

The MVP supports XInput-compatible controllers and generic HID gamepad/joystick devices through Raw Input. Some vendor-specific devices may expose unusual HID layouts; those can be added case-by-case without changing the WFP engine.

## MVP Limitations

This WFP MVP blocks by executable path at ALE authorization layers. It is reliable for new connections and many normal app network operations, but it may not instantly interrupt every already-established flow in all applications. A later packet-level engine can add stricter drop behavior if needed.

Use this only on computers and processes you own or are authorized to test.
