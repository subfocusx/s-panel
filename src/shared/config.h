#pragma once
#include <string>

struct PanelConfig {
    int taskbarHeight = 34;      // default Win11: 48
    int iconSize = 16;           // default Win11: 24
    int taskbarButtonWidth = 28; // default Win11: 44
    int trayIconSize = 16;       // default Win11: 24 (tray, separate)
};

// iniPath may be nullptr -> "<dll-or-exe-dir>\\config.ini"
bool ReadPanelConfig(const wchar_t* iniPath, PanelConfig* out);
std::wstring DefaultIniPath(void* hModuleOrNull);
