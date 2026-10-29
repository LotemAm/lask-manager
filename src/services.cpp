#include "services.h"

#include <algorithm>
#include <cwchar>
#include <thread>

namespace lask {
namespace {

constexpr size_t kConfigBatch = 32;
constexpr DWORD kRestartStopTimeoutMs = 30'000;

void CloseHandles(SC_HANDLE service, SC_HANDLE scm) {
    if (service) CloseServiceHandle(service);
    if (scm) CloseServiceHandle(scm);
}

// "...\svchost.exe -k netsvcs -p" -> "netsvcs"
std::wstring SvchostGroup(const wchar_t* path) {
    if (!path) return {};
    const wchar_t* k = wcsstr(path, L"-k ");
    if (!k) return {};
    k += 3;
    while (*k == L' ') ++k;
    const wchar_t* end = k;
    while (*end && *end != L' ' && *end != L'"') ++end;
    return {k, end};
}

// QueryServiceConfig2 into a buffer that grows on ERROR_INSUFFICIENT_BUFFER.
bool QueryConfig2(SC_HANDLE s, DWORD level, std::vector<std::byte>& buffer) {
    DWORD needed = 0;
    if (QueryServiceConfig2W(s, level, reinterpret_cast<LPBYTE>(buffer.data()), static_cast<DWORD>(buffer.size()), &needed))
        return true;
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    buffer.resize(needed);
    return QueryServiceConfig2W(s, level, reinterpret_cast<LPBYTE>(buffer.data()), static_cast<DWORD>(buffer.size()), &needed) != FALSE;
}

ServiceConfig ReadConfig(SC_HANDLE s, std::vector<std::byte>& buffer) {
    ServiceConfig c;
    DWORD needed = 0;
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(s, config, static_cast<DWORD>(buffer.size()), &needed)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return c;
        buffer.resize(needed);
        config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (!QueryServiceConfigW(s, config, static_cast<DWORD>(buffer.size()), &needed)) return c;
    }
    const DWORD startType = config->dwStartType;
    c.group = SvchostGroup(config->lpBinaryPathName); // copied out before the buffer is reused

    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    QueryServiceConfig2W(s, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, reinterpret_cast<LPBYTE>(&delayed), sizeof delayed, &needed);
    const bool triggered = QueryConfig2(s, SERVICE_CONFIG_TRIGGER_INFO, buffer) &&
                           reinterpret_cast<const SERVICE_TRIGGER_INFO*>(buffer.data())->cTriggers > 0;

    switch (startType) {
    case SERVICE_AUTO_START: c.startup = L"Automatic"; break;
    case SERVICE_DEMAND_START: c.startup = L"Manual"; break;
    case SERVICE_DISABLED: c.startup = L"Disabled"; break;
    case SERVICE_BOOT_START: c.startup = L"Boot"; break;
    case SERVICE_SYSTEM_START: c.startup = L"System"; break;
    default: break;
    }
    const bool isDelayed = startType == SERVICE_AUTO_START && delayed.fDelayedAutostart;
    if (isDelayed || (triggered && startType != SERVICE_DISABLED)) {
        c.startup += L" (";
        if (isDelayed) c.startup += L"Delayed Start";
        if (isDelayed && triggered) c.startup += L", ";
        if (triggered) c.startup += L"Trigger Start";
        c.startup += L")";
    }

    if (QueryConfig2(s, SERVICE_CONFIG_DESCRIPTION, buffer)) {
        const auto* d = reinterpret_cast<const SERVICE_DESCRIPTIONW*>(buffer.data());
        if (d->lpDescription) c.description = d->lpDescription;
    }
    return c;
}

DWORD WaitForState(SC_HANDLE s, DWORD target, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof status, &needed))
            return GetLastError();
        if (status.dwCurrentState == target) return ERROR_SUCCESS;
        if (GetTickCount64() >= deadline) return ERROR_SERVICE_REQUEST_TIMEOUT;
        Sleep(std::clamp<DWORD>(status.dwWaitHint / 10, 100, 1000)); // the SCM's own guidance
    }
}

} // namespace

std::shared_ptr<const ServiceList> ReadServices(SC_HANDLE scm) {
    std::vector<std::byte> buffer(64 * 1024);
    DWORD needed = 0, count = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD resume = 0;
        if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                                  reinterpret_cast<LPBYTE>(buffer.data()), static_cast<DWORD>(buffer.size()), &needed,
                                  &count, &resume, nullptr))
            break;
        if (GetLastError() != ERROR_MORE_DATA) return nullptr;
        buffer.resize(buffer.size() + needed + 4096); // services can appear between calls
        count = 0;
    }
    auto list = std::make_shared<ServiceList>();
    list->reserve(count);
    const auto* e = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) {
        const SERVICE_STATUS_PROCESS& st = e[i].ServiceStatusProcess;
        list->push_back({e[i].lpServiceName, e[i].lpDisplayName ? e[i].lpDisplayName : L"", st.dwCurrentState,
                         st.dwProcessId, st.dwControlsAccepted});
    }
    return list;
}

void LoadServiceConfigs(HWND notify, UINT message, std::vector<std::wstring> names) {
    std::thread([notify, message, names = std::move(names)] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        std::vector<std::byte> buffer(8 * 1024);
        auto batch = std::make_unique<ServiceConfigBatch>();
        auto post = [&] {
            if (batch->empty()) return;
            if (PostMessageW(notify, message, 0, reinterpret_cast<LPARAM>(batch.get()))) batch.release();
            batch = std::make_unique<ServiceConfigBatch>();
        };
        for (const std::wstring& name : names) {
            ServiceConfig config; // an empty one for a service we cannot open, so it is not asked for again
            if (SC_HANDLE s = scm ? OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG) : nullptr) {
                config = ReadConfig(s, buffer);
                CloseServiceHandle(s);
            }
            batch->emplace_back(name, std::move(config));
            if (batch->size() >= kConfigBatch) post();
        }
        post();
        if (scm) CloseServiceHandle(scm);
    }).detach();
}

DWORD RunServiceAction(const std::wstring& name, ServiceAction action) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    const DWORD access = SERVICE_QUERY_STATUS | (action != ServiceAction::Stop ? SERVICE_START : 0) |
                         (action != ServiceAction::Start ? SERVICE_STOP : 0);
    SC_HANDLE s = OpenServiceW(scm, name.c_str(), access);
    if (!s) {
        const DWORD error = GetLastError();
        CloseHandles(nullptr, scm);
        return error;
    }
    DWORD error = ERROR_SUCCESS;
    if (action != ServiceAction::Start) {
        SERVICE_STATUS status{};
        if (!ControlService(s, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            error = GetLastError();
        if (!error && action == ServiceAction::Restart) error = WaitForState(s, SERVICE_STOPPED, kRestartStopTimeoutMs);
    }
    if (!error && action != ServiceAction::Stop)
        if (!StartServiceW(s, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) error = GetLastError();
    CloseHandles(s, scm);
    return error;
}

const wchar_t* ServiceStateName(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING: return L"Running";
    case SERVICE_STOPPED: return L"Stopped";
    case SERVICE_START_PENDING: return L"Starting";
    case SERVICE_STOP_PENDING: return L"Stopping";
    case SERVICE_PAUSED: return L"Paused";
    case SERVICE_PAUSE_PENDING: return L"Pausing";
    case SERVICE_CONTINUE_PENDING: return L"Continuing";
    default: return L"";
    }
}

} // namespace lask
