# Lask Manager — project notes for Claude

Lightweight Task Manager replacement for Windows (`lask.exe`). C++23, raw Win32 + common controls,
CMake, MSVC. The design is `docs/design.md`; read it before changing architecture. The milestone
order is in its "Build order" section; we are on milestone 1 (kill-only MVP).

## Build and run

```powershell
cmake --preset vs                     # Visual Studio 18 2026 generator, x64
cmake --build --preset vs-release     # or vs-debug
.\build\vs\Release\lask.exe
```

The C++ standard flag is `/std:c++23preview` (MSVC 14.51). It still works on 14.52+, which also
accepts `/std:c++23`. Static CRT. Warnings at /W4; keep the build warning-free.

## Invariants (do not break these)

- The UI thread never waits on the kernel or on a worker. Process enumeration, OpenProcess,
  TerminateProcess and anything that can stall runs on a worker; results come back by PostMessage.
- One `NtQuerySystemInformation(SystemProcessInformation)` call per tick is the only per-tick
  kernel work. No WMI, no PDH process counters, no Toolhelp32, no EnumProcesses loop, no
  OpenProcess on the hot path. Per-process facts (icons, descriptions, user, command line) are
  lazy, cached, and fetched only for rows on screen.
- A process is identified by `(pid, createTime)`, never by pid alone. Kills verify the create time.
- Snapshots are immutable once published (`std::atomic<std::shared_ptr<const Snapshot>>`). The UI
  keeps the newest and the previous one; rates are `(current - previous) / elapsed`.
- Sort never moves a row while the mouse button is down or a menu is open (`IsInteracting()`).
- The grid is a `ListView` in `LVS_OWNERDATA` mode. Colors go through documented APIs
  (`ListView_SetBkColor`, `NM_CUSTOMDRAW`). Only the performance graphs are hand-drawn (Direct2D,
  not yet written). No XAML, no WinUI, no .NET, no WinRT activation.
- Manifest stays `asInvoker`. Elevation is a relaunch with `--kill <pid> --at <createtime>`.
- Undocumented calls (NT structs in `snapshot.cpp`, uxtheme ordinals in `theme.cpp`) stay isolated
  in those two files and fail soft.

## Conventions

- Files: `snapshot.*` (data), `theme.*` (look), `main.cpp` (window). New tabs get their own file.
- Namespace `lask`. `g` in main.cpp is the single UI-thread state object; nothing else touches it.
- UTF-16 everywhere (`UNICODE`), `swprintf_s` into fixed buffers on the paint path; no
  `std::wstring` allocation per row per tick.
- Colors come from `lask::Theme`; never hard-code a COLORREF outside `theme.cpp`.
- Measure startup when touching anything on the startup path: `Measure-Command { .\lask.exe }`
  under a CPU + memory hog. Target: under 300 ms idle, under 1.5 s loaded.

## Known cosmetic items

- The themed ListView draws faint vertical column separators in report view (both `Explorer` and
  `ItemsView` themes do). Windows 10's Task Manager had them too; decide later whether to suppress.
- Selection color is the theme's, not accent-tinted yet (`Theme::selection` exists for when
  `NM_CUSTOMDRAW` on the list lands with the heat-map cells).

## Not yet done (from the design)

Processes tab grouping, Performance graphs (Direct2D child window + 300-sample ring buffers,
30 s / 1 min / 5 min views), Details columns from the enrichment thread, Services tab, End process
tree, Open file location, tray-resident mode, IFEO "replace Task Manager" option, `--bench` flag,
I/O priority boost via `NtSetInformationProcess(ProcessIoPriority)`.
