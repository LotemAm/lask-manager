# Lask Manager — project notes for Claude

Lightweight Task Manager replacement for Windows (`lask.exe`). C++23, raw Win32 + common controls,
CMake, MSVC. The design is `docs/design.md`; read it before changing architecture. The milestone
order is in its "Build order" section; all seven milestones are done. What is left is listed under
"Not yet done" at the end of this file.

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

`lask.exe --bench` prints `first-paint-ms=… first-rows-ms=…` (from process creation) and exits;
`scripts\package.ps1 [-Preset ninja]` builds, benches and writes `dist\LaskManager-<ver>-x64.zip`.

Testing: put a `lask.json` (`{}` is enough) next to the exe under test. That is portable mode: the
settings go there instead of `%LOCALAPPDATA%\LaskManager\settings.json`, so tests never touch the
user's real settings. Remove it afterwards. A test that closes the app normally saves settings.
`{"theme": 1}` (light) or `2` (dark) in that `lask.json` overrides the Windows app mode, so either
look can be tested without changing the user's Windows setting. Esc in a grid closes the app (as Task
Manager's does): a test that sends Esc to dismiss a menu that already closed ends the run.

## Invariants (do not break these)

- The UI thread never waits on the kernel or on a worker. Process enumeration, OpenProcess,
  TerminateProcess and anything that can stall runs on a worker; results come back by PostMessage.
- One `NtQuerySystemInformation(SystemProcessInformation)` call per tick is the only per-tick
  kernel work. No WMI, no PDH process counters, no Toolhelp32, no EnumProcesses loop, no
  OpenProcess on the hot path. Per-process facts (icons, descriptions, user, command line) are
  lazy, cached, and fetched only for rows on screen. One deliberate exception: grouping needs every
  process's image path, so processes with no facts yet get a path-only lookup by PID
  (`kFactPath`, `SystemProcessIdInformation`, no OpenProcess), once per process, after the
  visible rows. While the Processes tab is visible the sampler also does one `EnumWindows` per
  tick (titles via `InternalGetWindowText`, which never sends a message to a hung app).
- The sampler also reads the system counters every tick (`CounterReader`: per-CPU times, memory,
  `IOCTL_DISK_PERFORMANCE` per physical disk, `GetIfEntry2` per adapter, `D3DKMTQueryStatistics`
  per GPU node and segment), so the graphs have history from startup. Device discovery happens
  after the first snapshot is published; disks are rediscovered every 60 ticks, adapters every 5.
  Clock speed and `GetPerformanceInfo` only run while the Performance tab is visible.
- Services: the sampler calls `EnumServicesStatusEx` only while the Services tab is visible, every
  5th tick or at once on `refreshServices()` (tab opened, action started or finished); the list is
  a `shared_ptr<const ServiceList>` shared by snapshots in between. Configs (`QueryServiceConfig`,
  description, delayed / trigger start) are fetched once per service name on a detached
  below-normal thread (`LoadServiceConfigs`), not on the enrichment thread: they are keyed by
  name, not by process identity. Start / stop / restart run on throwaway workers.
- Elevation relaunch arguments: `--kill <pid> --at <createtime>`,
  `--service <start|stop|restart> "<name>"` (runs on a worker after the first paint, with the
  Services tab showing) and `--replace-taskmgr on|off` (writes the IFEO key, then runs normally).
  `RestartElevatedWith` sets `exiting` so the old instance really closes, even in tray mode.
- One instance: a launch without those arguments (or `--bench`) finds `LaskManager.Main`, posts the
  registered `LaskManager.Activate` message and exits. The elevated instance allows that message
  through UIPI. Under IFEO this is how Ctrl+Shift+Esc reaches a running (tray) instance.
- Settings: read once before the window exists (one small file), saved synchronously in
  `WM_DESTROY` and on a detached worker after each option change. Hidden in the tray means
  paused: the sampler waits `INFINITE` until the window is shown (or F5).
- A process is identified by `(pid, createTime)`, never by pid alone. Kills verify the create time.
- Snapshots are immutable once published (`std::atomic<std::shared_ptr<const Snapshot>>`). The UI
  keeps the newest and the previous one; rates are `(current - previous) / elapsed`.
- Sort never moves a row while the mouse button is down or a menu is open (`IsInteracting()`).
- The grid is a `ListView` in `LVS_OWNERDATA` mode. Colors go through documented APIs
  (`ListView_SetBkColor`, `NM_CUSTOMDRAW`). Only the performance graphs are hand-drawn (Direct2D,
  `graph.cpp`). No XAML, no WinUI, no .NET, no WinRT activation.
- Nothing outside the first screen's needs is imported at startup: d2d1/dwrite (graph.cpp),
  iphlpapi/powrprof (counters.cpp) are `LoadLibraryEx`'d on first use, and gdi32's D3DKMT
  functions are looked up with `GetProcAddress` so a missing export fails soft. Check with
  `dumpbin /dependents` after adding a library.
- Manifest stays `asInvoker`. Elevation is always a relaunch (see the arguments above). Elevated,
  we enable SeDebugPrivilege and SeIncreaseBasePriorityPrivilege, which also lets
  `RaiseOwnIoPriority` set our own I/O priority to High (it fails soft unelevated).
- Undocumented calls (NT structs and info classes in `snapshot.cpp`, uxtheme ordinals in
  `theme.cpp`) stay isolated in those two files and fail soft. `facts.cpp` reaches the NT ones only
  through `QueryCommandLine` / `QueryImageNtPath`.
- Enrichment (`lask::Enricher`) starts after the second snapshot, runs below normal, and only
  fetches what the UI's wanted set names (visible rows + 16 lookahead). It owns its caches; the UI
  keeps its own `Identity -> FactEntry` map, filled by posted `FactsBatch`es. The thread is detached
  on purpose so a hung `LookupAccountSid` cannot block exit.

## Conventions

- Files: `snapshot.*` (data), `counters.*` (system counters, sampler side), `history.*` (rings
  and the per-device panels, UI side), `facts.*` (lazy per-process facts, enrichment thread,
  UI-side `FactCache`), `theme.*` (look), `ui.*` (what every grid shares: `Chrome` fonts + palette,
  the dark header, accent selection, filter match), `graph.*` (the Direct2D graph control),
  `services.*` (service list, configs, actions), `settings.*` (the JSON settings file), `shell.*`
  (tray icon, start with Windows, the IFEO hook), `processes.*`, `performance.*` and
  `servicestab.*` (tabs), `main.cpp` (window, nav, Details tab, options, actions). New tabs get
  their own file, as a class that holds a `const Chrome&`.
- Namespace `lask`. `g` in main.cpp is the single UI-thread state object; nothing else touches it.
  Tabs get what they need by reference at construction (`Chrome`, `FactCache`, the filter buffer).
- Every tab rebuilds on every tick, visible or not, so no tab ever holds a `FactEntry*` across a
  `FactCache::sweep()`. The Details grid is created on first show (startup cost); until then it
  does no work at all. The Performance tab's windows are created on first show too, but
  `PerfModel` is fed from the first tick; graphs hold `Ring*` into panels, so
  `PerformanceTab::refresh()` re-points them every tick, visible or not. The Services grid is
  also created on first show.
- The rings live on the UI thread and are appended on adopt. A tick skipped while
  `IsInteracting()` folds into the next one (its rate covers both intervals).
- UTF-16 everywhere (`UNICODE`), `swprintf_s` into fixed buffers on the paint path; no
  `std::wstring` allocation per row per tick.
- Colors come from `lask::Theme`; never hard-code a COLORREF outside `theme.cpp`.
- Measure startup when touching anything on the startup path: `Measure-Command { .\lask.exe }`
  under a CPU + memory hog. Target: under 300 ms idle, under 1.5 s loaded. Each ListView costs
  7-12 ms to create, so don't create one the first screen doesn't show. The sampler starts in
  `WM_CREATE` so the first snapshot is ready the moment the empty window has painted.

## Win32 traps already hit

- The main window needs `WS_CLIPCHILDREN`: without it, its background erase paints over the list
  and the rows stay blank until something invalidates the list again.
- ListView sub-item indices must stay dense (`< column count`); a column whose `iSubItem` is past
  the count blanks the whole grid. Hence `App::columns` and the rebuild in `ToggleColumn`.
- Selected-row colors: clear `CDIS_SELECTED | CDIS_HOT | CDIS_FOCUS` in `CDDS_ITEMPREPAINT` and set
  `clrTextBk`; that fills every cell, icon included, so no manual `FillRect` is needed. With
  `CDRF_NOTIFYSUBITEMDRAW` this has to be repeated at the sub-item stage; `CLR_NONE` as a
  sub-item `clrTextBk` keeps the theme's hover highlight.
- `TerminateProcess` fails with access denied on a process that is already exiting (a parent that
  quit when its child was ended). With a `PROCESS_TERMINATE` handle in hand that means success.
- A one-column ListView (the nav) draws a column separator at the column's right edge unless the
  column is exactly as wide as the control. Even then the themed ListView draws the edge as a line
  in the empty area below the last row; the custom-drawn lists (nav, performance panels) paint it
  over at `CDDS_POSTPAINT` with `FillBelowRows`.
- `Process.CloseMainWindow()` in a test script closes a message box, not the app, if one is up: a
  test that hangs on exit usually means the app showed an error.
- A Direct2D hardware render target loads the GPU vendor's user-mode driver: +20 MB working set
  for a few graphs (34 -> 58 MB measured). Software is the default; "Hardware-accelerated graphs"
  in the options menu switches (`SetHardwareGraphs` + `Graph::reset()`). Switching back does not
  give the memory back until restart: the driver DLLs stay loaded.
- Explorer owns the Ctrl+Shift+Esc hotkey (`RegisterHotKey` fails with 1409). Ctrl+Shift+Esc
  reaches us only through the IFEO hook; the tray hotkey is Win+Shift+Esc.
- `fopen_s` opens exclusively: two instrumentation sinks writing one trace file lose one of them.
- Testing service actions: pick a service whose DACL gives `RP` (start) only to SY/BA (`sc sdshow`),
  e.g. ALG. Many manual services (TapiSrv) let interactive users start them, so an "expect access
  denied" test would really start the service.

## Known cosmetic items

- The themed ListView draws faint vertical column separators in report view (both `Explorer` and
  `ItemsView` themes do). Windows 10's Task Manager had them too; decide later whether to suppress.
- The filter matches descriptions, users and paths only for processes whose facts were already
  fetched (i.e. that have been on screen); names and PIDs always match.
- Light theme was checked on screen in every tab (2026-10-30), and switching Options > Theme
  (Use Windows setting / Light / Dark) re-themes the running window, menus included. High
  contrast has only been checked by reading the code. With "Show high usage in the accent color"
  on, the selected row's tint sits between two heat levels, so in light theme a selection reads
  much like a shaded cell; only the unshaded Name / Status cells set it apart.
- UWP apps show up as "Application Frame Host" in Apps: the frame window belongs to that process,
  and mapping it to the app's own process (Task Manager does) is not done.
- During the first second the Processes groups settle: a process is "Background" until its path
  arrives and puts it under Windows processes.
- Working set is ~23 MB with ~970 processes (design target: 20 MB at 400), ~35 MB once the
  Performance tab has loaded Direct2D. Startup to first rows is ~10-15 ms slower than milestone 1
  because the first screen now has two ListViews (grid + nav). Restoring the Performance tab as
  the last-used tab loads Direct2D at startup.
- The tray icon and the window use the stock application icon; there is no app icon yet.
- Per tick at ~950 processes / 13k threads: the process snapshot is ~7 ms on the sampler (the design
  assumed ~1 ms at 400), counters ~1.6 ms (2 ms with details), UI work ~1 ms. Idle CPU measures
  1.5-3% of one core on this machine; the design target is 0.5% at 400 processes.
- CPU per-core graphs cover the current processor group only (64 logical processors at most).

## Not yet done (from the design)

GPU column (per process), Set affinity, Properties, "Go to services" from a process, stopping
dependent services, Processes column picker, memory composition bar and SMBIOS slot/speed details,
per-process network (ETW), "Not responding" on the Details tab (window info is only gathered while
Processes is visible), UWP app mapping, an app icon, code signing (the design's antivirus
mitigation), running `--bench` under load in CI.
