# Lask Manager

A lightweight Task Manager for Windows: opens instantly on a machine that is already thrashing, and
ends the process you point at. One small native exe, no runtime, four tabs (Processes, Performance,
Details, Services) and nothing else.

Why: the Windows 11 Task Manager is a XAML app that opens every process on startup. When the PC is
starved it takes seconds to appear, makes things worse while it is up, and re-sorts the row you are
about to right-click. `lask.exe` takes one kernel snapshot per tick, never blocks its UI thread, and
freezes the sort while your mouse is down. The full reasoning is in [docs/design.md](docs/design.md).

## Status

Milestones 1-4 of the design's build order:

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

The Performance and Services tabs are still to come; see the build order in the design doc.

## Build

Requirements: Visual Studio 2026 (MSVC 14.51 or later) with the C++ desktop workload, CMake 3.28+.

```powershell
cmake --preset vs
cmake --build --preset vs-release
.\build\vs\Release\lask.exe
```

From a developer prompt, `cmake --preset ninja` + `cmake --build --preset ninja-release` is faster.

## Keyboard

| Key | Action |
| --- | --- |
| Delete | End the selected process |
| F5 | Refresh now |
| Ctrl+F | Filter by name, PID, user, description, path or command line |
| Ctrl+Tab, Ctrl+1, Ctrl+2 | Next tab, Processes, Details |
| Right / Left | Expand / collapse an app row (Processes) |
| Enter, double-click | Expand an app row, or switch to a window row (Processes) |
| Tab | Move between the filter box and the grid |
| Esc | Clear the filter; with no filter, close |
| Typing | Jump to the next process whose name starts with what you typed |
| Shift+F10 / Menu | Context menu |

## Layout

```
src/main.cpp      window, navigation, Details grid, filter box, column picker, actions
src/processes.*   Processes tab: grouping, folding, expand/collapse, heat-map cells
src/ui.*          shared grid chrome: fonts, dark header, accent selection, filter match
src/snapshot.*    one NtQuerySystemInformation call per tick; sampler thread; terminate helpers
src/facts.*       enrichment thread: icons, descriptions, users, paths, command lines, on demand
src/theme.*       light/dark + accent from the registry, dark title bar, uxtheme dark-mode ordinals
src/app.manifest  asInvoker, common controls v6, per-monitor v2 DPI
docs/design.md    the system design this follows
```
