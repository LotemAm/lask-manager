# Lask Manager — project notes for Claude

Lightweight Task Manager replacement for Windows (`lask.exe`). C++23, raw Win32 + common controls,
CMake, MSVC. The design is `docs/design.md`; read it before changing architecture. The milestone
order is in its "Build order" section; milestones 1-3 (kill-only MVP, theme, enrichment) are done
and milestone 4 (Processes tab) is next.

## Build and run

```powershell
cmake --preset vs                     # Visual Studio 18 2026 generator, x64
cmake --build --preset vs-release     # or vs-debug
.\build\vs\Release\lask.exe
```

The C++ standard flag is `/std:c++23preview` (MSVC 14.51). It still works on 14.52+, which also
accepts `/std:c++23`. Static CRT. Warnings at /W4; keep the build warning-free.

If `cmake --preset vs` says the VS instance "is not known to the Visual Studio Installer", an
installer update is running; build from `vcvars64.bat` with `cmake --preset ninja` meanwhile.

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
- Undocumented calls (NT structs and info classes in `snapshot.cpp`, uxtheme ordinals in
  `theme.cpp`) stay isolated in those two files and fail soft. `facts.cpp` reaches the NT ones only
  through `QueryCommandLine` / `QueryImageNtPath`.
- Enrichment (`lask::Enricher`) starts after the second snapshot, runs below normal, and only
  fetches what the UI's wanted set names (visible rows + 16 lookahead). It owns its caches; the UI
  keeps its own `Identity -> FactEntry` map, filled by posted `FactsBatch`es. The thread is detached
  on purpose so a hung `LookupAccountSid` cannot block exit.

## Conventions

- Files: `snapshot.*` (data), `facts.*` (lazy per-process facts, enrichment thread), `theme.*`
  (look), `main.cpp` (window). New tabs get their own file.
- Namespace `lask`. `g` in main.cpp is the single UI-thread state object; nothing else touches it.
- UTF-16 everywhere (`UNICODE`), `swprintf_s` into fixed buffers on the paint path; no
  `std::wstring` allocation per row per tick.
- Colors come from `lask::Theme`; never hard-code a COLORREF outside `theme.cpp`.
- Measure startup when touching anything on the startup path: `Measure-Command { .\lask.exe }`
  under a CPU + memory hog. Target: under 300 ms idle, under 1.5 s loaded.

## Win32 traps already hit

- The main window needs `WS_CLIPCHILDREN`: without it, its background erase paints over the list
  and the rows stay blank until something invalidates the list again.
- ListView sub-item indices must stay dense (`< column count`); a column whose `iSubItem` is past
  the count blanks the whole grid. Hence `App::columns` and the rebuild in `ToggleColumn`.
- Selected-row colors: clear `CDIS_SELECTED | CDIS_HOT | CDIS_FOCUS` in `CDDS_ITEMPREPAINT` and set
  `clrTextBk`; that fills every cell, icon included, so no manual `FillRect` is needed.

## Known cosmetic items

- The themed ListView draws faint vertical column separators in report view (both `Explorer` and
  `ItemsView` themes do). Windows 10's Task Manager had them too; decide later whether to suppress.
- The filter matches descriptions, users and paths only for processes whose facts were already
  fetched (i.e. that have been on screen); names and PIDs always match.
- Light theme and high contrast are implemented but have only been checked by reading the code.

## Not yet done (from the design)

Processes tab grouping, Performance graphs (Direct2D child window + 300-sample ring buffers,
30 s / 1 min / 5 min views), Services tab, End process tree, Open file location, "Not responding"
status, column layout persistence, tray-resident mode, IFEO "replace Task Manager" option,
`--bench` flag, I/O priority boost via `NtSetInformationProcess(ProcessIoPriority)`.
