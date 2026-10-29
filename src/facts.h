// Per-process facts that need OpenProcess or file I/O: image path, description, icon, user,
// elevation, architecture, command line. Fetched lazily on the enrichment thread, only for the
// processes the UI says it wants (rows on screen plus a small lookahead), and cached by identity.
#pragma once

#include "snapshot.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lask {

enum FactKind : uint32_t {
    kFactBasic = 1u << 0,       // path, description, user, elevation, architecture
    kFactCommandLine = 1u << 1, // only while the column is visible: it is the one long string
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

    // UI thread: replace the wanted set (most important first). Never blocks.
    void want(std::vector<Identity> ids, uint32_t kinds, int iconSize);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace lask
