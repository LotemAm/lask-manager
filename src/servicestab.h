// The Services tab: a flat, sortable list of Win32 services with a status dot. The list comes from
// the snapshot (the sampler enumerates every 5th tick while this tab is visible); descriptions,
// startup types and svchost groups are fetched once per service on a worker.
#pragma once

#include "services.h"
#include "ui.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lask {

class ServicesTab {
public:
    // Both must outlive the tab; `filter` is the filter box's text buffer.
    ServicesTab(const Chrome& chrome, const wchar_t* filter);

    // Configuration batches are posted to `parent` as `configMessage`; pass them to onConfigs().
    void create(HWND parent, int id, UINT configMessage);
    bool created() const { return grid_ != nullptr; }
    HWND grid() const { return grid_; }

    void update(std::shared_ptr<const ServiceList> services); // cheap when the list is unchanged
    void refilter();
    void onConfigs(ServiceConfigBatch* batch); // takes ownership
    std::optional<ServiceInfo> selection() const;
    LRESULT onNotify(NMHDR* h);

private:
    enum class Col : int { Name, Pid, Description, Status, Startup, Group, Details, Count };

    struct Row {
        const ServiceInfo* info;
        const ServiceConfig* config; // null until fetched
    };

    void rebuild();
    void requestConfigs();
    void updateSortArrows();
    const wchar_t* text(const Row& row, Col col);
    LRESULT customDraw(NMLVCUSTOMDRAW* cd);

    const Chrome& chrome_;
    const wchar_t* filter_;
    HWND grid_ = nullptr;
    HWND parent_ = nullptr;
    UINT configMessage_ = 0;

    std::shared_ptr<const ServiceList> services_;
    std::vector<Row> rows_;
    std::unordered_map<std::wstring, ServiceConfig> configs_; // by service name; never erased
    std::unordered_set<std::wstring> pending_;                // asked for, not back yet
    Col sortColumn_ = Col::Name;
    bool sortDescending_ = false;
    wchar_t text_[160] = {};
};

} // namespace lask
