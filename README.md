# PhoneLinkLock

Launches Windows **Phone Link**, forces its window fullscreen and on top,
strips the close/minimize controls, and holds that lock until UI Automation
detects text in the window indicating the phone has linked — then hands
control back and lets the window behave normally.

## Build (Windows, Visual Studio + CMake)

Easiest: run the included build script from a regular or "Developer"
PowerShell/Command Prompt (it auto-detects VS2022/VS2019 and needs `cmake`
on PATH):

```powershell
.\build.ps1            # Release build (default)
.\build.ps1 -Config Debug
```

or, from `cmd.exe`:

```bat
build.bat
build.bat Debug
```

Both scripts just wrap the manual steps below:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build\Release\PhoneLinkLock.exe` — a single, statically
CRT-linked executable (see the `MSVC_RUNTIME_LIBRARY` setting in
`CMakeLists.txt`), so it doesn't need the VC++ redistributable installed on
the target machine.

No console window appears (it's built with `add_executable(... WIN32 ...)`);
you'll see a system-tray icon instead, which you can right-click to exit.

## Why this isn't a Windows *Service*

A real Windows Service runs in Session 0, isolated from your desktop —
since Vista, services can't draw or manipulate windows in your interactive
session at all. So a background process that needs to touch Phone Link's
window has to run **in your user session**, not as SYSTEM. The Windows
equivalent of "daemon that starts at boot and respawns on failure" for this
kind of task is a **Task Scheduler task set to run at logon**, which is
what's below.

## Autostart as a "daemon" via Task Scheduler

Run this once (PowerShell, as your normal user — no admin needed unless you
want it to run for all users):

```powershell
$exe = "C:\Path\To\PhoneLinkLock.exe"

$action  = New-ScheduledTaskAction -Execute $exe
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet `
    -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -DontStopOnIdleEnd

Register-ScheduledTask -TaskName "PhoneLinkLock" `
    -Action $action -Trigger $trigger -Settings $settings `
    -Description "Locks Phone Link fullscreen until the phone is linked"
```

This gives you the daemon-like properties you asked for:
- starts automatically at logon
- `-RestartCount`/`-RestartInterval` restart it if it crashes
- runs invisibly (tray icon only, no console/taskbar window)

To remove it later: `Unregister-ScheduledTask -TaskName "PhoneLinkLock" -Confirm:$false`

## Tuning notes

- **`kLinkedKeywords`** in `src/main.cpp` is the list of substrings the app
  looks for in Phone Link's UI tree to decide "linked". Phone Link's actual
  wording varies by version/region/locale — run the app once, watch what
  text appears once your phone connects, and adjust the list.
- **`kBlockEscapeHotkeys`** toggles the low-level keyboard hook that blocks
  Win+D, Win+M, Alt+F4, and Alt+Tab while locked. This is intrusive by
  design (that's what you asked for); set it to `false` if you just want
  the fullscreen/no-close-button behavior without hooking global keys.
- Task Manager can always end the process — no Win32-level lock can prevent
  that, short of a kernel driver, which is well outside what this project
  does.
