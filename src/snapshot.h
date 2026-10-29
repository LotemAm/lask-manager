// Process snapshots: one NtQuerySystemInformation call per tick, published lock-free.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lask {

// One row per process. Plain data, no owning pointers, so a snapshot is one flat allocation.
struct ProcessRow {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    uint32_t sessionId = 0;
    uint32_t threadCount = 0;
    uint32_t handleCount = 0;
    int32_t basePriority = 0;
    int64_t createTime = 0;         // FILETIME units (100 ns since 1601). Identity = (pid, createTime).
    uint64_t cpuTime = 0;           // kernel + user, 100 ns units
    uint64_t privateWorkingSet = 0; // bytes; what Task Manager's "Memory" column shows
    uint64_t privateBytes = 0;      // commit, bytes
    uint64_t ioBytes = 0;           // read + write transfer counts, bytes
    bool suspended = false;         // every thread is waiting with reason Suspended
    wchar_t name[64] = {};          // image name, NUL-terminated, truncated if longer
};

// What a process is, as opposed to which PID it happens to have right now.
struct Identity {
    uint32_t pid = 0;
    int64_t createTime = 0;
    bool operator==(const Identity&) const = default;
};

struct IdentityHash {
    size_t operator()(const Identity& id) const noexcept {
        return std::hash<uint64_t>{}(static_cast<uint64_t>(id.createTime) ^ (uint64_t{id.pid} * 0x9E3779B97F4A7C15ull));
    }
};

// Immutable once published. The UI keeps the newest and the previous one to compute rates.
struct Snapshot {
    uint64_t qpc = 0;          // QueryPerformanceCounter at capture
    uint64_t qpcFrequency = 0; // ticks per second
    uint32_t logicalCpus = 0;
    std::vector<ProcessRow> rows;

    // Elapsed time since `earlier`, in 100 ns units (the same unit as ProcessRow::cpuTime).
    uint64_t elapsed100ns(const Snapshot& earlier) const;
};

// Captures one snapshot. `buffer` is kept between calls so it grows once and stays grown.
bool Capture(std::vector<std::byte>& buffer, Snapshot& out);

// Worker thread that captures a snapshot every interval and posts `message` to `notify`.
// The UI thread never calls into the kernel; it only reads latest().
class Sampler {
public:
    Sampler() = default;
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    void start(HWND notify, UINT message, unsigned intervalMs);
    void stop();
    void setInterval(unsigned intervalMs) { intervalMs_.store(intervalMs, std::memory_order_relaxed); }
    void requestNow(); // take a snapshot as soon as possible (F5, after a kill)

    std::shared_ptr<const Snapshot> latest() const { return latest_.load(std::memory_order_acquire); }

private:
    void run(std::stop_token stop);

    std::atomic<std::shared_ptr<const Snapshot>> latest_;
    std::atomic<unsigned> intervalMs_{1000};
    HANDLE wake_ = nullptr;
    HWND notify_ = nullptr;
    UINT message_ = 0;
    std::jthread thread_;
};

// Opens `pid`, verifies its creation time still matches (PIDs get recycled), terminates it.
// Returns 0 on success, ERROR_NOT_FOUND if the identity no longer matches, else a Win32 error.
DWORD TerminateProcessByIdentity(uint32_t pid, int64_t createTime);

bool IsElevated();
bool EnableDebugPrivilege(); // only succeeds when elevated; lets us end other users' processes

// Lazy per-process queries for the enrichment thread; never on the per-tick path.
// Command line through ProcessCommandLineInformation (Windows 8.1+; PROCESS_QUERY_LIMITED_INFORMATION).
bool QueryCommandLine(HANDLE process, std::wstring& out);
// NT image path ("\Device\HarddiskVolume3\...") by PID without opening the process, through
// SystemProcessIdInformation. Works for processes an unelevated caller cannot open.
bool QueryImageNtPath(uint32_t pid, std::wstring& out);

} // namespace lask
