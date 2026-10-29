# Lask Manager — System Design

Oct 29, 2026 · Lotem · exported from the Claude Doc; the doc is the editable source

## Why Task Manager is slow when you need it most

Task Manager is slow at exactly the wrong moment because it is a heavy app that does expensive per-process work on startup, on a machine that is already starved. The pain is mostly four things, and each one maps to a design rule below.

| Cause | What happens on a struggling PC | Rule for this design |
| --- | --- | --- |
| Heavy runtime | The Windows 11 Task Manager is a XAML/WinUI app with Mica, animations and a large working set. Paging in the XAML stack and DLLs takes seconds when the disk or RAM is saturated. | Single small native exe, no XAML, no .NET, few DLLs. Target under 2 MB on disk, under 20 MB working set. |
| Per-process enrichment on the hot path | On every refresh it opens every process to fetch icons, version info (the friendly name), user, GPU and power data. Hundreds of OpenProcess calls while the system is thrashing. | One kernel snapshot call per tick (NtQuerySystemInformation). Everything else is fetched lazily, cached, and only for rows that are visible. |
| Fighting for the same resources | It runs at High priority, but its I/O and memory still compete with whatever is hogging the machine; it also gets EcoQoS-throttled onto E-cores on hybrid CPUs. | High priority class, high I/O priority, power throttling disabled, memory priority raised, worker threads kept below the UI thread. |
| UI rebuilds and reorders under your cursor | The grid re-sorts by CPU every second, so the row you are about to right-click moves. | Virtualized grid that only redraws visible rows; sort freezes while a context menu or the mouse button is held; kill never blocks the UI. |

The fourth cause is also why the fix is only partly about speed: a kill button that reliably hits the right process is the real product.

## Requirements

The product is a Task Manager replacement that opens instantly and kills reliably, with four tabs and nothing else.

**Functional**

- Processes tab: app name, PID group, status, CPU %, memory (private working set), disk MB/s, GPU %; sort by any column header, with CPU and memory descending as the common case; grouped like Task Manager (Apps, Background, Windows processes); right-click → End task, End process tree, Open file location, Go to details, Properties.
- Performance tab: CPU (total + per-core graph, clock, uptime), Memory (used/available/committed, graph), each physical disk (active time, throughput), each network adapter (send/receive), each GPU (utilization, VRAM). Selectable 30 s, 1 min or 5 min rolling graphs.
- Details tab: flat table of every process: image name, PID, status, user, CPU, memory, description, command line, image path, elevated, architecture, threads, handles, priority. Sortable, column picker, filter box. Right-click → End, End tree, Set priority, Set affinity, Open file location, Search online.
- Services tab: name, PID, description, status, group, startup type. Right-click → Start, Stop, Restart, Open Services, Go to details.
- Global: Ctrl+Shift+Esc or a tray hotkey opens it; refresh rate selectable (0.5 s, 1 s, 2 s, paused); always-on-top; restart as administrator; optional "replace Task Manager" so Windows launches this one.

**Non-functional**

| Metric | Target | Why |
| --- | --- | --- |
| Cold start to first full process list | under 300 ms idle, under 1.5 s with CPU at 100 % and RAM full | The whole point. Measure this on every build. |
| Working set | under 20 MB with 400 processes | Less to page in under memory pressure. |
| CPU cost per 1 s refresh | under 0.5 % of one core at 400 processes | A task manager must not be the thing it is showing. |
| Kill latency | click to TerminateProcess call under 50 ms; UI never waits on the kill | A hung target must not hang the tool. |
| Dependencies | ntdll, kernel32, user32, gdi32, advapi32, d2d1, dwrite, dwmapi, iphlpapi; no runtime installers | Zero install, copy one exe. |

**Constraints**

- Solo developer, C++ expert; prefer one codebase with no framework learning curve.
- Windows 10 1809 and later, Windows 11 primary. x64 only at first; ARM64 is a rebuild, not a redesign.
- Must work unelevated for the common case (your own processes) and offer elevation for the rest.

## Architecture

One process, four threads, one shared store: the sampler fills it, the UI reads it, and nothing on the UI thread ever waits on a worker or on the kernel.

_(Diagram: process layout — 4 threads, 1 shared store. See the Architecture section text; the live diagram is in the Claude Doc.)_

The UI thread owns the window, the ListView controls and the graph panels' Direct2D targets and does nothing else. Every call that can stall (process enumeration, OpenProcess, TerminateProcess, the service control manager) lives on one of the three workers, so a hung kernel call hangs a worker, not the window.

**Snapshot model**

- A `ProcessSnapshot` is one immutable allocation: a header (tick time, system-wide CPU/memory/disk/net/GPU counters) plus a flat array of `ProcessRow` {pid, parent pid, create time, image name, session, kernel time, user time, private working set, handle count, thread count, read/write bytes, GPU time}.
- The sampler builds the next snapshot in its own buffer, then publishes it with an atomic pointer swap. The UI reads the newest and the previous one; CPU % and all rates are (current − previous) / elapsed. No locks, no per-row allocations, no strings copied on the hot path.
- Process identity is (pid, create time), never pid alone: Windows recycles PIDs quickly, and a stale row must not inherit the facts of a dead process.
- Facts that require OpenProcess (icon, description, user, command line, image path, elevation) live in a separate cache keyed by that identity, filled by the enrichment thread only for the PIDs in the wanted set (rows on screen plus a small lookahead).

**Startup sequence**

1. Raise own priority class to High, I/O priority to High, disable power throttling (so hybrid CPUs don't park it on E-cores), then create the window and render target. Paint an empty grid immediately.
2. Sampler takes its first snapshot on a worker; the UI shows names and memory as soon as it lands, with CPU blank.
3. Second tick 250 ms later gives the first CPU % column; from then on the configured interval (default 1 s).
4. Enrichment starts only after the second tick, at below-normal priority, so icons and descriptions fill in without delaying the list.

**Tab data flow**

| Tab | Reads from | Extra work |
| --- | --- | --- |
| Processes | newest + previous snapshot, fact cache | grouping and friendly names come from the cache; rows without facts yet show the image name |
| Performance | 300-sample ring buffers the sampler appends to each tick (30 s / 1 min views = last 30 / 60) | none on the UI thread; graphs are drawn from the ring |
| Details | same snapshots, no grouping | column picker decides which facts enter the wanted set |
| Services | a `ServiceSnapshot` the sampler refreshes every 5th tick | service → PID join uses the process snapshot |

## Data collection

Every number on screen comes from a handful of cheap kernel queries; the only per-process handle work is lazy, cached and limited to visible rows. No WMI, no PDH process counters, no Toolhelp32, no EnumProcesses loop.

**Processes and Details (same snapshot, two views)**

| Column | Source | Cost and notes |
| --- | --- | --- |
| Name, PID, parent, session, threads, handles | `NtQuerySystemInformation(SystemProcessInformation)` | One call returns every process and its threads. Keep the buffer between ticks and grow on `STATUS_INFO_LENGTH_MISMATCH`. \~1 ms for 400 processes. |
| CPU % | `KernelTime + UserTime` delta / (elapsed × logical CPUs) | From the same call. Matches Task Manager's definition. |
| Memory | `WorkingSetPrivateSize` (private working set) | Same call. Also `PrivatePageCount` for commit on Details. |
| Disk MB/s | `ReadTransferCount + WriteTransferCount` delta | Same call. Task Manager shows the same counters. |
| GPU % | `D3DKMTQueryStatistics` (gdi32), per adapter node, per process running time delta | Optional column. Undocumented but stable since Win 8; this is what Task Manager uses. Fail soft if it errors. |
| Status (Running / Suspended / Not responding) | Suspended: every thread `WaitReason == Suspended` from the thread array. Not responding: `IsHungAppWindow` on the process's top-level windows | `EnumWindows` once per tick on the sampler thread also yields window titles for the Apps group. |
| Description (friendly name) | `GetFileVersionInfo` → `FileDescription` on the image path | Lazy, enrichment thread, cached by path (not PID). Falls back to the image name. |
| Icon | `PrivateExtractIcons` or `ExtractIconEx` on the image path, 16 px | Lazy, cached by path, converted once to a Direct2D bitmap. Avoid `SHGetFileInfo`: it can call into the shell and block. |
| Image path | `QueryFullProcessImageName` with `PROCESS_QUERY_LIMITED_INFORMATION` | Lazy. Works for most protected processes too. |
| User | `OpenProcessToken` → `GetTokenInformation(TokenUser)` → `LookupAccountSid` | Lazy, SID → name cached. |
| Command line | `NtQueryInformationProcess(ProcessCommandLineInformation)` | Lazy. Available since Win 8.1, no PEB reading needed. |
| Elevated, architecture, priority | `TokenElevation`, `IsWow64Process2`, `GetPriorityClass` | Lazy, cheap once the handle is open. |

Grouping on the Processes tab: Apps = has a visible top-level window; Windows processes = session 0, or runs as SYSTEM / LOCAL SERVICE / NETWORK SERVICE, or image under `%SystemRoot%`; everything else is Background. Per-process network needs an ETW session (`Microsoft-Windows-Kernel-Network`); leave it out of v1.

**Performance**

| Panel | Source | Notes |
| --- | --- | --- |
| CPU | `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`: idle, kernel, user per logical CPU | Total and per-core graphs from one call. Current clock from `CallNtPowerInformation(ProcessorInformation)`; base clock and name from `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0`. Uptime from `GetTickCount64`. Process, thread and handle totals come from the process snapshot. |
| Memory | `GlobalMemoryStatusEx` + `GetPerformanceInfo` | In use, available, committed / commit limit, cached, paged and non-paged pool. Slot and speed details need SMBIOS (`GetSystemFirmwareTable('RSMB')`); optional. |
| Disk | `IOCTL_DISK_PERFORMANCE` on `\\.\PhysicalDriveN` | Bytes read/written and idle time → throughput and active %. Model name via `IOCTL_STORAGE_QUERY_PROPERTY`. Needs no admin. |
| Network | `GetIfTable2` → `InOctets` / `OutOctets` per adapter | Filter to operational, non-loopback adapters. Name, type and link speed come with the row. |
| GPU | `D3DKMTQueryStatistics`: adapter and node running time; segment usage for VRAM | Per-engine % (3D, Copy, Video Decode) and dedicated/shared memory. Adapter name via `D3DKMTQueryAdapterInfo`. |

Each panel keeps a ring buffer per series sized for the longest window (300 samples at a 1 s tick for 5 min; the 30 s and 1 min views draw the last 30 or 60); the graph draws from the ring, so a tick that lands mid-paint shows up on the next frame.

**Services**

| Field | Source | Cadence |
| --- | --- | --- |
| Name, display name, state, PID | `EnumServicesStatusEx(SC_ENUM_PROCESS_INFO, SERVICE_WIN32)` | Every 5th tick (5 s). One call, \~250 services, well under 1 ms. |
| Description, startup type, svchost group | `QueryServiceConfig2` + `QueryServiceConfig` per service; group parsed from `-k <group>` in the image path | Once at first view, on the enrichment thread, cached by service name. |
| Start / Stop / Restart | `OpenService` + `StartService` / `ControlService(SERVICE_CONTROL_STOP)`; restart polls `QueryServiceStatusEx` until stopped, then starts | Action thread. Most services need admin; show the shield icon and offer elevation when `OpenService` fails with access denied. |

**Refresh cadence**

| Work | Interval | Thread |
| --- | --- | --- |
| Process snapshot, CPU, memory, disk, network, GPU counters | every tick (default 1 s; 0.5 s / 2 s / paused selectable) | sampler |
| Service status | every 5th tick | sampler |
| Window titles and hung check | every tick, only while Processes tab is visible | sampler |
| Facts for visible rows | continuous, draining the wanted set | enrichment |
| Sort and group | only on a new snapshot or a header click; frozen while the right mouse button is down or a menu is open | UI |

## UI and theming

The window is a plain Win32 top-level window built from common controls: a `ListView` in virtual mode for every grid and for the left navigation, native menus, and one small owner-drawn child window per performance graph. Nothing is hand-drawn except the graphs, because Windows has no graph control.

**Common controls first, custom only where Windows has nothing**

A `ListView` with `LVS_OWNERDATA` is exactly the virtualized grid the design needs: it asks for text only for the cells in view (`LVN_GETDISPINFO`), the row count is set per tick with `LVM_SETITEMCOUNT` plus `LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL`, and sorting, column resize and reorder (`LVS_EX_HEADERDRAGDROP`), sort arrows (`HDF_SORTUP`) and keyboard navigation come built in. Colors are set per item and sub-item through `NM_CUSTOMDRAW`, which is documented, so dark backgrounds, accent selection and heat-map cells cost nothing custom. The parts Windows leaves half-themed (scrollbars, header, menus, tooltips) use the same calls every large Win32 app ships: `SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr)` on the list and header, the uxtheme `SetPreferredAppMode` / `FlushMenuThemes` ordinals for menus, and `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` for the title bar; `DWMWA_SYSTEMBACKDROP_TYPE` adds Mica on Windows 11 and fails harmlessly on 10. The graph panel is the one hand-written control: a child window that draws a polyline and fill from the ring buffer with Direct2D on `WM_PAINT`, about 150 lines.

**Matching the Windows look**

| Element | Where the value comes from |
| --- | --- |
| Light / dark | `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme` (0 = dark) |
| Accent color | `HKCU\Software\Microsoft\Windows\DWM\AccentColor` (stored as ABGR); fallback `DwmGetColorizationColor`. Same number `UISettings::GetColorValue(Accent)` returns, without initializing WinRT. |
| Accent shades (hover, pressed, graph fills) | Derived in HSL from the accent, the way Fluent's `SystemAccentColorLight1..3` / `Dark1..3` steps are: lighten or darken in fixed steps; no second registry read needed. |
| Font | `Segoe UI Variable` (Win 11) with `Segoe UI` fallback, via the system message font on the controls and DirectWrite on the graphs; sizes 12 / 14 / 20 like Task Manager. |
| Icons | Segoe Fluent Icons glyphs for the left navigation (Processes, Performance, Details, Services), drawn as text. |
| Live changes | `WM_SETTINGCHANGE` with `ImmersiveColorSet` and `WM_DWMCOLORIZATIONCOLORCHANGED` rebuild the palette and invalidate. `WM_DPICHANGED` rescales (per-monitor v2 DPI aware). |

Color roles are a small table (background, card surface, text, secondary text, border, accent, accent-text, selection, graph line, graph fill, warning), filled once per theme change. Nothing outside the palette (custom-draw handlers, graph panels) references a raw color.

**Layout**

- Left navigation: a one-column `ListView` with four rows (Segoe Fluent Icons glyph + label), themed like the grids; a `SysTabControl32` is the Windows 10-style alternative.
- Processes: `ListView` groups (Apps / Background / Windows processes) via `LVM_ENABLEGROUPVIEW`, with the group id answered through `LVIF_GROUPID` in `LVN_GETDISPINFO`; heat-map cell shading through `NM_CUSTOMDRAW` sub-item colors. If virtual-mode groups misbehave, group headers become flat rows drawn differently.
- Performance: a `ListView` of panels on the left (name, headline number, sparkline drawn in custom draw), the selected panel's graph child window and a stats grid on the right.
- Details: dense flat `ListView`, an edit control as the filter box above it, column picker in the header context menu.
- Services: flat `ListView` with a status color dot via custom draw.
- Context menus are native `TrackPopupMenu` menus; keyboard: Delete = End task, Ctrl+F = filter, F5 = refresh now, Esc = close.

**Update cost**

Per tick the UI thread calls `LVM_SETITEMCOUNT` once and `LVM_REDRAWITEMS` for the visible range; the `ListView` then asks `LVN_GETDISPINFO` for only those cells, which format numbers straight from the snapshot into a small reusable buffer. No per-row allocation, no strings kept. The graph windows invalidate only themselves. With 400 processes this is well under a millisecond per tick, and nothing redraws between ticks.

## Killing processes and privileges

End task is one `OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess` on the action thread; the UI marks the row "ending" and moves on, so a target stuck in the kernel can only hang that thread, not the window.

**Terminate paths, in the order tried**

1. `OpenProcess(PROCESS_TERMINATE, pid)` → `TerminateProcess(h, 1)`. Works unelevated for your own processes; this is the common case.
2. Access denied → if we hold `SeDebugPrivilege` (enabled at startup when elevated), retry: the privilege bypasses the DACL on almost everything except protected processes (PPL: antivirus, csrss, some system services).
3. Still denied and not elevated → offer "Restart as administrator" inline on the row (`ShellExecute` with `runas`, pass the selected PID so the elevated instance kills it immediately on launch).
4. Protected process even when elevated → say so plainly ("Protected by Windows; cannot be ended") instead of a generic error. Task Manager silently fails here, which is part of the frustration.

**End process tree** walks the snapshot by parent PID, verifies each child's create time is later than its parent's (a recycled PID can look like a child), then terminates leaves first. Two-second cap per process, then move on and report what is left.

**Verification**: after `TerminateProcess` returns, the row stays until the next snapshot no longer contains (pid, create time). A process that lingers two ticks is shown as "terminating" with its reason when known (`NtQueryInformationProcess(ProcessBasicInformation)` exit status, or a pending driver I/O).

**Elevation strategy**

- Manifest `asInvoker`, never `requireAdministrator`: launching must never pop UAC when you just want to see what is running.
- When started elevated: enable `SeDebugPrivilege`, `SeIncreaseBasePriorityPrivilege` (for Set priority) and `SeShutdownPrivilege`; show a shield in the title bar.
- Service start/stop and killing other users' processes show the shield on the menu item and route through "Restart as administrator" when denied.
- The elevated relaunch passes `--kill <pid> --at <createtime>` so the kill happens before the window even paints.

**Replace Task Manager (optional, opt-in)**

`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe` → `Debugger = "<path>\lask.exe"`: Windows then launches this exe for Ctrl+Shift+Esc, the Ctrl+Alt+Del screen and the taskbar menu. This is the same mechanism Process Explorer and System Informer use. Requires admin to set; the app offers it in settings and removes it on uninstall. The exe must ignore the extra `taskmgr.exe` argument it receives.

**Making the app itself survive a thrashing machine**

- `SetPriorityClass(HIGH_PRIORITY_CLASS)` for the process; UI thread at `THREAD_PRIORITY_HIGHEST`, sampler normal, enrichment `BELOW_NORMAL`.
- `SetThreadInformation(ThreadPowerThrottling)` and `SetProcessInformation(ProcessPowerThrottling)` with throttling disabled, so hybrid CPUs don't schedule it on E-cores or in EcoQoS.
- `SetProcessInformation(ProcessMemoryPriority, MEMORY_PRIORITY_NORMAL)` and `SetFileInformationByHandle` / `NtSetInformationProcess(ProcessIoPriority, High)` so its page-ins win against a disk-bound hog.
- Static CRT, `/OPT:REF`, delay-load nothing: fewer pages to fault in. Tray-resident mode (optional) keeps it warm, so opening is a window show rather than a process start.

## Tech stack and trade-offs

C++23 (/std:c++23, MSVC Build Tools 14.52 or later), raw Win32 common controls for the grids, Direct2D only for the graphs, MSVC with static CRT, CMake. It hits the startup and size targets, and the only hand-written control is the graph panel, because Windows ships no graph control.

| Option | Startup under load | Dark mode + accent | Effort for a solo C++ dev | Verdict |
| --- | --- | --- | --- | --- |
| C++ / Win32 / Direct2D | Best: \~1 MB exe, 8 DLLs, no runtime | Full control, one code path | Medium: a custom grid is about 2k lines | Graphs only; fallback for any control that can't be themed |
| C++ / Win32 / common controls | Best | Good enough: list and header colors via documented NM\_CUSTOMDRAW; dark scrollbars, menus and title bar via the same undocumented uxtheme calls every major Win32 app ships | Low | Chosen |
| Rust / windows-rs / Direct2D | Same as C++ | Same | High: new language, same amount of UI code | No benefit for this project |
| C# / WinForms or WPF | Poor: .NET runtime JIT and 30-60 MB working set on first paint; neither is NativeAOT-capable | WPF good, WinForms poor | Low | Reproduces Task Manager's problem |
| WinUI 3 / XAML Islands | Worst: this is what Task Manager is | Native | Low | Reproduces Task Manager's problem exactly |
| Flutter desktop | Poor: Dart runtime, Skia, \~40 MB | Good | Low (known stack) | Too heavy for the goal |

**Decisions inside the chosen stack**

- Direct2D only for the graph panels: GDI could draw them too, but Direct2D gives anti-aliased lines and alpha fills, and each panel is one small child window. The grids use the system font through the ListView, which gives them `Segoe UI Variable` rendering that matches the rest of Windows 11. Keep a GDI fallback only if D2D device creation fails (remote desktop without GPU, bad driver) — software WARP rendering is the simpler answer and is already built into D2D.
- No C++/WinRT: everything needed (accent, theme, DWM) is reachable from registry and dwmapi; skipping WinRT avoids `RoInitialize` and a few MB of activation overhead.
- NT APIs through `ntdll` with hand-declared prototypes (`phnt` headers are the reference). They are undocumented but have been stable across every Windows release since Vista; this is what every serious process tool uses.
- No plugin system, no scripting, no settings beyond a small JSON next to the exe (or `%LOCALAPPDATA%`): every feature that is not one of the four tabs is a feature Task Manager has that you don't use.

**What this costs**

- Accessibility: the ListView grids come with UI Automation for free; only the graph panels are opaque to screen readers, which is acceptable for a personal tool; `IRawElementProviderSimple` can be added later if needed.
- Localization: strings are English-only in v1; the structure (one string table) makes it a later job, not a redesign.
- Testing: the sampler and snapshot code is plain C++ and unit-testable; the UI is tested by eye and by a startup-time script (`Measure-Command` in PowerShell under a synthetic load).

## Build order

Ship the kill button first: a milestone-1 build that opens in 200 ms and ends a process already beats Task Manager on the day that matters. Each step is usable on its own.

1. **Core sampler + Details tab (kill-only MVP).** `NtQuerySystemInformation` snapshot, delta CPU %, a flat virtual grid with name / PID / CPU / memory, sort by header, Delete key ends the selected process, Restart-as-admin. Measure cold start under `cpustres` + a memory hog before moving on; everything later must not regress it.
2. **Theme.** Palette from registry + DWM, dark title bar, live `WM_SETTINGCHANGE`, accent on selection and header. Do this early so every later screen is built against the real look.
3. **Enrichment.** Wanted set, fact cache, icon / description / user / command line / path columns, column picker and filter box on Details.
4. **Processes tab.** Grouping (Apps / Background / Windows), window titles, expand/collapse, heat-map cells, End process tree, Open file location, Go to details.
5. **Performance tab.** CPU + per-core, memory, then disk, network, GPU. Ring buffers and the graph widget. GPU is last because `D3DKMTQueryStatistics` is the fiddliest API in the project.
6. **Services tab.** Enumerate, join to PIDs, config cache, start / stop / restart with elevation.
7. **Polish.** Replace-Task-Manager option, tray-resident mode and hotkey, always-on-top, settings file, column widths persisted, DPI change handling, installer-free zip release.

Step 1 is about two evenings of work; steps 2-4 are where most of the time goes because that is where the grid grows its features. Set priority, affinity and the GPU column can slip to step 7 without hurting the daily use.

## Risks and what to revisit

The real risks are undocumented APIs and antivirus, not performance; the design already leaves performance headroom.

| Risk | Likelihood | Mitigation |
| --- | --- | --- |
| An NT API shape changes in a Windows update (`SYSTEM_PROCESS_INFORMATION`, `D3DKMTQueryStatistics`) | Low; these have been stable for 15+ years and Task Manager depends on them too | Use `phnt`-style structs with version checks; GPU column fails soft to blank, never crashes |
| Antivirus flags a small unsigned exe that opens processes with `SeDebugPrivilege` and writes IFEO | Medium | Sign the exe (a cheap code-signing cert or self-signed for your own machine); keep the IFEO option opt-in; avoid `PROCESS_VM_READ`, which is the access right that looks like injection |
| Undocumented dark-mode theming of common controls breaks in a Windows update | Medium | Isolate the uxtheme ordinal calls behind one function with a build-number check; the documented NM\_CUSTOMDRAW path keeps the grids correct, so the worst case is light scrollbars and menus, not a broken app |
| Direct2D fails on remote desktop or a broken GPU driver | Low | Create the device with `D2D1_RENDER_TARGET_TYPE_DEFAULT` so it falls back to WARP; the whole UI is a few hundred primitives, software rendering is fine |
| Startup regression creeps in as features land | High if unmeasured | A `--bench` flag that prints time-to-first-snapshot and time-to-first-paint; run it under load in CI or a script before each release |
| Killing the wrong process when the list re-sorts | Medium | Identity is (pid, create time) from the moment the row is clicked; sort is frozen while the mouse is down or a menu is open |

**Open questions**

- [ ] Windows 10 support: worth keeping, or Windows 11 only? It changes little (no Mica, `Segoe UI` instead of Variable), but it decides how much fallback code to write.
- [ ] Tray-resident mode on by default, or only a launch-on-demand exe? Resident guarantees instant open but is one more thing running.
- [ ] Replace Task Manager via IFEO as a first-class setting, or leave it to the user with a documented registry key?
- [ ] Per-process network (ETW) and the Startup / Users / App history tabs: out of scope for v1; revisit only if daily use misses them.

**Revisit as it grows**

- If process counts go past \~2,000 (build servers, container hosts), move sorting off the UI thread and keep a sorted index in the snapshot.
- If the fact cache gets large, evict by last-seen tick; today a few hundred entries is nothing.
- If you want it on ARM64 laptops, the only native-arch concern is `IsWow64Process2` and the x64 emulation column; everything else is a recompile.
