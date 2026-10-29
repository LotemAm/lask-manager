#include "settings.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

namespace lask {
namespace {

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- a parser for exactly what SaveSettings writes: one flat object of booleans, numbers and
// arrays of numbers. Anything else is skipped. ------------------------------------------------------

struct Value {
    enum Kind { None, Bool, Number, Array } kind = None;
    bool b = false;
    double number = 0;
    std::vector<int> array;
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    template <class OnPair>
    bool object(OnPair onPair) {
        skip();
        if (!eat('{')) return false;
        for (;;) {
            skip();
            if (eat('}')) return true;
            std::string key;
            if (!string(key)) return false;
            skip();
            if (!eat(':')) return false;
            Value v = value();
            onPair(key, v);
            skip();
            if (eat(',')) continue;
            return eat('}');
        }
    }

private:
    void skip() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool eat(char c) {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    bool string(std::string& out) {
        if (!eat('"')) return false;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) ++i_;
            out += s_[i_++];
        }
        return eat('"');
    }
    bool number(double& out) {
        const char* begin = s_.c_str() + i_;
        char* end = nullptr;
        out = std::strtod(begin, &end);
        if (end == begin) return false;
        i_ += static_cast<size_t>(end - begin);
        return true;
    }
    Value value() {
        skip();
        Value v;
        if (s_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            v.kind = Value::Bool;
            v.b = true;
        } else if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            v.kind = Value::Bool;
        } else if (eat('[')) {
            v.kind = Value::Array;
            for (;;) {
                skip();
                if (eat(']')) break;
                double d = 0;
                if (!number(d)) {
                    skipBalanced(']');
                    break;
                }
                v.array.push_back(static_cast<int>(d));
                skip();
                eat(',');
            }
        } else if (number(v.number)) {
            v.kind = Value::Number;
        } else {
            std::string ignored;
            if (!string(ignored)) skipBalanced(',');
        }
        return v;
    }
    void skipBalanced(char until) {
        int depth = 0;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if ((c == '[' || c == '{')) ++depth;
            if ((c == ']' || c == '}') && depth-- == 0) return;
            if (c == until && depth == 0) return;
            ++i_;
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

void Append(std::string& out, const char* key, const std::vector<int>& values) {
    out += "  \"";
    out += key;
    out += "\": [";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(values[i]);
    }
    out += "],\n";
}

void Append(std::string& out, const char* key, long long value) {
    out += "  \"";
    out += key;
    out += "\": " + std::to_string(value) + ",\n";
}

void Append(std::string& out, const char* key, bool value) {
    out += "  \"";
    out += key;
    out += value ? "\": true,\n" : "\": false,\n";
}

} // namespace

std::wstring SettingsPath() {
    const std::wstring portable = ExeDirectory() + L"lask.json";
    if (FileExists(portable)) return portable;
    wchar_t local[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return portable;
    return std::wstring(local) + L"\\LaskManager\\settings.json";
}

bool LoadSettings(Settings& s) {
    HANDLE file = CreateFileW(SettingsPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 1 << 20) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) read = 0;
        text.resize(read);
    }
    CloseHandle(file);

    Parser parser(text);
    return parser.object([&s](const std::string& key, const Value& v) {
        auto flag = [&](const char* name, bool& field) {
            if (key == name && v.kind == Value::Bool) field = v.b;
        };
        auto integer = [&](const char* name, auto& field) {
            if (key == name && v.kind == Value::Number) field = static_cast<std::remove_reference_t<decltype(field)>>(v.number);
        };
        auto list = [&](const char* name, std::vector<int>& field) {
            if (key == name && v.kind == Value::Array) field = v.array;
        };
        flag("alwaysOnTop", s.alwaysOnTop);
        integer("refreshMs", s.refreshMs);
        flag("hardwareGraphs", s.hardwareGraphs);
        flag("tray", s.tray);
        flag("accentHeat", s.accentHeat);
        integer("tab", s.tab);
        flag("navCollapsed", s.navCollapsed);
        flag("maximized", s.maximized);
        if (key == "window" && v.kind == Value::Array && v.array.size() == 4) {
            s.window = {v.array[0], v.array[1], v.array[2], v.array[3]};
            s.hasWindow = s.window.right > s.window.left && s.window.bottom > s.window.top;
        }
        integer("graphSeconds", s.graphSeconds);
        flag("logicalProcessors", s.logicalProcessors);
        list("detailsColumns", s.detailsColumns);
        list("detailsWidths", s.detailsWidths);
        integer("detailsSort", s.detailsSort);
        flag("detailsDescending", s.detailsDescending);
        list("processesWidths", s.processesWidths);
        list("processesOrder", s.processesOrder);
        list("servicesWidths", s.servicesWidths);
        list("servicesOrder", s.servicesOrder);
    });
}

bool SaveSettings(const Settings& s) {
    std::string out = "{\n";
    Append(out, "alwaysOnTop", s.alwaysOnTop);
    Append(out, "refreshMs", static_cast<long long>(s.refreshMs));
    Append(out, "hardwareGraphs", s.hardwareGraphs);
    Append(out, "tray", s.tray);
    Append(out, "accentHeat", s.accentHeat);
    Append(out, "tab", static_cast<long long>(s.tab));
    Append(out, "navCollapsed", s.navCollapsed);
    if (s.hasWindow) Append(out, "window", std::vector<int>{s.window.left, s.window.top, s.window.right, s.window.bottom});
    Append(out, "maximized", s.maximized);
    Append(out, "graphSeconds", static_cast<long long>(s.graphSeconds));
    Append(out, "logicalProcessors", s.logicalProcessors);
    Append(out, "detailsColumns", s.detailsColumns);
    Append(out, "detailsWidths", s.detailsWidths);
    Append(out, "detailsSort", static_cast<long long>(s.detailsSort));
    Append(out, "detailsDescending", s.detailsDescending);
    Append(out, "processesWidths", s.processesWidths);
    Append(out, "processesOrder", s.processesOrder);
    Append(out, "servicesWidths", s.servicesWidths);
    Append(out, "servicesOrder", s.servicesOrder);
    out.erase(out.size() - 2, 1); // the last comma
    out += "}\n";

    const std::wstring path = SettingsPath();
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr); // may exist
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) && written == out.size();
    CloseHandle(file);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

} // namespace lask
