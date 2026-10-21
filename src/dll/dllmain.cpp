// dllmain.cpp — точки входа taskbar.dll (живёт внутри explorer.exe).
//
// Что делает DLL:
//   1) читает конфиг (config.ini рядом с DLL, иначе %LOCALAPPDATA%\s-panel\config.ini);
//   2) ставит хуки на размеры панели (см. hooks.cpp);
//   3) поднимает иконку в трее с мини-панелью настроек (trayui.cpp);
//   4) следит за config.ini и применяет изменения на лету.
// Откат: экспорт SpanelUninstall (вызывает loader.exe --uninstall) снимает хуки,
// гасит иконку и останавливает потоки — перезапуск explorer не нужен.
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <mutex>
#include "../shared/config.h"
#include "offsets.h"
#include "trayui.h"

bool InstallHooks(HMODULE taskbarMod, const PanelConfig& cfg);
void RemoveHooks();
void ApplyPanelConfig(const PanelConfig& cfg);
void TrayUiNoteExternalChange(const PanelConfig& cfg);

static HMODULE g_hMod = nullptr;
static std::wstring g_configPath;
static std::mutex g_pathLock;

static std::wstring DllDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_hMod, path, MAX_PATH);
    std::wstring s = path;
    auto pos = s.find_last_of(L"\\/");
    return (pos == std::string::npos) ? std::wstring() : s.substr(0, pos + 1);
}

// Путь конфига: рядом с DLL, если там есть файл; иначе пользовательский.
static std::wstring ResolveConfigPath() {
    std::wstring local = DllDir() + L"config.ini";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    std::wstring user = UserConfigPath();
    if (!user.empty() && GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) return user;
    return local;
}

static std::wstring CurrentConfigPath() {
    std::lock_guard<std::mutex> lock(g_pathLock);
    return g_configPath;
}

// ---------------------------------------------------------------- слежение за файлом
// Опрос mtime короткими slices (50 мс): никаких неотменяемых блокирующих вызовов.
// Это важно для безопасной выгрузки: поток гарантированно завершается за <100 мс
// после установки g_watchStop, и FreeLibrary не застанет наш код на стеке.
static HANDLE g_watchStop = nullptr;
static HANDLE g_watchThread = nullptr;

static bool FileMTime(const std::wstring& path, FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) return false;
    *out = attr.ftLastWriteTime;
    return true;
}

static DWORD WINAPI WatchThread(LPVOID) {
    std::wstring path = CurrentConfigPath();
    PanelConfig last{};
    ReadPanelConfig(path.c_str(), &last);
    FILETIME lastWrite{};
    FileMTime(path, &lastWrite);

    for (;;) {
        // Ждём событие остановки slices по 50 мс — выходим быстро.
        if (WaitForSingleObject(g_watchStop, 50) == WAIT_OBJECT_0) break;
        std::wstring cur = CurrentConfigPath();
        if (cur != path) {
            path = cur;
            ReadPanelConfig(path.c_str(), &last);
            FileMTime(path, &lastWrite);
            continue;
        }
        FILETIME ft{};
        if (!FileMTime(path, &ft)) continue;
        if (CompareFileTime(&ft, &lastWrite) == 0) continue;
        lastWrite = ft;

        PanelConfig cfg{};
        ReadPanelConfig(path.c_str(), &cfg);
        bool changed = cfg.taskbarHeight != last.taskbarHeight ||
                       cfg.iconSize != last.iconSize ||
                       cfg.taskbarButtonWidth != last.taskbarButtonWidth ||
                       cfg.trayIconSize != last.trayIconSize ||
                       cfg.taskbarIconSpacing != last.taskbarIconSpacing ||
                       cfg.trayIconSpacing != last.trayIconSpacing;
        if (changed) {
            LogLine(L"config changed on disk: h=%d icon=%d btn=%d tray=%d tsp=%d trsp=%d",
                    cfg.taskbarHeight, cfg.iconSize, cfg.taskbarButtonWidth, cfg.trayIconSize,
                    cfg.taskbarIconSpacing, cfg.trayIconSpacing);
            last = cfg;
            ApplyPanelConfig(cfg);
            TrayUiNoteExternalChange(cfg);
        }
    }
    LogLine(L"WatchThread exit");
    return 0;
}

static void StartWatcher() {
    g_watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_watchStop) return;
    g_watchThread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (g_watchThread) {
        // Понижаем приоритет: не мешаем оболочке.
        SetThreadPriority(g_watchThread, THREAD_PRIORITY_IDLE);
    }
}

static void StopWatcher() {
    if (g_watchThread) {
        if (g_watchStop) SetEvent(g_watchStop);
        DWORD w = WaitForSingleObject(g_watchThread, 3000);
        if (w != WAIT_OBJECT_0) {
            LogLine(L"StopWatcher: thread did not exit in 3s (leaking, skip FreeLibrary risk)");
        }
        CloseHandle(g_watchThread);
        g_watchThread = nullptr;
    }
    if (g_watchStop) {
        CloseHandle(g_watchStop);
        g_watchStop = nullptr;
    }
}

// ---------------------------------------------------------------- инициализация
static DWORD WINAPI InitThread(LPVOID) {
    wchar_t dllPath[MAX_PATH]{};
    GetModuleFileNameW(g_hMod, dllPath, MAX_PATH);
    LogLine(L"InitThread start, dll=%s", dllPath);

    {
        std::lock_guard<std::mutex> lock(g_pathLock);
        g_configPath = ResolveConfigPath();
    }
    std::wstring ini = CurrentConfigPath();
    LogLine(L"config ini=%s", ini.c_str());

    PanelConfig cfg;
    ReadPanelConfig(ini.c_str(), &cfg);
    LogLine(L"cfg height=%d icon=%d btn=%d tray=%d", cfg.taskbarHeight, cfg.iconSize,
            cfg.taskbarButtonWidth, cfg.trayIconSize);

    HMODULE taskbar = GetModuleHandleW(L"Taskbar.dll");
    LogLine(L"GetModuleHandle(Taskbar.dll)=%p", (void*)taskbar);
    bool ok = taskbar && InstallHooks(taskbar, cfg);
    LogLine(L"%s", ok ? L"hooks installed" : L"resolver refused, no-op");

    // Трей-иконка с панелью настроек.
    TrayUiStart(ini.c_str(), &ApplyPanelConfig);

    // Слежение за конфигом (применение правок без перезапуска explorer).
    StartWatcher();
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

// Экспорт для отката: loader.exe --uninstall вызывает его удалённо.
extern "C" __declspec(dllexport) void __cdecl SpanelUninstall(void) {
    LogLine(L"SpanelUninstall called");
    TrayUiStop();
    StopWatcher();
    RemoveHooks();
    LogLine(L"SpanelUninstall done");
}
