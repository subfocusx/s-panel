// dllmain.cpp — точки входа taskbar.dll (живёт внутри explorer.exe).
//
// Что делает DLL:
//   1) читает конфиг (config.ini рядом с DLL, иначе %LOCALAPPDATA%\s-panel\config.ini);
//   2) ставит хуки на размеры панели (см. hooks.cpp);
//   3) следит за config.ini и применяет изменения на лету.
// Трей-иконка с настройками живёт в loader.exe (--tray), связь — через config.ini.
// Откат: экспорт SpanelUninstall (вызывает loader.exe --uninstall) снимает хуки,
// перезапуск explorer не нужен.
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <mutex>
#include "../shared/config.h"
#include "offsets.h"
#include "MinHook.h"

bool InstallHooks(HMODULE taskbarMod, const PanelConfig& cfg);
void RemoveHooks();
void ApplyPanelConfig(const PanelConfig& cfg);

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
    LogLine(L"WatchThread start");
    std::wstring path = CurrentConfigPath();
    PanelConfig last{};
    ReadPanelConfig(path.c_str(), &last);

    for (;;) {
        // Ждём событие остановки slices по 250 мс — выходим быстро.
        if (WaitForSingleObject(g_watchStop, 250) == WAIT_OBJECT_0) break;
        // Перечитываем безусловно и сравниваем ЗНАЧЕНИЯ: опрос mtime ломался
        // на рваной записи (метка обновлялась раньше удачного чтения
        // и вотчер залипал на дефолтах). Чтение ini раз в 250 мс — дёшево.
        std::wstring cur = CurrentConfigPath();
        if (cur != path) {
            path = cur;
            ReadPanelConfig(path.c_str(), &last);
            continue;
        }
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
        }
    }
    LogLine(L"WatchThread exit");
    return 0;
}

static void StartWatcher() {
    g_watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_watchStop) {
        LogLine(L"StartWatcher: CreateEvent FAILED");
        return;
    }
    g_watchThread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    LogLine(L"StartWatcher: thread=%p", (void*)g_watchThread);
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

    // Трей-иконка живёт в loader.exe (--tray), а не в explorer: окна и GDI
    // в процессе оболочки периодически роняли её (fastfail в C++ unwind).
    // Связь — через config.ini: вотчер ниже применяет правки попапа.
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

// Пустышка для V4-пробы (никем не вызывается).
extern "C" void SpanelDummyTarget() {}

// Временный диагностический экспорт: варианты маркировки CFG + статус CFG.
// Удалить после выяснения причины GLE=87.
extern "C" __declspec(dllexport) void __cdecl SpanelCfgProbe(void) {
    LogLine(L"SpanelCfgProbe start");

    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
    if (GetProcessMitigationPolicy(GetCurrentProcess(),
                                    ProcessControlFlowGuardPolicy, &cfg, sizeof(cfg))) {
        LogLine(L"CFG policy: Enable=%d Strict=%d Xfg=%d XfgAudit=%d",
                (int)cfg.EnableControlFlowGuard, (int)cfg.StrictMode, (int)cfg.EnableXfg,
                (int)cfg.EnableXfgAuditMode);
    } else {
        LogLine(L"GetProcessMitigationPolicy FAILED GLE=%lu", GetLastError());
    }

    typedef BOOL(WINAPI * Fn_t)(HANDLE, PVOID, SIZE_T, ULONG, PCFG_CALL_TARGET_INFO);
    Fn_t fn = (Fn_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),
                                   "SetProcessValidCallTargets");
    if (!fn) {
        LogLine(L"SpanelCfgProbe: no API");
        return;
    }

    void* tramp = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    LogLine(L"SpanelCfgProbe: test page=%p", tramp);
    if (!tramp) {
        LogLine(L"SpanelCfgProbe: VirtualAlloc FAILED GLE=%lu", GetLastError());
        return;
    }
    uintptr_t page = (uintptr_t)tramp & ~(uintptr_t)0xFFF;
    LogLine(L"tramp=%p page=%p", tramp, (void*)page);

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(tramp, &mbi, sizeof(mbi)))
        LogLine(L"tramp state=0x%lx protect=0x%lx type=0x%lx", mbi.State, mbi.Protect, mbi.Type);

    // V1: одна страница, все 256 слотов.
    CFG_CALL_TARGET_INFO full[256];
    for (int i = 0; i < 256; i++) {
        full[i].Offset = (ULONG_PTR)(i * 16);
        full[i].Flags = CFG_CALL_TARGET_VALID;
    }
    SetLastError(0);
    BOOL r = fn(GetCurrentProcess(), (PVOID)page, 4096, 256, full);
    LogLine(L"V1 full-page: r=%d GLE=%lu", (int)r, GetLastError());

    // V2: 16 байт вокруг трамплина, 1 слот.
    uintptr_t slot = (uintptr_t)tramp & ~(uintptr_t)0xF;
    CFG_CALL_TARGET_INFO one[1] = {{(ULONG_PTR)(slot - page), CFG_CALL_TARGET_VALID}};
    SetLastError(0);
    r = fn(GetCurrentProcess(), (PVOID)page, 4096, 1, one);
    LogLine(L"V2 one-slot: r=%d GLE=%lu", (int)r, GetLastError());

    // V3: VirtualProtect RWX + повтор V1.
    DWORD old = 0;
    if (VirtualProtect((void*)page, 4096, PAGE_EXECUTE_READWRITE, &old)) {
        SetLastError(0);
        r = fn(GetCurrentProcess(), (PVOID)page, 4096, 256, full);
        LogLine(L"V3 after RWX: r=%d GLE=%lu", (int)r, GetLastError());
    }
    // V4: настоящий трамплин MinHook — создаём хук на пустышку, маркируем, снимаем.
    // (Патч висит миллисекунды, пустышку никто не вызывает.)
    extern void SpanelDummyTarget();
    void* dummyOrig = nullptr;
    if (MH_CreateHook((void*)SpanelDummyTarget, (void*)SpanelDummyTarget,
                      &dummyOrig) == MH_OK &&
        dummyOrig) {
        uintptr_t dpage = (uintptr_t)dummyOrig & ~(uintptr_t)0xFFF;
        MEMORY_BASIC_INFORMATION dmbi{};
        if (VirtualQuery(dummyOrig, &dmbi, sizeof(dmbi)))
            LogLine(L"tramp state=0x%lx protect=0x%lx type=0x%lx", dmbi.State, dmbi.Protect,
                    dmbi.Type);
        CFG_CALL_TARGET_INFO dfull[256];
        for (int i = 0; i < 256; i++) {
            dfull[i].Offset = (ULONG_PTR)(i * 16);
            dfull[i].Flags = CFG_CALL_TARGET_VALID;
        }
        SetLastError(0);
        r = fn(GetCurrentProcess(), (PVOID)dpage, 4096, 256, dfull);
        LogLine(L"V4 real trampoline: r=%d GLE=%lu", (int)r, GetLastError());
        MH_RemoveHook((void*)SpanelDummyTarget);
    } else {
        LogLine(L"V4: MH_CreateHook failed");
    }
    LogLine(L"SpanelCfgProbe done");
}

// Экспорт для отката: loader.exe --uninstall вызывает его удалённо.
extern "C" __declspec(dllexport) void __cdecl SpanelUninstall(void) {
    LogLine(L"SpanelUninstall called");
    StopWatcher();
    RemoveHooks();
    LogLine(L"SpanelUninstall done");
}
