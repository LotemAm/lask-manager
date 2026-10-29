// The Processes tab: Task Manager's grouped view. Apps (processes with a window), Background
// processes and Windows processes; a process and its same-named descendants fold into one
// expandable row that also lists the app's windows; CPU, memory and disk cells are shaded by load.
//
// The groups are flat rows in the same owner-data ListView, not ListView groups: virtual-mode
// groups are not supported, and flat rows keep keyboard navigation and selection simple.
#pragma once

#include "facts.h"
#include "snapshot.h"
#include "ui.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lask {

class ProcessesTab {
public:
    // All three must outlive the tab; `filter` is the filter box's text buffer.
    ProcessesTab(const Chrome& chrome, const FactCache& facts, const wchar_t* filter);

    void create(HWND parent, int id);
    HWND grid() const { return grid_; }

    // A new snapshot: regroup, sort, keep the selection. `rates` is aligned with snapshot->rows and
    // must stay alive and unchanged until the next update().
    void update(std::shared_ptr<const Snapshot> snapshot, const std::vector<RowRates>* rates);
    void refilter();     // the filter text changed
    void factsArrived(); // names and icons may have changed; regrouping waits for the next tick

    // Processes whose rows are on screen (or nearly), for the enrichment thread.
    void appendVisible(std::vector<Identity>& out) const;
    std::optional<ActionTarget> selection() const;

    // Everything from the grid that main.cpp does not handle itself.
    LRESULT onNotify(NMHDR* h);

private:
    enum class Kind : uint8_t { Group, Entry, Process, Window };
    enum Group : uint8_t { kApps, kBackground, kWindows, kGroupCount };
    enum class Col : int { Name, Status, Pid, Cpu, Memory, Disk, Count };

    struct Entry {
        int root = 0;                         // snapshot row of the process the others fold into
        int first = 0, count = 0;             // range in members_; the root comes first
        int firstWindow = 0, windowCount = 0; // range in windows_
        uint8_t group = kBackground;
        double cpu = -1.0;
        uint64_t memory = 0;
        double disk = -1.0;
        bool hung = false;
        bool suspended = false;
    };

    struct Row {
        Kind kind;
        uint8_t group;
        int entry; // index into entries_; -1 for group rows
        int item;  // Entry/Process: snapshot row; Window: index into snapshot windows; Group: entry count
    };

    // What the selection is, in terms that survive a regroup.
    struct Key {
        Kind kind = Kind::Group;
        uint8_t group = 0;
        Identity id;
        HWND window = nullptr;
    };

    static LRESULT CALLBACK KeySubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    const ProcessRow& processAt(int r) const { return snapshot_->rows[r]; }
    Identity identityAt(int r) const { return {processAt(r).pid, processAt(r).createTime}; }
    const FactEntry* factsAt(int r) const { return facts_.find(identityAt(r)); }
    const wchar_t* friendlyName(int r) const;
    int windowOwner(int windowIndex) const; // snapshot row, or -1
    bool expandable(int e) const { return entries_[e].count > 1 || entries_[e].windowCount > 0; }
    bool expanded(int e) const { return expanded_.contains(identityAt(entries_[e].root)); }
    bool isWindowsProcess(int r) const;
    bool entryMatches(int e) const;
    bool processHung(int r, const Entry& e) const;

    void regroup();
    void buildRows();
    void apply(const std::optional<Key>& keep, bool invalidateAll);
    std::optional<Key> selectedKey() const;
    int indexOf(const Key& key) const;
    void select(int index);
    void toggle(int index);
    bool onChevron(int index, POINT pt) const;
    RECT chevronRect(int index) const;

    int compareEntries(const Entry& a, const Entry& b) const;
    int compareProcesses(int a, int b) const;
    void updateSortArrows();

    void dispInfo(NMLVDISPINFOW* info);
    const wchar_t* text(const Row& row, Col col);
    int heatLevel(const Row& row, Col col) const;
    LRESULT customDraw(NMLVCUSTOMDRAW* cd);
    void drawGroup(NMLVCUSTOMDRAW* cd, int index);
    void drawChevron(HDC dc, int index);
    bool onKey(WPARAM key);

    const Chrome& chrome_;
    const FactCache& facts_;
    const wchar_t* filter_;
    HWND grid_ = nullptr;
    std::wstring windowsDir_; // "C:\Windows\", for the Windows processes rule

    std::shared_ptr<const Snapshot> snapshot_;
    const std::vector<RowRates>* rates_ = nullptr;
    std::vector<Entry> entries_;
    std::vector<int> members_; // snapshot rows, grouped by entry
    std::vector<int> windows_; // snapshot window indices, grouped by entry (z-order within each)
    std::vector<Row> rows_;    // what the grid shows
    std::unordered_set<Identity, IdentityHash> expanded_; // by root identity

    Col sortColumn_ = Col::Cpu;
    bool sortDescending_ = true;

    // Scratch kept between ticks so a warm regroup allocates nothing.
    std::unordered_map<uint32_t, int> pidIndex_;
    std::vector<int> order_, owner_, entryOf_, cursor_, sortedEntries_, sortedMembers_;
    std::vector<uint8_t> hasWindow_;
    wchar_t text_[192] = {};
};

} // namespace lask
