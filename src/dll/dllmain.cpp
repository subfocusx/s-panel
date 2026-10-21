#include <windows.h>
#include <shellapi.h>
#include <string>
#include "../shared/config.h"

bool InstallHooks(HMODULE taskbarMod, const PanelConfig& cfg);
void RemoveHooks();

static HMODULE g_hMod = nullptr;

static void RefreshTaskbar() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
}

static DWORD WINAPI InitThread(LPVOID) {
    wchar_t dllPath[MAX_PATH]{};
    GetModuleFileNameW(g_hMod, dllPath, MAX_PATH);
    std::wstring s = dllPath;
    auto pos = s.find_last_of(L"\\/");
    std::wstring ini =
        (pos == std::wstring::npos) ? L"config.ini" : s.substr(0, pos + 1) + L"config.ini";
    PanelConfig cfg;
    ReadPanelConfig(ini.c_str(), &cfg);

    HMODULE taskbar = GetModuleHandleW(L"Taskbar.dll");
    bool ok = taskbar && InstallHooks(taskbar, cfg);
    wchar_t msg[128]{};
    wsprintfW(msg, L"[s-panel] %s\n", ok ? L"GetFrameSize hook ON" : L"resolver refused, no-op");
    OutputDebugStringW(msg);
    if (ok) RefreshTaskbar();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hMod = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
