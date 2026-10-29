// The Performance tab's data: one panel per device, each with ring buffers of recent rates.
// Owned by the UI thread and fed from every adopted snapshot, so the graphs already have history the
// first time the tab is opened, and a ring never changes while it is being drawn.
#pragma once

#include "counters.h"
#include "snapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace lask {

// The last kCapacity samples of one series: 5 minutes at one sample a second.
class Ring {
public:
    static constexpr int kCapacity = 300;

    void push(float value) {
        values_[head_] = value;
        head_ = (head_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
    }
    int size() const { return count_; }
    // 0 is the newest sample.
    float ago(int age) const { return values_[(head_ - 1 - age + 2 * kCapacity) % kCapacity]; }
    float latest() const { return count_ ? ago(0) : 0.0f; }
    float max(int samples) const {
        float m = 0.0f;
        for (int i = 0; i < std::min(samples, count_); ++i) m = std::max(m, ago(i));
        return m;
    }

private:
    std::array<float, kCapacity> values_{};
    int head_ = 0;
    int count_ = 0;
};

enum class PanelKind : uint8_t { Cpu, Memory, Disk, Network, Gpu };

struct Panel {
    PanelKind kind = PanelKind::Cpu;
    uint64_t key = 0;   // disk number, adapter LUID, GPU LUID; 0 for CPU and memory
    size_t device = 0;  // index into the latest Machine's list for this kind
    int ordinal = 0;    // "GPU 0", "GPU 1"

    // CPU: a = utilization %. Memory: a = bytes in use. Disk: a = active time %, b = read B/s,
    // c = write B/s. Network: a = send bit/s, b = receive bit/s. GPU: a = utilization %,
    // b = dedicated bytes in use.
    Ring a, b, c;
    std::vector<Ring> cores; // CPU: utilization % per logical processor

    // The newest values, for the headline and the stats grid.
    struct Latest {
        bool valid = false;
        double cpu = 0;
        uint32_t mhz = 0;
        size_t processes = 0;
        uint64_t threads = 0, handles = 0, uptimeMs = 0;
        uint64_t inUse = 0, available = 0, committed = 0, commitLimit = 0;
        bool hasDetails = false;
        uint64_t cached = 0, pagedPool = 0, nonPagedPool = 0;
        double active = 0, readBps = 0, writeBps = 0, responseMs = 0;
        double sendBits = 0, receiveBits = 0;
        double gpu = 0;
        std::array<double, kGpuEngineTypes> engine{}; // max per engine type, -1 if the GPU has none
        uint64_t dedicated = 0, shared = 0;
    } latest;
};

class PerfModel {
public:
    // Every adopted snapshot. Adds and drops panels as devices come and go, keeping the history of
    // the ones that stay.
    void update(const Snapshot& current, const Snapshot* previous);

    const std::vector<std::unique_ptr<Panel>>& panels() const { return panels_; }
    const Machine* machine() const { return machine_.get(); }

private:
    void syncPanels(const Machine& machine);

    std::vector<std::unique_ptr<Panel>> panels_;
    std::shared_ptr<const Machine> machine_;
};

} // namespace lask
