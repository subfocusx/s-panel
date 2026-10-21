#include <windows.h>
#include <shellapi.h>
#include <string>
#include "../shared/config.h"
#include "offsets.h"

bool InstallHooks(HMODULE taskbarMod, const PanelConfig& cfg);
void RemoveHooks();

static HMODULE g_hMod = nullptr;

static DWORD WINAPI InitThread(LPVOID) {
    wchar_t dllPath[MAX_PATH]{};
    GetModuleFileNameW(g_hMod, dllPath, MAX_PATH);
    std::wstring s = dllPath;
    auto pos = s.find_last_of(L"\\/");
    std::wstring ini =
        (pos == std::wstring::npos) ? L"config.ini" : s.substr(0, pos + 1) + L"config.ini";
    LogLine(L"InitThread start, dll=%s", dllPath);
    LogLine(L"config ini=%s", ini.c_str());
    PanelConfig cfg;
    ReadPanelConfig(ini.c_str(), &cfg);
    LogLine(L"cfg taskbarHeight=%d iconSize=%d buttonWidth=%d trayIconSize=%d", cfg.taskbarHeight,
            cfg.iconSize, cfg.taskbarButtonWidth, cfg.trayIconSize);

    HMODULE taskbar = GetModuleHandleW(L"Taskbar.dll");
    LogLine(L"GetModuleHandle(Taskbar.dll)=%p", (void*)taskbar);
    if (taskbar) {
        LogLine(L"Taskbar.dll base=%p", (void*)taskbar);
    }
    bool ok = taskbar && InstallHooks(taskbar, cfg);
    LogLine(L"%s", ok ? L"GetFrameSize hook ON" : L"resolver refused, no-op");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hMod = hModule;
        DisableThreadLibraryCalls(hModule);
        LogLine(L"DLL_PROCESS_ATTACH module=%p", (void*)hModule);
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        LogLine(L"DLL_PROCESS_DETACH");
    }
    return TRUE;
}
