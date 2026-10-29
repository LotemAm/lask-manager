#include "shell.h"

#include <cwchar>
#include <string>

namespace lask {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"LaskManager";
constexpr wchar_t kIfeoKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\taskmgr.exe";

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return {path, n};
}

std::wstring Quoted(const std::wstring& path) {
    return L"\"" + path + L"\"";
}

} // namespace

bool TrayIcon::show(HWND owner, UINT message, HICON icon, const wchar_t* tip) {
    hide();
    data_ = {};
    data_.cbSize = sizeof data_;
    data_.hWnd = owner;
    data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = message;
    data_.hIcon = icon;
    wcscpy_s(data_.szTip, tip);
    if (!Shell_NotifyIconW(NIM_ADD, &data_)) return false;
    data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    added_ = true;
    return true;
}

void TrayIcon::hide() {
    if (!added_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
}

bool SetStartWithWindows(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS status;
    if (on) {
        const std::wstring command = Quoted(ExePath()) + L" --tray";
        status = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool IsReplacingTaskManager() {
    wchar_t value[MAX_PATH * 2] = {};
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kIfeoKey, L"Debugger", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return false;
    return _wcsicmp(value, Quoted(ExePath()).c_str()) == 0 || _wcsicmp(value, ExePath().c_str()) == 0;
}

DWORD SetReplaceTaskManager(bool on) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kIfeoKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return static_cast<DWORD>(status);
    if (on) {
        const std::wstring command = Quoted(ExePath());
        status = RegSetValueExW(key, L"Debugger", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"Debugger");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return static_cast<DWORD>(status);
}

} // namespace lask
