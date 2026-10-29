<h1><img src="res/lask.svg" alt="" width="44" height="44" align="center"> Lask Manager</h1>

A lightweight Task Manager for Windows: opens instantly on a machine that is already thrashing, and
ends the process you point at. One small native exe, no runtime, four tabs (Processes, Performance,
Details, Services) and nothing else.

Why: the Windows 11 Task Manager is a XAML app that opens every process on startup. When the PC is
starved it takes seconds to appear, makes things worse while it is up, and re-sorts the row you are
about to right-click. `lask.exe` takes one kernel snapshot per tick, never blocks its UI thread, and
freezes the sort while your mouse is down. The full reasoning is in [docs/design.md](docs/design.md).

## Settings

`%LOCALAPPDATA%\LaskManager\settings.json`, or `lask.json` next to `lask.exe` if that file exists
(portable use: create an empty `{}` there). Delete it to get the defaults back.

## Replace Task Manager

Options > Replace Task Manager sets
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe`
`Debugger` to this exe (administrator rights; you get a UAC prompt). Windows then starts Lask
Manager for Ctrl+Shift+Esc, the Ctrl+Alt+Del screen and the taskbar menu. Turn it off before you
move or delete `lask.exe`. To undo it by hand from an elevated prompt:

```powershell
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe" /v Debugger /f
```

## Command line

| Argument | Meaning |
| --- | --- |
| `--bench` | Print time to first paint and first rows (ms from process start), then exit |
| `--tray` | Start hidden in the notification area (what "start with Windows" uses) |
| `--kill <pid> --at <createtime>`, `--service <start\|stop\|restart> "<name>"`, `--replace-taskmgr on\|off` | Used by the elevated relaunch |

## Build

Requirements: Visual Studio 2026 (MSVC 14.51 or later) with the C++ desktop workload, CMake 3.28+.

```powershell
cmake --preset vs
cmake --build --preset vs-release
.\build\vs\Release\lask.exe
```

From a developer prompt, `cmake --preset ninja` + `cmake --build --preset ninja-release` is faster.
`.\scripts\package.ps1` builds a release and writes the zip to `dist\` (no installer: copy the exe).

## Keyboard

| Key | Action |
| --- | --- |
| Delete | End the selected process |
| F5 | Refresh now |
| Ctrl+F | Filter by name, PID, user, description, path or command line |
| Ctrl+Tab, Ctrl+1 ... Ctrl+4 | Next tab; Processes, Performance, Details, Services |
| Ctrl+, | Options |
| Ctrl+Plus / Ctrl+Minus, Ctrl+wheel | Zoom the UI in / out (100% to 200%) |
| Ctrl+0 | Reset the UI zoom to 100% |
| Win+Shift+Esc | Show or hide (when kept in the notification area) |
| Right / Left | Expand / collapse an app row (Processes) |
| Enter, double-click | Expand an app row, or switch to a window row (Processes) |
| Tab | Move between the filter box and the grid |
| Esc | Clear the filter; with no filter, close (or hide, in the notification area) |
| Typing | Jump to the next process whose name starts with what you typed |
| Shift+F10 / Menu | Context menu |

## Layout

```
src/main.cpp      window, navigation, Details grid, filter box, column picker, actions
src/processes.*   Processes tab: grouping, folding, expand/collapse, heat-map cells
src/performance.* Performance tab: panel list with sparklines, detail pane, stats
src/graph.*       the Direct2D graph control (d2d1/dwrite loaded on first use)
src/history.*     ring buffers and per-device panels, fed every tick on the UI thread
src/counters.*    CPU, memory, disk, network and GPU counters, read by the sampler
src/services.*    service list, per-service config fetch, start / stop / restart
src/settings.*    the settings file (flat JSON)
src/shell.*       tray icon, start with Windows, the "replace Task Manager" IFEO hook
scripts/package.ps1  release zip
src/servicestab.* Services tab: grid with status dots, sort, filter
src/ui.*          shared grid chrome: fonts, dark header, accent selection, filter match
src/snapshot.*    one NtQuerySystemInformation call per tick; sampler thread; terminate helpers
src/facts.*       enrichment thread: icons, descriptions, users, paths, command lines, on demand
src/theme.*       light/dark + accent from the registry, dark title bar, uxtheme dark-mode ordinals
src/app.manifest  asInvoker, common controls v6, per-monitor v2 DPI
docs/design.md    the system design this follows
```
