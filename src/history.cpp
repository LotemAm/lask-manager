#include "history.h"

#include <algorithm>

namespace lask {
namespace {

uint64_t Delta(uint64_t now, uint64_t before) {
    return now >= before ? now - before : 0; // a counter that went backwards (device reset) counts as idle
}

uint64_t GpuKey(const LUID& luid) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}

// The same device in the previous snapshot. Usually the same index in the same Machine; after a
// rediscovery it is looked up by key.
template <class Devices, class KeyOf>
int PreviousIndex(const Devices& now, const Devices& before, size_t index, KeyOf keyOf) {
    if (&now == &before) return static_cast<int>(index);
    for (size_t i = 0; i < before.size(); ++i)
        if (keyOf(before[i]) == keyOf(now[index])) return static_cast<int>(i);
    return -1;
}

} // namespace

void PerfModel::syncPanels(const Machine& m) {
    std::vector<std::unique_ptr<Panel>> next;
    auto take = [this, &next](PanelKind kind, uint64_t key, size_t device, int ordinal) {
        auto it = std::find_if(panels_.begin(), panels_.end(),
                               [&](const auto& p) { return p && p->kind == kind && p->key == key; });
        std::unique_ptr<Panel> panel = it != panels_.end() ? std::move(*it) : std::make_unique<Panel>();
        panel->kind = kind;
        panel->key = key;
        panel->device = device;
        panel->ordinal = ordinal;
        next.push_back(std::move(panel));
    };
    take(PanelKind::Cpu, 0, 0, 0);
    take(PanelKind::Memory, 0, 0, 0);
    for (size_t i = 0; i < m.disks.size(); ++i) take(PanelKind::Disk, m.disks[i].number, i, static_cast<int>(m.disks[i].number));
    for (size_t i = 0; i < m.networks.size(); ++i) take(PanelKind::Network, m.networks[i].luid, i, static_cast<int>(i));
    for (size_t i = 0; i < m.gpus.size(); ++i) take(PanelKind::Gpu, GpuKey(m.gpus[i].luid), i, static_cast<int>(i));
    panels_ = std::move(next);
}

void PerfModel::update(const Snapshot& current, const Snapshot* previous) {
    const SystemCounters& now = current.system;
    if (!now.machine) return; // the very first snapshot carries no counters
    if (now.machine != machine_) {
        machine_ = now.machine;
        syncPanels(*machine_);
    }
    const Machine& m = *machine_;
    const SystemCounters* before = previous && previous->system.machine ? &previous->system : nullptr;
    const uint64_t elapsed = previous ? current.elapsed100ns(*previous) : 0;
    const double seconds = static_cast<double>(elapsed) / 10'000'000.0;
    const bool rates = before && elapsed > 0;

    for (auto& owned : panels_) {
        Panel& p = *owned;
        Panel::Latest& l = p.latest;
        switch (p.kind) {
        case PanelKind::Cpu: {
            l.processes = current.rows.size();
            l.threads = l.handles = 0;
            for (const auto& r : current.rows) {
                l.threads += r.threadCount;
                l.handles += r.handleCount;
            }
            l.uptimeMs = now.uptimeMs;
            l.hasDetails = now.hasDetails;
            l.mhz = now.currentMhz;
            if (!rates || before->cpus.size() != now.cpus.size() || now.cpus.empty()) break;
            if (p.cores.size() != now.cpus.size()) p.cores.assign(now.cpus.size(), Ring{});
            uint64_t busy = 0, total = 0;
            for (size_t i = 0; i < now.cpus.size(); ++i) {
                const uint64_t db = Delta(now.cpus[i].busy, before->cpus[i].busy);
                const uint64_t dt = db + Delta(now.cpus[i].idle, before->cpus[i].idle);
                p.cores[i].push(dt ? static_cast<float>(100.0 * static_cast<double>(db) / static_cast<double>(dt)) : 0.0f);
                busy += db;
                total += dt;
            }
            l.cpu = total ? 100.0 * static_cast<double>(busy) / static_cast<double>(total) : 0.0;
            p.a.push(static_cast<float>(l.cpu));
            l.valid = true;
            break;
        }
        case PanelKind::Memory:
            l.available = now.availablePhysical;
            l.inUse = m.totalPhysical > now.availablePhysical ? m.totalPhysical - now.availablePhysical : 0;
            l.committed = now.commitTotal;
            l.commitLimit = now.commitLimit;
            l.hasDetails = now.hasDetails;
            if (now.hasDetails) {
                l.cached = now.cached;
                l.pagedPool = now.pagedPool;
                l.nonPagedPool = now.nonPagedPool;
            }
            if (!rates) break; // keep every ring on the same clock as the CPU's
            p.a.push(static_cast<float>(l.inUse));
            l.valid = true;
            break;
        case PanelKind::Disk: {
            if (!rates || p.device >= now.disks.size() || !now.disks[p.device].ok) break;
            const int j = PreviousIndex(m.disks, before->machine->disks, p.device, [](const auto& d) { return d.number; });
            if (j < 0 || static_cast<size_t>(j) >= before->disks.size() || !before->disks[j].ok) break;
            const auto& d = now.disks[p.device];
            const auto& o = before->disks[j];
            const uint64_t query = Delta(d.queryTime, o.queryTime);
            const uint64_t idle = Delta(d.idleTime, o.idleTime);
            l.active = query ? std::clamp(100.0 * (1.0 - static_cast<double>(idle) / static_cast<double>(query)), 0.0, 100.0) : 0.0;
            l.readBps = static_cast<double>(Delta(d.bytesRead, o.bytesRead)) / seconds;
            l.writeBps = static_cast<double>(Delta(d.bytesWritten, o.bytesWritten)) / seconds;
            const uint64_t ops = Delta(d.reads + d.writes, o.reads + o.writes);
            const uint64_t busyTime = Delta(d.readTime, o.readTime) + Delta(d.writeTime, o.writeTime);
            l.responseMs = ops ? static_cast<double>(busyTime) / static_cast<double>(ops) / 10'000.0 : 0.0;
            p.a.push(static_cast<float>(l.active));
            p.b.push(static_cast<float>(l.readBps));
            p.c.push(static_cast<float>(l.writeBps));
            l.valid = true;
            break;
        }
        case PanelKind::Network: {
            if (!rates || p.device >= now.networks.size() || !now.networks[p.device].ok) break;
            const int j = PreviousIndex(m.networks, before->machine->networks, p.device, [](const auto& n) { return n.luid; });
            if (j < 0 || static_cast<size_t>(j) >= before->networks.size() || !before->networks[j].ok) break;
            const auto& n = now.networks[p.device];
            const auto& o = before->networks[j];
            l.sendBits = 8.0 * static_cast<double>(Delta(n.outOctets, o.outOctets)) / seconds;
            l.receiveBits = 8.0 * static_cast<double>(Delta(n.inOctets, o.inOctets)) / seconds;
            p.a.push(static_cast<float>(l.sendBits));
            p.b.push(static_cast<float>(l.receiveBits));
            l.valid = true;
            break;
        }
        case PanelKind::Gpu: {
            if (!rates || p.device >= now.gpus.size() || !now.gpus[p.device].ok) break;
            const int j = PreviousIndex(m.gpus, before->machine->gpus, p.device, [](const auto& g) { return GpuKey(g.luid); });
            if (j < 0 || static_cast<size_t>(j) >= before->gpus.size() || !before->gpus[j].ok) break;
            const auto& g = now.gpus[p.device];
            const auto& o = before->gpus[j];
            const auto& engines = m.gpus[p.device].engineOfNode;
            l.engine.fill(-1.0);
            l.gpu = 0.0;
            for (size_t node = 0; node < g.nodeRunning.size() && node < o.nodeRunning.size() && node < engines.size(); ++node) {
                const double busy = std::min(100.0, 100.0 * static_cast<double>(Delta(g.nodeRunning[node], o.nodeRunning[node])) /
                                                        static_cast<double>(elapsed));
                double& engine = l.engine[engines[node]];
                engine = std::max(engine, busy);
                l.gpu = std::max(l.gpu, busy); // Task Manager's headline: the busiest engine
            }
            l.dedicated = g.dedicatedUsed;
            l.shared = g.sharedUsed;
            p.a.push(static_cast<float>(l.gpu));
            p.b.push(static_cast<float>(l.dedicated));
            l.valid = true;
            break;
        }
        }
    }
}

} // namespace lask
