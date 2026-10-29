#include "snapshot.h"

#include <dwmapi.h>
#include <winternl.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <unordered_map>

namespace lask {
namespace {

constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);
constexpr ULONG kThreadStateWaiting = 5;  // KTHREAD_STATE::Waiting
constexpr ULONG kWaitReasonSuspended = 5; // KWAIT_REASON::Suspended
constexpr auto kSystemProcessIdInformation = static_cast<SYSTEM_INFORMATION_CLASS>(88);
constexpr auto kProcessCommandLineInformation = static_cast<PROCESSINFOCLASS>(60);

// winternl.h ships an obfuscated SYSTEM_PROCESS_INFORMATION ("Reserved1" ...). These are the real
// layouts, as documented by phnt; they have been stable since Windows Vista.
struct NtClientId {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
};

struct NtThreadInfo {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    NtClientId ClientId;
    LONG Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
};

struct NtProcessInfo {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    NtThreadInfo Threads[1];
};

struct NtProcessIdInfo {
    HANDLE ProcessId;
    UNICODE_STRING ImageName; // caller-supplied buffer; MaximumLength comes back as the size needed
};

uint32_t HandleToPid(HANDLE h) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));
}

void CopyImageName(wchar_t (&dst)[64], const UNICODE_STRING& s, uint32_t pid) {
    if (s.Buffer && s.Length) {
        const size_t n = std::min<size_t>(s.Length / sizeof(wchar_t), std::size(dst) - 1);
        std::wmemcpy(dst, s.Buffer, n);
        dst[n] = L'\0';
    } else if (pid == 0) {
        wcscpy_s(dst, L"System Idle Process");
    } else if (pid == 4) {
        wcscpy_s(dst, L"System");
    } else {
        wcscpy_s(dst, L"<unknown>");
    }
}

// Alt+Tab's notion of an app window: visible, unowned, not a tool window, not cloaked (UWP apps
// suspended in the background keep cloaked windows), with a title. InternalGetWindowText reads the
// stored title without sending WM_GETTEXT, so a hung app cannot stall the sampler.
BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
    wchar_t cls[16];
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) && wcscmp(cls, L"Progman") == 0) return TRUE; // desktop

    WindowInfo w;
    if (InternalGetWindowText(hwnd, w.title, static_cast<int>(std::size(w.title))) <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    w.hwnd = hwnd;
    w.pid = pid;
    w.hung = IsHungAppWindow(hwnd) != FALSE;
    reinterpret_cast<std::vector<WindowInfo>*>(lp)->push_back(w);
    return TRUE;
}

uint32_t LogicalCpuCount() {
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? n : 1;
}

} // namespace

uint64_t Snapshot::elapsed100ns(const Snapshot& earlier) const {
    if (qpcFrequency == 0 || qpc <= earlier.qpc) return 0;
    const uint64_t ticks = qpc - earlier.qpc;
    // ticks / freq seconds -> * 10^7 for 100 ns units; split to avoid overflow on long gaps.
    return (ticks / qpcFrequency) * 10'000'000ull + ((ticks % qpcFrequency) * 10'000'000ull) / qpcFrequency;
}

bool Capture(std::vector<std::byte>& buffer, Snapshot& out, bool windows) {
    if (buffer.empty()) buffer.resize(512 * 1024);

    NTSTATUS status = kStatusInfoLengthMismatch;
    for (int attempt = 0; attempt < 8 && status == kStatusInfoLengthMismatch; ++attempt) {
        ULONG needed = 0;
        status = NtQuerySystemInformation(SystemProcessInformation, buffer.data(),
                                          static_cast<ULONG>(buffer.size()), &needed);
        if (status == kStatusInfoLengthMismatch) buffer.resize(static_cast<size_t>(needed) + 64 * 1024);
    }
    if (status < 0) return false;

    LARGE_INTEGER qpc{}, freq{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    out.qpc = static_cast<uint64_t>(qpc.QuadPart);
    out.qpcFrequency = static_cast<uint64_t>(freq.QuadPart);
    out.logicalCpus = LogicalCpuCount();

    out.rows.clear();
    out.rows.reserve(512);
    const auto* p = reinterpret_cast<const NtProcessInfo*>(buffer.data());
    for (;;) {
        ProcessRow& r = out.rows.emplace_back();
        r.pid = HandleToPid(p->UniqueProcessId);
        r.parentPid = HandleToPid(p->InheritedFromUniqueProcessId);
        r.sessionId = p->SessionId;
        r.threadCount = p->NumberOfThreads;
        r.handleCount = p->HandleCount;
        r.basePriority = p->BasePriority;
        r.createTime = p->CreateTime.QuadPart;
        r.cpuTime = static_cast<uint64_t>(p->KernelTime.QuadPart) + static_cast<uint64_t>(p->UserTime.QuadPart);
        r.privateWorkingSet = static_cast<uint64_t>(p->WorkingSetPrivateSize.QuadPart);
        r.privateBytes = p->PrivatePageCount;
        r.ioBytes = static_cast<uint64_t>(p->ReadTransferCount.QuadPart) +
                    static_cast<uint64_t>(p->WriteTransferCount.QuadPart);
        CopyImageName(r.name, p->ImageName, r.pid);

        // Suspended, the way Task Manager means it: no thread can run because every one of them is
        // parked by a suspend (UWP apps in the background, debuggers, `pssuspend`).
        r.suspended = r.pid != 0 && p->NumberOfThreads > 0;
        for (ULONG t = 0; r.suspended && t < p->NumberOfThreads; ++t)
            r.suspended = p->Threads[t].ThreadState == kThreadStateWaiting &&
                          p->Threads[t].WaitReason == kWaitReasonSuspended;

        if (p->NextEntryOffset == 0) break;
        p = reinterpret_cast<const NtProcessInfo*>(reinterpret_cast<const std::byte*>(p) + p->NextEntryOffset);
    }

    out.windows.clear();
    out.hasWindows = windows;
    if (windows) EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&out.windows));
    return true;
}

void ComputeRates(const Snapshot& current, const Snapshot* previous, std::vector<RowRates>& out) {
    out.assign(current.rows.size(), RowRates{});
    if (!previous) return;
    const uint64_t elapsed = current.elapsed100ns(*previous);
    if (!elapsed) return;
    const double cpuDenominator = static_cast<double>(elapsed) * current.logicalCpus;
    const double seconds = static_cast<double>(elapsed) / 10'000'000.0;

    std::unordered_map<uint32_t, const ProcessRow*> previousByPid;
    previousByPid.reserve(previous->rows.size());
    for (const auto& r : previous->rows) previousByPid.emplace(r.pid, &r);

    for (size_t i = 0; i < current.rows.size(); ++i) {
        const ProcessRow& r = current.rows[i];
        auto it = previousByPid.find(r.pid);
        if (it == previousByPid.end() || it->second->createTime != r.createTime) continue;
        const ProcessRow& before = *it->second;
        const uint64_t cpu = r.cpuTime >= before.cpuTime ? r.cpuTime - before.cpuTime : 0;
        const uint64_t io = r.ioBytes >= before.ioBytes ? r.ioBytes - before.ioBytes : 0;
        out[i].cpu = std::clamp(100.0 * static_cast<double>(cpu) / cpuDenominator, 0.0, 100.0);
        out[i].diskBytesPerSec = static_cast<double>(io) / seconds;
    }
}

// ---- Sampler ---------------------------------------------------------------------------------

Sampler::~Sampler() {
    stop();
}

void Sampler::start(HWND notify, UINT message, unsigned intervalMs) {
    stop();
    notify_ = notify;
    message_ = message;
    intervalMs_.store(intervalMs, std::memory_order_relaxed);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void Sampler::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        if (wake_) SetEvent(wake_);
        thread_.join();
    }
    if (wake_) {
        CloseHandle(wake_);
        wake_ = nullptr;
    }
}

void Sampler::requestNow() {
    if (wake_) SetEvent(wake_);
}

void Sampler::run(std::stop_token stop) {
    std::stop_callback onStop(stop, [this] { SetEvent(wake_); });
    std::vector<std::byte> buffer;
    MEMORYSTATUSEX memory{sizeof memory};
    const uint64_t totalPhysical = GlobalMemoryStatusEx(&memory) ? memory.ullTotalPhys : 0;
    bool first = true;
    while (!stop.stop_requested()) {
        auto snap = std::make_shared<Snapshot>();
        snap->totalPhysical = totalPhysical;
        if (Capture(buffer, *snap, wantWindows_.load(std::memory_order_relaxed))) {
            latest_.store(std::move(snap), std::memory_order_release);
            if (notify_) PostMessageW(notify_, message_, 0, 0);
        }
        // The second tick comes quickly so the CPU column fills in almost at once.
        const DWORD wait = first ? 250 : intervalMs_.load(std::memory_order_relaxed);
        first = false;
        WaitForSingleObject(wake_, wait);
    }
}

// ---- actions ---------------------------------------------------------------------------------

DWORD TerminateProcessByIdentity(uint32_t pid, int64_t createTime, DWORD waitMs) {
    const DWORD access = PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | (waitMs ? SYNCHRONIZE : 0);
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (!h) return GetLastError();

    FILETIME created{}, exited{}, kernel{}, user{};
    if (createTime != 0 && GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        const int64_t actual = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (actual != createTime) {
            CloseHandle(h);
            return ERROR_NOT_FOUND; // the PID now belongs to a different process
        }
    }
    DWORD err = TerminateProcess(h, 1) ? ERROR_SUCCESS : GetLastError();
    // We hold PROCESS_TERMINATE, so "access denied" here means the process is already on its way
    // out (STATUS_PROCESS_IS_TERMINATING), typically a parent that exited when its child was ended.
    if (err == ERROR_ACCESS_DENIED) err = ERROR_SUCCESS;
    if (err == ERROR_SUCCESS && waitMs) WaitForSingleObject(h, waitMs);
    CloseHandle(h);
    return err;
}

DWORD TerminateTree(const Snapshot& snapshot, Identity root, Identity& failed) {
    std::vector<const ProcessRow*> tree;
    for (const auto& r : snapshot.rows)
        if (r.pid == root.pid && r.createTime == root.createTime) tree.push_back(&r);
    if (tree.empty()) return ERROR_NOT_FOUND;
    for (size_t i = 0; i < tree.size(); ++i) // breadth-first; the create-time check also rules out cycles
        for (const auto& r : snapshot.rows)
            if (r.parentPid == tree[i]->pid && r.createTime > tree[i]->createTime) tree.push_back(&r);

    DWORD first = ERROR_SUCCESS;
    for (auto it = tree.rbegin(); it != tree.rend(); ++it) {
        const DWORD err = TerminateProcessByIdentity((*it)->pid, (*it)->createTime, 2000);
        if (err != ERROR_SUCCESS && err != ERROR_NOT_FOUND && first == ERROR_SUCCESS) {
            first = err;
            failed = {(*it)->pid, (*it)->createTime};
        }
    }
    return first;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof elevation;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid) != 0;
    ok = ok && AdjustTokenPrivileges(token, FALSE, &tp, sizeof tp, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

// ---- lazy per-process queries ------------------------------------------------------------------

bool QueryCommandLine(HANDLE process, std::wstring& out) {
    std::vector<std::byte> buffer(1024);
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS status = NtQueryInformationProcess(process, kProcessCommandLineInformation, buffer.data(),
                                                          static_cast<ULONG>(buffer.size()), &needed);
        if (status == kStatusInfoLengthMismatch || status == kStatusBufferTooSmall) {
            if (needed <= buffer.size()) return false;
            buffer.resize(needed);
            continue;
        }
        if (status < 0) return false;
        const auto* s = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
        out.assign(s->Buffer ? s->Buffer : L"", s->Length / sizeof(wchar_t));
        return true;
    }
    return false;
}

bool QueryImageNtPath(uint32_t pid, std::wstring& out) {
    std::vector<wchar_t> buffer(512);
    for (int attempt = 0; attempt < 2; ++attempt) {
        NtProcessIdInfo info{};
        info.ProcessId = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid));
        info.ImageName.Buffer = buffer.data();
        info.ImageName.MaximumLength = static_cast<USHORT>(std::min<size_t>(buffer.size() * sizeof(wchar_t), 0xFFFE));
        const NTSTATUS status = NtQuerySystemInformation(kSystemProcessIdInformation, &info, sizeof info, nullptr);
        if (status == kStatusInfoLengthMismatch && info.ImageName.MaximumLength > buffer.size() * sizeof(wchar_t)) {
            buffer.resize(info.ImageName.MaximumLength / sizeof(wchar_t) + 1);
            continue;
        }
        if (status < 0 || info.ImageName.Length == 0) return false;
        out.assign(buffer.data(), info.ImageName.Length / sizeof(wchar_t));
        return true;
    }
    return false;
}

} // namespace lask
