#include "snapshot.h"

#include <winternl.h>

#include <algorithm>
#include <cwchar>

namespace lask {
namespace {

constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);

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

bool Capture(std::vector<std::byte>& buffer, Snapshot& out) {
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

        if (p->NextEntryOffset == 0) break;
        p = reinterpret_cast<const NtProcessInfo*>(reinterpret_cast<const std::byte*>(p) + p->NextEntryOffset);
    }
    return true;
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
    bool first = true;
    while (!stop.stop_requested()) {
        auto snap = std::make_shared<Snapshot>();
        if (Capture(buffer, *snap)) {
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

DWORD TerminateProcessByIdentity(uint32_t pid, int64_t createTime) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError();

    FILETIME created{}, exited{}, kernel{}, user{};
    if (createTime != 0 && GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        const int64_t actual = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (actual != createTime) {
            CloseHandle(h);
            return ERROR_NOT_FOUND; // the PID now belongs to a different process
        }
    }
    const DWORD err = TerminateProcess(h, 1) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    return err;
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

} // namespace lask
