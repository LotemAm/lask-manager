#include "counters.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <powerbase.h>
#include <psapi.h>
#include <winioctl.h>
#include <winternl.h>
#include <d3dkmthk.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace lask {
namespace {

constexpr uint32_t kRediscoverDisksEvery = 60;   // ticks
constexpr uint32_t kRediscoverNetworksEvery = 5; // adapters come and go with Wi-Fi and VPNs
constexpr uint32_t kMaxPhysicalDrives = 32;

// Documented on MSDN for CallNtPowerInformation(ProcessorInformation) but not in any header.
struct ProcessorPowerInformation {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

// ---- iphlpapi and powrprof: loaded on the sampler thread on first use, so their DLLs (and
// iphlpapi's dependencies) never load during startup -------------------------------------------

using GetIfTable2Fn = NETIO_STATUS(WINAPI*)(PMIB_IF_TABLE2*);
using FreeMibTableFn = VOID(WINAPI*)(PVOID);
using GetIfEntry2Fn = NETIO_STATUS(WINAPI*)(PMIB_IF_ROW2);
using CallNtPowerInformationFn = NTSTATUS(WINAPI*)(POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID, ULONG);

struct LateApis {
    GetIfTable2Fn getIfTable2 = nullptr;
    FreeMibTableFn freeMibTable = nullptr;
    GetIfEntry2Fn getIfEntry2 = nullptr;
    CallNtPowerInformationFn callNtPowerInformation = nullptr;
};

const LateApis& Late() {
    static const LateApis apis = [] {
        LateApis a;
        if (HMODULE ip = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            a.getIfTable2 = reinterpret_cast<GetIfTable2Fn>(GetProcAddress(ip, "GetIfTable2"));
            a.freeMibTable = reinterpret_cast<FreeMibTableFn>(GetProcAddress(ip, "FreeMibTable"));
            a.getIfEntry2 = reinterpret_cast<GetIfEntry2Fn>(GetProcAddress(ip, "GetIfEntry2"));
        }
        if (HMODULE power = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            a.callNtPowerInformation =
                reinterpret_cast<CallNtPowerInformationFn>(GetProcAddress(power, "CallNtPowerInformation"));
        return a;
    }();
    return apis;
}

// ---- GPU: D3DKMT from gdi32, looked up at run time so a missing export fails soft ---------------

using EnumAdapters2Fn = NTSTATUS(APIENTRY*)(const D3DKMT_ENUMADAPTERS2*);
using QueryAdapterInfoFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
using CloseAdapterFn = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);
using QueryStatisticsFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);

struct Kmt {
    EnumAdapters2Fn enumAdapters2 = nullptr;
    QueryAdapterInfoFn queryAdapterInfo = nullptr;
    CloseAdapterFn closeAdapter = nullptr;
    QueryStatisticsFn queryStatistics = nullptr;
    bool ok() const { return enumAdapters2 && queryAdapterInfo && closeAdapter && queryStatistics; }
};

const Kmt& KmtApi() {
    static const Kmt kmt = [] {
        Kmt k;
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) return k;
        k.enumAdapters2 = reinterpret_cast<EnumAdapters2Fn>(GetProcAddress(gdi, "D3DKMTEnumAdapters2"));
        k.queryAdapterInfo = reinterpret_cast<QueryAdapterInfoFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
        k.closeAdapter = reinterpret_cast<CloseAdapterFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        k.queryStatistics = reinterpret_cast<QueryStatisticsFn>(GetProcAddress(gdi, "D3DKMTQueryStatistics"));
        return k;
    }();
    return kmt;
}

bool QueryStatistics(D3DKMT_QUERYSTATISTICS& q) {
    return KmtApi().queryStatistics(&q) >= 0;
}

template <class T>
bool QueryAdapter(D3DKMT_HANDLE adapter, KMTQUERYADAPTERINFOTYPE type, T& out) {
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = adapter;
    q.Type = type;
    q.pPrivateDriverData = &out;
    q.PrivateDriverDataSize = sizeof out;
    return KmtApi().queryAdapterInfo(&q) >= 0;
}

std::vector<Machine::Gpu> DiscoverGpus() {
    std::vector<Machine::Gpu> gpus;
    const Kmt& kmt = KmtApi();
    if (!kmt.ok()) return gpus;
    D3DKMT_ADAPTERINFO adapters[MAX_ENUM_ADAPTERS] = {};
    D3DKMT_ENUMADAPTERS2 e{};
    e.NumAdapters = MAX_ENUM_ADAPTERS;
    e.pAdapters = adapters;
    if (kmt.enumAdapters2(&e) < 0) return gpus;

    for (ULONG i = 0; i < e.NumAdapters; ++i) {
        const D3DKMT_ADAPTERINFO& a = adapters[i];
        D3DKMT_ADAPTERTYPE type{};
        // The Basic Render Driver and display-only adapters have nothing to show.
        const bool real = QueryAdapter(a.hAdapter, KMTQAITYPE_ADAPTERTYPE, type) && type.RenderSupported &&
                          !type.SoftwareDevice;
        if (real) {
            Machine::Gpu gpu;
            gpu.luid = a.AdapterLuid;
            D3DKMT_ADAPTERREGISTRYINFO registry{};
            if (QueryAdapter(a.hAdapter, KMTQAITYPE_ADAPTERREGISTRYINFO, registry)) gpu.name = registry.AdapterString;
            if (gpu.name.empty()) gpu.name = L"GPU";

            D3DKMT_QUERYSTATISTICS q{};
            q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
            q.AdapterLuid = a.AdapterLuid;
            if (QueryStatistics(q)) {
                const ULONG nodes = q.QueryResult.AdapterInformation.NodeCount;
                const ULONG segments = q.QueryResult.AdapterInformation.NbSegments;
                for (ULONG n = 0; n < nodes; ++n) {
                    D3DKMT_NODEMETADATA meta{};
                    meta.NodeOrdinalAndAdapterIndex = n;
                    const bool known = QueryAdapter(a.hAdapter, KMTQAITYPE_NODEMETADATA, meta) &&
                                       meta.NodeData.EngineType < kGpuEngineTypes;
                    gpu.engineOfNode.push_back(static_cast<uint8_t>(known ? meta.NodeData.EngineType : DXGK_ENGINE_TYPE_OTHER));
                }
                for (ULONG s = 0; s < segments; ++s) {
                    D3DKMT_QUERYSTATISTICS sq{};
                    sq.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
                    sq.AdapterLuid = a.AdapterLuid;
                    sq.QuerySegment.SegmentId = s;
                    if (!QueryStatistics(sq)) continue;
                    const auto& info = sq.QueryResult.SegmentInformation;
                    (info.Aperture ? gpu.sharedLimit : gpu.dedicatedLimit) += info.CommitLimit;
                    gpu.segments = s + 1;
                }
            }
            gpus.push_back(std::move(gpu));
        }
        D3DKMT_CLOSEADAPTER close{a.hAdapter};
        kmt.closeAdapter(&close);
    }
    return gpus;
}

// ---- disks ----------------------------------------------------------------------------------------

HANDLE OpenDevice(const wchar_t* path) {
    // No access rights: enough for the performance, geometry and storage-property IOCTLs, and it
    // works unelevated.
    return CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

const wchar_t* BusName(STORAGE_BUS_TYPE bus) {
    switch (bus) {
    case BusTypeNvme: return L"NVMe";
    case BusTypeUsb: return L"USB";
    case BusTypeSata: return L"SATA";
    case BusTypeSas: return L"SAS";
    case BusTypeRAID: return L"RAID";
    case BusTypeSd:
    case BusTypeMmc: return L"SD";
    case BusTypeFileBackedVirtual:
    case BusTypeVirtual: return L"Virtual";
    default: return nullptr;
    }
}

void DescribeDisk(HANDLE h, Machine::Disk& disk) {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    alignas(STORAGE_DEVICE_DESCRIPTOR) std::byte buffer[1024] = {};
    DWORD bytes = 0;
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, buffer, sizeof buffer, &bytes, nullptr)) {
        const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
        bus = d->BusType;
        if (d->ProductIdOffset && d->ProductIdOffset < bytes) {
            const char* product = reinterpret_cast<const char*>(buffer) + d->ProductIdOffset;
            const size_t n = strnlen(product, bytes - d->ProductIdOffset);
            disk.model.assign(product, product + n); // ASCII by definition
            while (!disk.model.empty() && disk.model.back() == L' ') disk.model.pop_back();
        }
    }

    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    const bool knowsSeek = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &seek, sizeof seek,
                                           &bytes, nullptr) != FALSE;
    const wchar_t* media = knowsSeek ? (seek.IncursSeekPenalty ? L"HDD" : L"SSD") : L"Disk";
    if (const wchar_t* busName = BusName(bus)) {
        wchar_t kind[32];
        swprintf_s(kind, L"%s (%s)", media, busName);
        disk.kind = kind;
    } else {
        disk.kind = media;
    }

    DISK_GEOMETRY_EX geometry{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof geometry, &bytes, nullptr))
        disk.size = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
}

// Which drive letters live on which physical disk (volumes spanning disks are left out).
void AssignVolumes(std::vector<Machine::Disk>& disks) {
    const DWORD letters = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(letters & (1u << i))) continue;
        const wchar_t path[] = {L'\\', L'\\', L'.', L'\\', static_cast<wchar_t>(L'A' + i), L':', L'\0'};
        HANDLE h = OpenDevice(path);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER number{};
        DWORD bytes = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof number, &bytes, nullptr) &&
            number.DeviceType == FILE_DEVICE_DISK) {
            for (auto& d : disks) {
                if (d.number != number.DeviceNumber) continue;
                if (!d.volumes.empty()) d.volumes += L' ';
                d.volumes += path + 4; // "C:"
            }
        }
        CloseHandle(h);
    }
}

// ---- networks ---------------------------------------------------------------------------------------

// Physical adapters that are up: what Task Manager lists. Virtual switches, filters, loopback and
// disconnected adapters are left out.
std::vector<Machine::Network> DiscoverNetworks() {
    std::vector<Machine::Network> out;
    const LateApis& api = Late();
    MIB_IF_TABLE2* table = nullptr;
    if (!api.getIfTable2 || !api.freeMibTable || api.getIfTable2(&table) != NO_ERROR) return out;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (!row.InterfaceAndOperStatusFlags.HardwareInterface || row.InterfaceAndOperStatusFlags.FilterInterface)
            continue;
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.OperStatus != IfOperStatusUp) continue;
        Machine::Network n;
        n.luid = row.InterfaceLuid.Value;
        n.alias = row.Alias;
        n.description = row.Description;
        n.linkBitsPerSec = std::max(row.ReceiveLinkSpeed, row.TransmitLinkSpeed);
        n.wireless = row.Type == IF_TYPE_IEEE80211;
        out.push_back(std::move(n));
    }
    api.freeMibTable(table);
    return out;
}

bool SameNetworks(const std::vector<Machine::Network>& a, const std::vector<Machine::Network>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
        return x.luid == y.luid && x.linkBitsPerSec == y.linkBitsPerSec && x.alias == y.alias;
    });
}

void ReadCpuIdentity(Machine& m) {
    constexpr wchar_t kKey[] = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    wchar_t name[128] = {};
    DWORD size = sizeof name;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) {
        m.cpuName = name;
        while (!m.cpuName.empty() && m.cpuName.back() == L' ') m.cpuName.pop_back();
    }
    DWORD mhz = 0;
    size = sizeof mhz;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &size) == ERROR_SUCCESS)
        m.baseMhz = mhz;

    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &bytes);
    std::vector<std::byte> buffer(bytes);
    if (bytes && GetLogicalProcessorInformationEx(RelationAll,
                                                  reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                                                  &bytes)) {
        for (DWORD at = 0; at < bytes;) {
            const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
            if (info->Relationship == RelationProcessorCore) ++m.cores;
            else if (info->Relationship == RelationProcessorPackage) ++m.sockets;
            at += info->Size;
        }
    }
    m.logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

    MEMORYSTATUSEX memory{sizeof memory};
    if (GlobalMemoryStatusEx(&memory)) m.totalPhysical = memory.ullTotalPhys;
}

} // namespace

CounterReader::CounterReader() {
    discover();
}

CounterReader::~CounterReader() {
    for (HANDLE h : diskHandles_)
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// Builds a new Machine. Snapshots that already hold the old one keep it; nothing is mutated.
void CounterReader::discover() {
    auto m = std::make_shared<Machine>();
    if (machine_) {
        m->cpuName = machine_->cpuName;
        m->baseMhz = machine_->baseMhz;
        m->sockets = machine_->sockets;
        m->cores = machine_->cores;
        m->logical = machine_->logical;
        m->totalPhysical = machine_->totalPhysical;
        m->gpus = machine_->gpus; // GPUs are fixed for the life of the process
    } else {
        ReadCpuIdentity(*m);
        m->gpus = DiscoverGpus();
    }

    for (HANDLE h : diskHandles_)
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    diskHandles_.clear();
    for (uint32_t n = 0; n < kMaxPhysicalDrives; ++n) {
        wchar_t path[32];
        swprintf_s(path, L"\\\\.\\PhysicalDrive%u", n);
        HANDLE h = OpenDevice(path);
        if (h == INVALID_HANDLE_VALUE) continue;
        Machine::Disk disk;
        disk.number = n;
        DescribeDisk(h, disk);
        m->disks.push_back(std::move(disk));
        diskHandles_.push_back(h);
    }
    AssignVolumes(m->disks);

    m->networks = DiscoverNetworks();
    machine_ = std::move(m);
}

void CounterReader::readCpus(SystemCounters& out) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si); // the current processor group: what SystemProcessorPerformanceInformation covers
    const size_t n = si.dwNumberOfProcessors;
    cpuBuffer_.resize(n * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
    ULONG returned = 0;
    if (NtQuerySystemInformation(SystemProcessorPerformanceInformation, cpuBuffer_.data(),
                                 static_cast<ULONG>(cpuBuffer_.size()), &returned) < 0)
        return;
    const auto* p = reinterpret_cast<const SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION*>(cpuBuffer_.data());
    const size_t count = returned / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION);
    out.cpus.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const auto idle = static_cast<uint64_t>(p[i].IdleTime.QuadPart);
        const auto kernel = static_cast<uint64_t>(p[i].KernelTime.QuadPart); // includes idle
        const auto user = static_cast<uint64_t>(p[i].UserTime.QuadPart);
        out.cpus[i] = {idle, (kernel >= idle ? kernel - idle : 0) + user};
    }
}

void CounterReader::readDisks(SystemCounters& out) {
    out.disks.resize(diskHandles_.size());
    for (size_t i = 0; i < diskHandles_.size(); ++i) {
        // Answered from partmgr's in-memory counters; it does not touch the device. Fails with
        // ERROR_INVALID_FUNCTION if disk counters were turned off (diskperf -n).
        DISK_PERFORMANCE perf{};
        DWORD bytes = 0;
        SystemCounters::Disk& d = out.disks[i];
        d.ok = DeviceIoControl(diskHandles_[i], IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof perf, &bytes, nullptr) != FALSE;
        if (!d.ok) continue;
        d.bytesRead = static_cast<uint64_t>(perf.BytesRead.QuadPart);
        d.bytesWritten = static_cast<uint64_t>(perf.BytesWritten.QuadPart);
        d.idleTime = static_cast<uint64_t>(perf.IdleTime.QuadPart);
        d.queryTime = static_cast<uint64_t>(perf.QueryTime.QuadPart);
        d.readTime = static_cast<uint64_t>(perf.ReadTime.QuadPart);
        d.writeTime = static_cast<uint64_t>(perf.WriteTime.QuadPart);
        d.reads = perf.ReadCount;
        d.writes = perf.WriteCount;
    }
}

void CounterReader::readNetworks(SystemCounters& out) {
    const auto& nets = machine_->networks;
    out.networks.resize(nets.size());
    for (size_t i = 0; i < nets.size(); ++i) {
        MIB_IF_ROW2 row{};
        row.InterfaceLuid.Value = nets[i].luid;
        SystemCounters::Network& n = out.networks[i];
        n.ok = Late().getIfEntry2 && Late().getIfEntry2(&row) == NO_ERROR;
        if (!n.ok) continue;
        n.inOctets = row.InOctets;
        n.outOctets = row.OutOctets;
    }
}

void CounterReader::readGpus(SystemCounters& out) {
    const auto& gpus = machine_->gpus;
    out.gpus.resize(gpus.size());
    for (size_t i = 0; i < gpus.size(); ++i) {
        const Machine::Gpu& gpu = gpus[i];
        SystemCounters::Gpu& g = out.gpus[i];
        g.ok = true;
        g.nodeRunning.resize(gpu.engineOfNode.size());
        for (size_t n = 0; n < gpu.engineOfNode.size() && g.ok; ++n) {
            D3DKMT_QUERYSTATISTICS q{};
            q.Type = D3DKMT_QUERYSTATISTICS_NODE;
            q.AdapterLuid = gpu.luid;
            q.QueryNode.NodeId = static_cast<ULONG>(n);
            g.ok = QueryStatistics(q);
            g.nodeRunning[n] = static_cast<uint64_t>(q.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart);
        }
        for (uint32_t s = 0; s < gpu.segments && g.ok; ++s) {
            D3DKMT_QUERYSTATISTICS q{};
            q.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
            q.AdapterLuid = gpu.luid;
            q.QuerySegment.SegmentId = s;
            if (!QueryStatistics(q)) continue;
            const auto& info = q.QueryResult.SegmentInformation;
            (info.Aperture ? g.sharedUsed : g.dedicatedUsed) += info.BytesResident;
        }
    }
}

void CounterReader::read(SystemCounters& out, bool details) {
    ++ticks_;
    if (ticks_ % kRediscoverDisksEvery == 0) {
        discover();
    } else if (ticks_ % kRediscoverNetworksEvery == 0) {
        auto networks = DiscoverNetworks();
        if (!SameNetworks(networks, machine_->networks)) {
            auto m = std::make_shared<Machine>(*machine_);
            m->networks = std::move(networks);
            machine_ = std::move(m);
        }
    }
    out.machine = machine_;

    readCpus(out);
    MEMORYSTATUSEX memory{sizeof memory};
    if (GlobalMemoryStatusEx(&memory)) {
        out.availablePhysical = memory.ullAvailPhys;
        out.commitLimit = memory.ullTotalPageFile;
        out.commitTotal = memory.ullTotalPageFile - memory.ullAvailPageFile;
    }
    out.uptimeMs = GetTickCount64();
    readDisks(out);
    readNetworks(out);
    readGpus(out);

    out.hasDetails = details;
    if (!details) return;
    std::vector<ProcessorPowerInformation> power(std::max<size_t>(out.cpus.size(), 1));
    if (Late().callNtPowerInformation &&
        Late().callNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
                                      static_cast<ULONG>(power.size() * sizeof(ProcessorPowerInformation))) >= 0) {
        uint64_t sum = 0;
        for (const auto& p : power) sum += p.CurrentMhz;
        out.currentMhz = static_cast<uint32_t>(sum / power.size());
    }
    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof pi;
    if (GetPerformanceInfo(&pi, sizeof pi)) {
        out.cached = static_cast<uint64_t>(pi.SystemCache) * pi.PageSize;
        out.pagedPool = static_cast<uint64_t>(pi.KernelPaged) * pi.PageSize;
        out.nonPagedPool = static_cast<uint64_t>(pi.KernelNonpaged) * pi.PageSize;
    }
}

} // namespace lask
