// System-wide counters for the Performance tab: CPU per logical processor, memory, each physical
// disk, each network adapter, each GPU. Read by the sampler every tick into the snapshot as raw
// cumulative values; the UI turns two snapshots into rates, the same way it does for processes.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lask {

constexpr int kGpuEngineTypes = 10; // DXGK_ENGINE_TYPE_MAX

// What the machine has. Rebuilt only when the set of devices changes, and shared by every snapshot
// in between, so the per-tick data below stays plain numbers.
struct Machine {
    std::wstring cpuName;
    uint32_t baseMhz = 0;
    uint32_t sockets = 0, cores = 0, logical = 0;
    uint64_t totalPhysical = 0;

    struct Disk {
        uint32_t number = 0;   // \\.\PhysicalDriveN
        std::wstring volumes;  // "C: D:"
        std::wstring model;
        std::wstring kind;     // "SSD (NVMe)", "HDD", "SSD (USB)" ...
        uint64_t size = 0;     // bytes; 0 if unknown
    };
    struct Network {
        uint64_t luid = 0;
        std::wstring alias;       // "Wi-Fi", "Ethernet"
        std::wstring description; // the adapter's model
        uint64_t linkBitsPerSec = 0;
        bool wireless = false;
    };
    struct Gpu {
        LUID luid{};
        std::wstring name;
        std::vector<uint8_t> engineOfNode; // DXGK_ENGINE_TYPE per node
        uint32_t segments = 0;
        uint64_t dedicatedLimit = 0, sharedLimit = 0;
    };
    std::vector<Disk> disks;
    std::vector<Network> networks;
    std::vector<Gpu> gpus;
};

// Cumulative counters from one tick, aligned with the Machine's lists.
struct SystemCounters {
    std::shared_ptr<const Machine> machine;

    struct Cpu {
        uint64_t idle = 0, busy = 0; // 100 ns; busy = kernel - idle + user
    };
    std::vector<Cpu> cpus;

    uint64_t availablePhysical = 0;
    uint64_t commitTotal = 0, commitLimit = 0;
    uint64_t uptimeMs = 0;

    // Only while the Performance tab is visible: none of these is graphed.
    bool hasDetails = false;
    uint32_t currentMhz = 0;
    uint64_t cached = 0, pagedPool = 0, nonPagedPool = 0;

    struct Disk {
        bool ok = false;
        uint64_t bytesRead = 0, bytesWritten = 0;
        uint64_t idleTime = 0, queryTime = 0; // 100 ns
        uint64_t readTime = 0, writeTime = 0; // 100 ns, summed over requests
        uint64_t reads = 0, writes = 0;
    };
    std::vector<Disk> disks;

    struct Network {
        bool ok = false;
        uint64_t inOctets = 0, outOctets = 0;
    };
    std::vector<Network> networks;

    struct Gpu {
        bool ok = false;
        std::vector<uint64_t> nodeRunning; // 100 ns per node
        uint64_t dedicatedUsed = 0, sharedUsed = 0;
    };
    std::vector<Gpu> gpus;
};

// Sampler thread only. Keeps disk handles open and rediscovers devices now and then.
class CounterReader {
public:
    CounterReader();
    ~CounterReader();
    CounterReader(const CounterReader&) = delete;
    CounterReader& operator=(const CounterReader&) = delete;

    // `details` adds the counters only the Performance tab shows (clock speed, cache, pools).
    void read(SystemCounters& out, bool details);

private:
    void discover();
    void readCpus(SystemCounters& out);
    void readDisks(SystemCounters& out);
    void readNetworks(SystemCounters& out);
    void readGpus(SystemCounters& out);

    std::shared_ptr<const Machine> machine_;
    std::vector<HANDLE> diskHandles_; // aligned with machine_->disks
    std::vector<std::byte> cpuBuffer_;
    uint32_t ticks_ = 0;
};

} // namespace lask
