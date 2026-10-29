// Per-process facts that need OpenProcess or file I/O: image path, description, icon, user,
// elevation, architecture, command line. Fetched lazily on the enrichment thread, only for the
// processes the UI says it wants (rows on screen plus a small lookahead), and cached by identity.
#pragma once

#include "snapshot.h"

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lask {

enum FactKind : uint32_t {
    kFactBasic = 1u << 0,       // path, description, user, elevation, architecture
    kFactCommandLine = 1u << 1, // only while the column is visible: it is the one long string
    kFactPath = 1u << 2,        // image path alone, by PID, no OpenProcess: what grouping needs
};

// Immutable once posted; the UI keeps a shared_ptr per identity.
struct ProcessFacts {
    uint32_t fetched = 0;       // FactKind bits already attempted (attempted, not necessarily found)
    std::wstring path;          // Win32 path, empty if Windows would not say
    std::wstring description;   // FileDescription from the version resource
    std::wstring user;          // account name without the domain
    std::wstring commandLine;
    int8_t elevated = -1;       // -1 unknown, 0 no, 1 yes
    USHORT machine = 0;         // IMAGE_FILE_MACHINE_*; 0 unknown
};

struct FactsUpdate {
    Identity id;
    std::shared_ptr<const ProcessFacts> facts;
    HICON icon = nullptr; // set the first time an icon for facts->path is extracted at the current size
};

// Payload of the posted message (lParam). The receiver owns it and every icon in it.
using FactsBatch = std::vector<FactsUpdate>;
void DestroyBatch(FactsBatch* batch);

class Enricher {
public:
    Enricher() = default;
    ~Enricher();
    Enricher(const Enricher&) = delete;
    Enricher& operator=(const Enricher&) = delete;

    void start(HWND notify, UINT message);
    void stop();
    bool running() const { return state_ != nullptr; }

    // UI thread: replace the wanted set. `ids` (rows on screen, most important first) get `kinds`
    // and icons; `others` (every process the UI knows nothing about yet) only get kFactPath, after
    // `ids`, so the Processes tab can tell Windows processes from background ones. Never blocks.
    void want(std::vector<Identity> ids, std::vector<Identity> others, uint32_t kinds, int iconSize);

private:
    struct State;
    std::shared_ptr<State> state_;
};

// UI-side copy of what the enrichment thread found. UI thread only.
struct FactEntry {
    std::shared_ptr<const ProcessFacts> facts;
    int image = 0; // index into FactCache::images(); 0 is the generic application icon
};

class FactCache {
public:
    FactCache() = default;
    ~FactCache();
    FactCache(const FactCache&) = delete;
    FactCache& operator=(const FactCache&) = delete;

    // Pointers stay valid until the next sweep(): unordered_map never moves its elements.
    const FactEntry* find(const Identity& id) const;
    // Takes ownership of the batch and every icon in it.
    void apply(FactsBatch* batch);
    // Drops processes that are no longer in `snapshot`.
    void sweep(const Snapshot& snapshot);

    // Recreates the image list for `dpi`; returns false (and keeps it) if the icon size is the same.
    // Every icon then has to come again; the enrichment thread notices the size change.
    bool createImages(UINT dpi);
    HIMAGELIST images() const { return images_; }
    int iconSize() const { return iconSize_; }

private:
    std::unordered_map<Identity, FactEntry, IdentityHash> entries_;
    std::unordered_map<std::wstring, int> iconByPath_;
    HIMAGELIST images_ = nullptr;
    int iconSize_ = 0;
};

} // namespace lask
