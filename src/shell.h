// Where Lask Manager meets the Windows shell: the notification-area icon, starting with Windows
// (tray mode), and the opt-in "replace Task Manager" hook.
#pragma once

#include <windows.h>
#include <shellapi.h>

namespace lask {

class TrayIcon {
public:
    // Adds the icon (or re-adds it after Explorer restarted). Clicks arrive as `message` with
    // NOTIFYICON_VERSION_4 parameters: LOWORD(lParam) is the event.
    bool show(HWND owner, UINT message, HICON icon, const wchar_t* tip);
    void hide();
    bool visible() const { return added_; }

private:
    NOTIFYICONDATAW data_{};
    bool added_ = false;
};

// HKCU\...\Run\LaskManager = "<exe>" --tray. Per user, no admin needed.
bool SetStartWithWindows(bool on);

// HKLM\...\Image File Execution Options\taskmgr.exe\Debugger. Windows then starts this exe for
// Ctrl+Shift+Esc, the Ctrl+Alt+Del screen and the taskbar's Task Manager item, with taskmgr.exe's
// path as an extra argument. Reading works unelevated; changing it needs administrator rights.
bool IsReplacingTaskManager();
DWORD SetReplaceTaskManager(bool on);

} // namespace lask
