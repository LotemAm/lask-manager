# Lask Manager

A lightweight Task Manager for Windows: opens instantly on a machine that is already thrashing, and
ends the process you point at. One small native exe, no runtime, four tabs (Processes, Performance,
Details, Services) and nothing else.

Why: the Windows 11 Task Manager is a XAML app that opens every process on startup. When the PC is
starved it takes seconds to appear, makes things worse while it is up, and re-sorts the row you are
about to right-click. `lask.exe` takes one kernel snapshot per tick, never blocks its UI thread, and
freezes the sort while your mouse is down. The full reasoning is in [docs/design.md](docs/design.md).

## Status

All seven milestones of the design's build order:

- **Kill-only MVP.** Flat process grid, sort by any column, Delete ends the selected process,
  restart-as-administrator when Windows says no.
- **Theme.** Light/dark and the accent color from the registry, accent shades derived in HSL,
  accent-tinted selection and sorted-column header, dark title bar and menus, high-contrast
  support, live updates when any of these change.
- **Enrichment.** Icons, description, user, image path, command line, elevation and architecture,
  fetched on a below-normal worker only for rows on screen. Status (Running / Suspended) and
  priority come from the snapshot. Column picker (right-click the header) and a filter box.
- **Processes tab.** Apps / Background processes / Windows processes, a program and its
  same-named children folded into one expandable row that also lists its windows, heat-shaded
  CPU, memory and disk cells, Not responding / Suspended status, End task (the whole app), End
  process tree, Open file location, Go to details, Switch to. Left navigation between tabs.

- **Performance tab.** CPU (overall or one graph per logical processor), memory, every physical
  disk (active time, read/write rate, response time), every network adapter (send/receive), every
  GPU (busiest engine, per-engine %, dedicated and shared memory). Direct2D graphs over 30 s,
  1 min or 5 min (right-click the graph), sparklines in the panel list, history from startup.

- **Services tab.** Every Win32 service with PID, display name, status dot, startup type
  (including delayed and trigger start), svchost group and description; sortable and filterable.
  Start, Stop, Restart (with the UAC shield, and an offer to restart elevated and finish the action
  there when Windows says no), Go to details, Open Services.

- **Polish.** Options (gear button, Ctrl+,): always on top; update speed 0.5 s / 1 s / 2 s /
  paused; hardware-accelerated graphs (smoother, ~20 MB more memory); keep running in the
  notification area (starts with Windows, Win+Shift+Esc shows it, closing hides it, sampling
  pauses while hidden); replace Task Manager. Window position, tab, column widths and order, sort
  and graph range are remembered. One instance: a second launch shows the running one. Set
  priority on the Processes and Details menus. High I/O priority for itself when elevated.

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
