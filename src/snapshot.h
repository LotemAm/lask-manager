// Process snapshots: one NtQuerySystemInformation call per tick, published lock-free.
#pragma once

#include "counters.h"

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

// A top-level window the user would recognise as an app window (what Alt+Tab shows).
struct WindowInfo {
    HWND hwnd = nullptr;
    uint32_t pid = 0;
    bool hung = false;     // IsHungAppWindow: no message pumped for 5 s
    wchar_t title[128] = {};
};

// Immutable once published. The UI keeps the newest and the previous one to compute rates.
struct Snapshot {
    uint64_t qpc = 0;          // QueryPerformanceCounter at capture
    uint64_t qpcFrequency = 0; // ticks per second
    uint32_t logicalCpus = 0;
    uint64_t totalPhysical = 0; // bytes of RAM, read once when the sampler starts
    bool hasWindows = false;    // windows was filled (only while the Processes tab is visible)
    std::vector<ProcessRow> rows;
    std::vector<WindowInfo> windows;
    SystemCounters system;      // empty in the very first snapshot (device discovery comes after it)

    // Elapsed time since `earlier`, in 100 ns units (the same unit as ProcessRow::cpuTime).
    uint64_t elapsed100ns(const Snapshot& earlier) const;
};

// Captures one snapshot. `buffer` is kept between calls so it grows once and stays grown.
// `windows` adds one EnumWindows pass for app window titles and the hung check.
bool Capture(std::vector<std::byte>& buffer, Snapshot& out, bool windows);

// Per-row rates between two snapshots, aligned with `current.rows`. A row without a matching
// identity in `previous` (or without a previous snapshot) gets -1.
struct RowRates {
    double cpu = -1.0;             // percent of all logical CPUs
    double diskBytesPerSec = -1.0; // read + write
};
void ComputeRates(const Snapshot& current, const Snapshot* previous, std::vector<RowRates>& out);

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
    void setWantWindows(bool want) { wantWindows_.store(want, std::memory_order_relaxed); }
    void setWantDetails(bool want) { wantDetails_.store(want, std::memory_order_relaxed); }

    std::shared_ptr<const Snapshot> latest() const { return latest_.load(std::memory_order_acquire); }

private:
    void run(std::stop_token stop);

    std::atomic<std::shared_ptr<const Snapshot>> latest_;
    std::atomic<unsigned> intervalMs_{1000};
    std::atomic<bool> wantWindows_{false};
    std::atomic<bool> wantDetails_{false}; // Performance tab visible: clock speed, cache, pools
    HANDLE wake_ = nullptr;
    HWND notify_ = nullptr;
    UINT message_ = 0;
    std::jthread thread_;
};

// Opens `pid`, verifies its creation time still matches (PIDs get recycled), terminates it, and
// waits up to `waitMs` for it to be gone. Returns 0 on success, ERROR_NOT_FOUND if the identity no
// longer matches, else a Win32 error.
DWORD TerminateProcessByIdentity(uint32_t pid, int64_t createTime, DWORD waitMs = 0);

// Ends `root` and its descendants in `snapshot`, leaves first, two seconds at most per process.
// A child counts only if it was created after its parent (a recycled parent PID is not a parent).
// Returns 0 or the first failure, with the process that failed in `failed`.
DWORD TerminateTree(const Snapshot& snapshot, Identity root, Identity& failed);

bool IsElevated();
bool EnableDebugPrivilege(); // only succeeds when elevated; lets us end other users' processes

// Lazy per-process queries for the enrichment thread; never on the per-tick path.
// Command line through ProcessCommandLineInformation (Windows 8.1+; PROCESS_QUERY_LIMITED_INFORMATION).
bool QueryCommandLine(HANDLE process, std::wstring& out);
// NT image path ("\Device\HarddiskVolume3\...") by PID without opening the process, through
// SystemProcessIdInformation. Works for processes an unelevated caller cannot open.
bool QueryImageNtPath(uint32_t pid, std::wstring& out);

} // namespace lask
