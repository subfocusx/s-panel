#include "config.h"
#include <windows.h>

std::wstring DefaultIniPath(void* hModuleOrNull) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW((HMODULE)hModuleOrNull, path, MAX_PATH);
    std::wstring s = path;
    auto pos = s.find_last_of(L"\\/");
    if (pos != std::wstring::npos) s.resize(pos + 1);
    s += L"config.ini";
    return s;
}

bool ReadPanelConfig(const wchar_t* iniPath, PanelConfig* out) {
    if (!out) return false;
    std::wstring p;
    if (iniPath && *iniPath) {
        p = iniPath;
    } else {
        p = DefaultIniPath(nullptr);
        // For DLL: nullptr gives host (explorer) path — caller passes DLL path explicitly.
    }
    PanelConfig cfg; // defaults from header
    cfg.taskbarHeight =
        GetPrivateProfileIntW(L"Panel", L"TaskbarHeight", cfg.taskbarHeight, p.c_str());
    cfg.iconSize =
        GetPrivateProfileIntW(L"Panel", L"IconSize", cfg.iconSize, p.c_str());
    cfg.taskbarButtonWidth = GetPrivateProfileIntW(
        L"Panel", L"TaskbarButtonWidth", cfg.taskbarButtonWidth, p.c_str());
    cfg.trayIconSize =
        GetPrivateProfileIntW(L"Panel", L"TrayIconSize", cfg.trayIconSize, p.c_str());
    // Clamp to sane ranges so a typo can't collapse the taskbar.
    if (cfg.taskbarHeight < 24) cfg.taskbarHeight = 24;
    if (cfg.taskbarHeight > 72) cfg.taskbarHeight = 72;
    if (cfg.iconSize < 12) cfg.iconSize = 12;
    if (cfg.iconSize > 48) cfg.iconSize = 48;
    if (cfg.taskbarButtonWidth < 20) cfg.taskbarButtonWidth = 20;
    if (cfg.taskbarButtonWidth > 120) cfg.taskbarButtonWidth = 120;
    if (cfg.trayIconSize < 12) cfg.trayIconSize = 12;
    if (cfg.trayIconSize > 48) cfg.trayIconSize = 48;
    *out = cfg;
    return true;
}
