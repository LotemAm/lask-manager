# Lask Manager

A lightweight Task Manager for Windows: opens instantly on a machine that is already thrashing, and
ends the process you point at. One small native exe, no runtime, four tabs (Processes, Performance,
Details, Services) and nothing else.

Why: the Windows 11 Task Manager is a XAML app that opens every process on startup. When the PC is
starved it takes seconds to appear, makes things worse while it is up, and re-sorts the row you are
about to right-click. `lask.exe` takes one kernel snapshot per tick, never blocks its UI thread, and
freezes the sort while your mouse is down. The full reasoning is in [docs/design.md](docs/design.md).

## Status

Milestone 1 (kill-only MVP): flat process grid with name / PID / CPU / memory / threads / handles,
sort by any column, Delete ends the selected process, restart-as-administrator when Windows says
no, light/dark theme and the user's accent color. Everything else in the design is still to come;
see the build order in the design doc.

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
| Esc | Close |
| Typing | Jump to the next process whose name starts with what you typed |
| Shift+F10 / Menu | Context menu |

## Layout

```
src/main.cpp      window, grid, context menu, kill flow
src/snapshot.*    one NtQuerySystemInformation call per tick; sampler thread; terminate helpers
src/theme.*       light/dark + accent from the registry, dark title bar, uxtheme dark-mode ordinals
src/app.manifest  asInvoker, common controls v6, per-monitor v2 DPI
docs/design.md    the system design this follows
```
