#include "facts.h"

#include <commctrl.h>
#include <sddl.h>

#include <cwchar>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lask {
namespace {

constexpr size_t kBatchSize = 16;
constexpr ULONGLONG kForgetAfterMs = 60'000; // a record nobody has wanted for a minute is dropped
constexpr ULONGLONG kEvictEveryMs = 10'000;

struct Wanted {
    std::vector<Identity> ids;
    std::vector<Identity> others;
    uint32_t kinds = 0;
    int iconSize = 16;
};

bool SameCreateTime(HANDLE process, int64_t createTime) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return true; // cannot tell; trust the PID
    return ((static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime) == createTime;
}

USHORT Machine(HANDLE process) {
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN, nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!IsWow64Process2(process, &processMachine, &nativeMachine)) return 0;
    return processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine; // not WOW64 = native
}

std::wstring ReadDescription(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_LOCALISED, path.c_str(), &ignored);
    if (!size) return {};
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoExW(FILE_VER_GET_LOCALISED, path.c_str(), 0, size, data.data())) return {};

    struct Translation {
        WORD language, codePage;
    };
    Translation* translations = nullptr;
    UINT bytes = 0;
    // The file's own language first, then the usual suspects for files with a broken table.
    Translation candidates[3] = {{0x0409, 0x04B0}, {0x0409, 0x04E4}, {0x0000, 0x04B0}};
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &bytes) &&
        bytes >= sizeof(Translation))
        candidates[0] = translations[0];
    for (const Translation& t : candidates) {
        wchar_t key[64];
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", t.language, t.codePage);
        wchar_t* value = nullptr;
        UINT chars = 0;
        if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &chars) && value && chars > 1) {
            std::wstring s(value, wcsnlen(value, chars));
            while (!s.empty() && s.back() == L' ') s.pop_back();
            return s;
        }
    }
    return {};
}

// Everything below runs on the enrichment thread only.
class Worker {
public:
    // One pass over the wanted set; stops early when the UI publishes a newer one.
    template <class StillWanted, class Post>
    void pass(const Wanted& wanted, StillWanted stillWanted, Post post) {
        if (wanted.iconSize != iconSize_) {
            iconSize_ = wanted.iconSize;
            iconsSent_.clear(); // the UI dropped its image list; send every icon again at the new size
        }
        const ULONGLONG now = GetTickCount64();
        auto batch = std::make_unique<FactsBatch>();
        for (const Identity& id : wanted.ids) {
            if (!stillWanted()) break;
            Record& r = records_[id];
            r.lastWanted = now;
            const uint32_t missing = wanted.kinds & ~(r.facts ? r.facts->fetched : 0u);
            if (missing) r.facts = fetch(id, r.facts, missing);

            HICON icon = nullptr;
            if (!r.facts->path.empty() && iconsSent_.insert(r.facts->path).second) icon = extractIcon(r.facts->path);
            if (missing || icon) batch->push_back({id, r.facts, icon});
            if (batch->size() >= kBatchSize) {
                post(batch.release());
                batch = std::make_unique<FactsBatch>();
            }
        }
        for (const Identity& id : wanted.others) {
            if (!stillWanted()) break;
            Record& r = records_[id];
            r.lastWanted = now;
            if (r.facts && (r.facts->fetched & (kFactPath | kFactBasic))) continue;
            r.facts = fetchPath(id, r.facts);
            batch->push_back({id, r.facts, nullptr});
            if (batch->size() >= kBatchSize) {
                post(batch.release());
                batch = std::make_unique<FactsBatch>();
            }
        }
        if (!batch->empty()) post(batch.release());
        evict(now);
    }

private:
    struct Record {
        std::shared_ptr<const ProcessFacts> facts;
        ULONGLONG lastWanted = 0;
    };

    std::shared_ptr<const ProcessFacts> fetch(const Identity& id, const std::shared_ptr<const ProcessFacts>& previous,
                                              uint32_t missing) {
        auto f = std::make_shared<ProcessFacts>(previous ? *previous : ProcessFacts{});
        f->fetched |= missing | ((missing & kFactBasic) ? kFactPath : 0u);

        HANDLE h = id.pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id.pid) : nullptr;
        if (h && !SameCreateTime(h, id.createTime)) {
            CloseHandle(h); // the PID was recycled; these facts would describe someone else
            return f;
        }
        if (missing & kFactBasic) {
            f->path = imagePath(h, id.pid);
            if (!f->path.empty()) f->description = description(f->path);
            if (h) {
                readToken(h, *f);
                f->machine = Machine(h);
            }
        }
        if ((missing & kFactCommandLine) && h) QueryCommandLine(h, f->commandLine);
        if (h) CloseHandle(h);
        return f;
    }

    // Path only, without opening the process. There is no create-time check here: the PID is from
    // the current snapshot, and a wrong guess only misfiles one row until its basic facts arrive.
    std::shared_ptr<const ProcessFacts> fetchPath(const Identity& id,
                                                  const std::shared_ptr<const ProcessFacts>& previous) {
        auto f = std::make_shared<ProcessFacts>(previous ? *previous : ProcessFacts{});
        f->fetched |= kFactPath;
        std::wstring nt;
        if (id.pid && QueryImageNtPath(id.pid, nt)) f->path = dosPath(nt);
        return f;
    }

    std::wstring imagePath(HANDLE h, uint32_t pid) {
        if (h) {
            pathBuffer_.resize(32768);
            DWORD size = static_cast<DWORD>(pathBuffer_.size());
            if (QueryFullProcessImageNameW(h, 0, pathBuffer_.data(), &size)) return {pathBuffer_.data(), size};
        }
        // Services and other users' processes usually refuse even limited access when we are not
        // elevated; the kernel still tells us their image path by PID.
        std::wstring nt;
        if (!pid || !QueryImageNtPath(pid, nt)) return {};
        return dosPath(nt);
    }

    // "\Device\HarddiskVolume3\x" -> "C:\x". Unknown devices keep a GLOBALROOT path, which the
    // file APIs (version info, icons) still accept.
    std::wstring dosPath(const std::wstring& nt) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            for (const auto& [device, drive] : devices_) {
                if (nt.size() > device.size() && nt.compare(0, device.size(), device) == 0 && nt[device.size()] == L'\\')
                    return drive + nt.substr(device.size());
            }
            if (attempt == 0 && !loadDevices()) break;
        }
        return L"\\\\?\\GLOBALROOT" + nt;
    }

    // Returns true if the device map changed (a drive was mounted since the last look).
    bool loadDevices() {
        const DWORD drives = GetLogicalDrives();
        if (drives == devicesFor_ && !devices_.empty()) return false;
        devicesFor_ = drives;
        devices_.clear();
        for (int i = 0; i < 26; ++i) {
            if (!(drives & (1u << i))) continue;
            const wchar_t drive[3] = {static_cast<wchar_t>(L'A' + i), L':', L'\0'};
            wchar_t target[MAX_PATH];
            if (QueryDosDeviceW(drive, target, MAX_PATH)) devices_.emplace_back(target, drive);
        }
        return true;
    }

    const std::wstring& description(const std::wstring& path) {
        auto it = descriptions_.find(path);
        if (it == descriptions_.end()) it = descriptions_.emplace(path, ReadDescription(path)).first;
        return it->second;
    }

    void readToken(HANDLE process, ProcessFacts& f) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return;
        alignas(TOKEN_USER) std::byte buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
        DWORD size = 0;
        if (GetTokenInformation(token, TokenUser, buffer, sizeof buffer, &size))
            f.user = account(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid);
        TOKEN_ELEVATION elevation{};
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size))
            f.elevated = elevation.TokenIsElevated ? 1 : 0;
        CloseHandle(token);
    }

    // LookupAccountSid can go to a domain controller, so every SID is looked up once.
    const std::wstring& account(PSID sid) {
        std::string key(static_cast<const char*>(sid), GetLengthSid(sid));
        auto it = accounts_.find(key);
        if (it != accounts_.end()) return it->second;

        wchar_t name[256], domain[256];
        DWORD nameLen = static_cast<DWORD>(std::size(name)), domainLen = static_cast<DWORD>(std::size(domain));
        SID_NAME_USE use{};
        std::wstring result;
        if (LookupAccountSidW(nullptr, sid, name, &nameLen, domain, &domainLen, &use)) {
            result = name;
        } else if (wchar_t* text = nullptr; ConvertSidToStringSidW(sid, &text)) {
            result = text;
            LocalFree(text);
        }
        return accounts_.emplace(std::move(key), std::move(result)).first->second;
    }

    HICON extractIcon(const std::wstring& path) const {
        HICON icon = nullptr;
        const UINT n = PrivateExtractIconsW(path.c_str(), 0, iconSize_, iconSize_, &icon, nullptr, 1, LR_DEFAULTCOLOR);
        return n != 0 && n != 0xFFFFFFFFu ? icon : nullptr;
    }

    void evict(ULONGLONG now) {
        if (now - lastEvict_ < kEvictEveryMs) return;
        lastEvict_ = now;
        std::erase_if(records_, [now](const auto& kv) { return now - kv.second.lastWanted > kForgetAfterMs; });
    }

    std::unordered_map<Identity, Record, IdentityHash> records_;
    std::unordered_map<std::wstring, std::wstring> descriptions_; // by image path
    std::unordered_map<std::string, std::wstring> accounts_;      // by binary SID
    std::unordered_set<std::wstring> iconsSent_;                  // paths whose icon the UI already has
    std::vector<std::pair<std::wstring, std::wstring>> devices_;  // NT device -> "C:"
    std::vector<wchar_t> pathBuffer_;
    DWORD devicesFor_ = 0;
    int iconSize_ = 0;
    ULONGLONG lastEvict_ = 0;
};

} // namespace

struct Enricher::State {
    std::atomic<std::shared_ptr<const Wanted>> wanted;
    std::atomic<bool> stop{false};
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HWND notify = nullptr;
    UINT message = 0;

    ~State() {
        CloseHandle(wake);
        CloseHandle(done);
    }
};

void DestroyBatch(FactsBatch* batch) {
    if (!batch) return;
    for (const auto& u : *batch)
        if (u.icon) DestroyIcon(u.icon);
    delete batch;
}

Enricher::~Enricher() {
    stop();
}

void Enricher::start(HWND notify, UINT message) {
    stop();
    auto s = std::make_shared<State>();
    s->notify = notify;
    s->message = message;
    state_ = s;
    // Detached on purpose: LookupAccountSid or a file read can hang on a sick machine, and quitting
    // must not wait for it. The thread owns a reference to the state it touches.
    std::thread([s] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Worker worker;
        while (!s->stop.load(std::memory_order_relaxed)) {
            if (auto wanted = s->wanted.load(std::memory_order_acquire)) {
                worker.pass(
                    *wanted,
                    [&] { return !s->stop.load(std::memory_order_relaxed) && s->wanted.load(std::memory_order_acquire) == wanted; },
                    [&](FactsBatch* batch) {
                        if (!PostMessageW(s->notify, s->message, 0, reinterpret_cast<LPARAM>(batch))) DestroyBatch(batch);
                    });
            }
            WaitForSingleObject(s->wake, static_cast<DWORD>(kEvictEveryMs));
        }
        SetEvent(s->done);
    }).detach();
}

void Enricher::stop() {
    if (!state_) return;
    state_->stop.store(true, std::memory_order_relaxed);
    SetEvent(state_->wake);
    WaitForSingleObject(state_->done, 300);
    state_.reset();
}

void Enricher::want(std::vector<Identity> ids, std::vector<Identity> others, uint32_t kinds, int iconSize) {
    if (!state_) return;
    state_->wanted.store(
        std::make_shared<const Wanted>(Wanted{std::move(ids), std::move(others), kinds, iconSize}),
        std::memory_order_release);
    SetEvent(state_->wake);
}

// ---- FactCache (UI thread) ---------------------------------------------------------------------

FactCache::~FactCache() {
    if (images_) ImageList_Destroy(images_);
}

const FactEntry* FactCache::find(const Identity& id) const {
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

void FactCache::apply(FactsBatch* batch) {
    for (const FactsUpdate& u : *batch) {
        const std::wstring& path = u.facts->path;
        if (u.icon && images_ && !iconByPath_.contains(path)) {
            const int image = ImageList_AddIcon(images_, u.icon);
            if (image >= 0) {
                iconByPath_.emplace(path, image);
                for (auto& [id, entry] : entries_)
                    if (entry.facts && entry.facts->path == path) entry.image = image;
            }
        }
        FactEntry& entry = entries_[u.id];
        entry.facts = u.facts;
        const auto icon = path.empty() ? iconByPath_.end() : iconByPath_.find(path);
        entry.image = icon == iconByPath_.end() ? 0 : icon->second;
    }
    DestroyBatch(batch); // the image list keeps its own copies of the icons
}

void FactCache::sweep(const Snapshot& snapshot) {
    std::unordered_set<Identity, IdentityHash> live;
    live.reserve(snapshot.rows.size());
    for (const auto& r : snapshot.rows) live.insert({r.pid, r.createTime});
    std::erase_if(entries_, [&live](const auto& kv) { return !live.contains(kv.first); });
}

bool FactCache::createImages(UINT dpi) {
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (images_ && size == iconSize_) return false;
    iconSize_ = size;
    HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 64, 64);
    auto generic = static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, size, size, LR_SHARED));
    ImageList_AddIcon(images, generic); // index 0: processes without an icon, or not fetched yet
    if (images_) ImageList_Destroy(images_);
    images_ = images;
    iconByPath_.clear();
    for (auto& [id, entry] : entries_) entry.image = 0;
    return true;
}

} // namespace lask
