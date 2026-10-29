// The settings file: a small flat JSON object. `lask.json` next to the exe if it exists (portable
// use), otherwise %LOCALAPPDATA%\LaskManager\settings.json. Unknown keys are ignored and missing
// ones keep their defaults, so old and new versions can share a file.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace lask {

struct Settings {
    bool alwaysOnTop = false;
    unsigned refreshMs = 1000; // 500, 1000 or 2000; 0 is paused
    bool hardwareGraphs = false; // Direct2D on the GPU: smoother, ~20 MB more working set
    bool tray = false;           // closing hides to the notification area; starts with Windows
    bool accentHeat = true;      // Processes heat map in the accent color; false = Task Manager's orange
    int theme = 0;               // 0 follows Windows, 1 light, 2 dark (lask::ThemeMode)
    int tab = 0;
    bool navCollapsed = false; // the nav shows icons only

    bool hasWindow = false; // the window's normal (restored) rectangle, in workspace pixels
    RECT window{};
    bool maximized = false;

    int graphSeconds = 60;
    bool logicalProcessors = false;

    std::vector<int> detailsColumns;   // Details column ids in display order (Name first)
    std::vector<int> detailsWidths;    // per Details column id, at 96 dpi
    int detailsSort = -1;              // column id; -1 = default
    bool detailsDescending = true;
    std::vector<int> processesWidths;  // per column, at 96 dpi
    std::vector<int> processesOrder;
    std::vector<int> servicesWidths;
    std::vector<int> servicesOrder;
};

std::wstring SettingsPath();
bool LoadSettings(Settings& out);       // false (defaults kept) if there is no file or it is unreadable
bool SaveSettings(const Settings& in);  // writes a temporary file and moves it over the old one

} // namespace lask
