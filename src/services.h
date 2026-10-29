// Windows services: the list (enumerated by the sampler every 5th tick while the Services tab is
// visible), their configuration (fetched once per service on a worker), and start / stop / restart
// (on a worker; most services need administrator rights).
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lask {

struct ServiceInfo {
    std::wstring name;        // the key: SCM service name
    std::wstring displayName;
    DWORD state = 0;          // SERVICE_RUNNING, SERVICE_STOPPED, ...
    DWORD pid = 0;            // 0 when not running
    DWORD controlsAccepted = 0;
};

// Immutable once published, like a snapshot; shared by every snapshot until the next refresh.
using ServiceList = std::vector<ServiceInfo>;

// Sampler thread. `scm` needs SC_MANAGER_ENUMERATE_SERVICE. Null on failure.
std::shared_ptr<const ServiceList> ReadServices(SC_HANDLE scm);

struct ServiceConfig {
    std::wstring description;
    std::wstring startup; // "Automatic (Delayed Start)", "Manual (Trigger Start)", "Disabled", ...
    std::wstring group;   // svchost group from "-k <group>" in the image path
};

using ServiceConfigBatch = std::vector<std::pair<std::wstring, ServiceConfig>>;

// Fetches the configuration of `names` on a detached below-normal thread and posts
// ServiceConfigBatch* (owned by the receiver) to `notify` in chunks.
void LoadServiceConfigs(HWND notify, UINT message, std::vector<std::wstring> names);

enum class ServiceAction { Start, Stop, Restart };

// Blocking; call on a worker. Restart stops, waits up to 30 s for the service to stop, then starts.
// Returns 0 or a Win32 error (ERROR_ACCESS_DENIED when not elevated, typically).
DWORD RunServiceAction(const std::wstring& name, ServiceAction action);

const wchar_t* ServiceStateName(DWORD state);

} // namespace lask
