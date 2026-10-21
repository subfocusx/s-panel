// hooks.cpp — хуки Taskbar.View.dll (Windows 11 build 26100).
//
// Точки хука берутся из PDB Taskbar.View.pdb (GUID 0AE0E302E8E34BE29CC303A7A62ABCA7, age 1)
// и проверяются на попадание в .text перед установкой. Если символ не найден или RVA
// не попадает в .text — хук не ставится, остальные продолжают работать (частичный режим).
#include "offsets.h"
#include "../shared/config.h"
#include "MinHook.h"
#include <shellapi.h>
#include <winuser.h>
#include <atomic>
#include <cstring>
#include <cstdint>

static constexpr const char* kViewGuid = "0AE0E302E8E34BE29CC303A7A62ABCA7";
static constexpr DWORD kViewAge = 1;

// RVA из PDB Taskbar.View.pdb.
static constexpr DWORD kRvaGetFrameSize = 0x7F4B0;               // TaskbarConfiguration::GetFrameSize (static double(TaskbarSize))
static constexpr DWORD kRvaGetIconHeightInViewPixels = 0x1CA628; // TaskbarConfiguration::GetIconHeightInViewPixels (static double(TaskbarSize))
static constexpr DWORD kRvaDefaultButtonWidth = 0x8BF640;        // TaskbarComponentHost::DefaultButtonWidth (double(this))
static constexpr DWORD kRvaTrayIconSize = 0x1E99FC;              // IconViewModel::OnSystemTrayFrameSizeChanged (double(this))

// Живые значения: читаются хуками из потока UI explorer, пишутся из потока трея.
struct LiveValues {
    std::atomic<int> height{34};
    std::atomic<int> iconSize{16};
    std::atomic<int> buttonWidth{28};
    std::atomic<int> trayIconSize{16};
};
static LiveValues g_live;

// ---------------------------------------------------------------- вертикальная панель
static volatile LONG g_vertCache = 0;
static volatile ULONGLONG g_vertCheckedAt = 0;
static constexpr ULONGLONG kVertRefreshMs = 1000;

static bool QueryVerticalTaskbar() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) return false;
    return abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT;
}

static bool IsVerticalTaskbarCached() {
    ULONGLONG now = GetTickCount64();
    if (now - g_vertCheckedAt >= kVertRefreshMs) {
        InterlockedExchange(&g_vertCache, QueryVerticalTaskbar() ? 1 : 0);
        g_vertCheckedAt = now;
    }
    return g_vertCache != 0;
}

// ---------------------------------------------------------------- хуки
static volatile LONG g_hookCalls = 0;

using GetFrameSize_t = double(WINAPI*)(int);
static GetFrameSize_t GetFrameSize_Orig = nullptr;

static double WINAPI GetFrameSize_Hook(int e) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    bool vertical = IsVerticalTaskbarCached();
    double ret;
    if (!vertical && (e == 1 || e == 2))
        ret = (double)g_live.height.load(std::memory_order_relaxed);
    else
        ret = GetFrameSize_Orig(e);
    if (n == 1 || n % 500 == 0)
        LogLine(L"GetFrameSize_Hook call=%ld e=%d vertical=%d ret=%f", n, e, vertical ? 1 : 0, ret);
    return ret;
}

using GetIconHeight_t = double(WINAPI*)(int);
static GetIconHeight_t GetIconHeight_Orig = nullptr;

static double WINAPI GetIconHeight_Hook(int e) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    bool vertical = IsVerticalTaskbarCached();
    double ret;
    if (!vertical && (e == 1 || e == 2))
        ret = (double)g_live.iconSize.load(std::memory_order_relaxed);
    else
        ret = GetIconHeight_Orig(e);
    if (n == 1 || n % 200 == 0)
        LogLine(L"GetIconHeight_Hook call=%ld e=%d vertical=%d ret=%f", n, e, vertical ? 1 : 0, ret);
    return ret;
}

using GetButtonWidth_t = double(*)(void* thisPtr);
static GetButtonWidth_t GetButtonWidth_Orig = nullptr;

static double GetButtonWidth_Hook(void* self) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    double ret = (double)g_live.buttonWidth.load(std::memory_order_relaxed);
    if (n == 1 || n % 200 == 0)
        LogLine(L"GetButtonWidth_Hook call=%ld ret=%f", n, ret);
    return ret;
}

using GetTrayIconSize_t = double(*)(void* thisPtr);
static GetTrayIconSize_t GetTrayIconSize_Orig = nullptr;

static double GetTrayIconSize_Hook(void* self) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    double ret = (double)g_live.trayIconSize.load(std::memory_order_relaxed);
    if (n == 1 || n % 200 == 0)
        LogLine(L"GetTrayIconSize_Hook call=%ld ret=%f", n, ret);
    return ret;
}

// ---------------------------------------------------------------- проверка образа
static bool IsRvaInText(const IMAGE_NT_HEADERS64* nt, DWORD rva) {
    auto secs = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(secs[i].Name, ".text", 5) != 0) continue;
        DWORD va = secs[i].VirtualAddress;
        return rva >= va && rva - va < secs[i].Misc.VirtualSize;
    }
    return false;
}

static bool VerifyViewGuid(const uint8_t* base) {
    auto dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LogLine(L"VerifyViewGuid: bad DOS signature");
        return false;
    }
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        LogLine(L"VerifyViewGuid: bad NT signature");
        return false;
    }
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY)) {
        LogLine(L"VerifyViewGuid: no debug directory");
        return false;
    }
    auto entries = (const IMAGE_DEBUG_DIRECTORY*)(base + dir.VirtualAddress);
    size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < count; i++) {
        const IMAGE_DEBUG_DIRECTORY& e = entries[i];
        if (e.Type != IMAGE_DEBUG_TYPE_CODEVIEW || e.SizeOfData < 24 || e.AddressOfRawData == 0)
            continue;
        const uint8_t* p = base + e.AddressOfRawData;
        if (memcmp(p, "RSDS", 4) != 0) continue;

        const uint8_t* g = p + 4;
        uint32_t d1;
        uint16_t d2, d3;
        memcpy(&d1, g, 4);
        memcpy(&d2, g + 4, 2);
        memcpy(&d3, g + 6, 2);
        uint32_t age;
        memcpy(&age, p + 20, 4);

        char guid[33];
        int n = snprintf(guid, sizeof(guid), "%08lX%04X%04X", (unsigned long)d1, d2, d3);
        for (int b = 0; b < 8; b++) n += snprintf(guid + n, sizeof(guid) - n, "%02X", g[8 + b]);

        if (strcmp(guid, kViewGuid) != 0 || age != kViewAge) {
            LogLine(L"VerifyViewGuid mismatch: guid=%hs age=%lu expected guid=%hs age=%lu",
                    guid, (unsigned long)age, kViewGuid, (unsigned long)kViewAge);
            return false;
        }
        LogLine(L"VerifyViewGuid ok: guid=%hs age=%lu", guid, (unsigned long)age);
        return true;
    }
    LogLine(L"VerifyViewGuid: no RSDS record");
    return false;
}

// ---------------------------------------------------------------- пересчёт layout
static HWND FindMSTaskSwWClass(HWND hTaskbarWnd) {
    HWND hRebar = FindWindowExW(hTaskbarWnd, nullptr, L"ReBarWindow32", nullptr);
    if (!hRebar) return nullptr;
    return FindWindowExW(hRebar, nullptr, L"MSTaskSwWClass", nullptr);
}

// Пересчитать панель: WM_SETTINGCHANGE заставляет XAML-остров перемерить панель,
// а значит перечитать захукаенные размеры. Два прохода (с промежуточным значением
// высоты) обходят кэш layout.
static void RefreshTaskbarLayout() {
    if (IsVerticalTaskbarCached()) {
        LogLine(L"RefreshTaskbarLayout skipped: vertical taskbar");
        return;
    }
    HWND hTaskbarWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTaskbarWnd) {
        LogLine(L"RefreshTaskbarLayout: Shell_TrayWnd not found");
        return;
    }
    int target = g_live.height.load(std::memory_order_relaxed);
    LogLine(L"RefreshTaskbarLayout start hwnd=%p height=%d icon=%d btn=%d tray=%d", (void*)hTaskbarWnd,
            target, g_live.iconSize.load(), g_live.buttonWidth.load(), g_live.trayIconSize.load());

    g_live.height.store(target - 1, std::memory_order_relaxed);
    SendMessageW(hTaskbarWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);
    g_live.height.store(target, std::memory_order_relaxed);
    SendMessageW(hTaskbarWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);

    HWND hMSTaskSwWClass = FindMSTaskSwWClass(hTaskbarWnd);
    if (hMSTaskSwWClass) {
        SendMessageW(hMSTaskSwWClass, 0x452, 3, 0);
        LogLine(L"RefreshTaskbarLayout sent 0x452 to MSTaskSwWClass=%p", (void*)hMSTaskSwWClass);
    } else {
        LogLine(L"RefreshTaskbarLayout: MSTaskSwWClass not found");
    }
}

// Применить новый конфиг: значения + пересчёт панели.
void ApplyPanelConfig(const PanelConfig& cfg) {
    g_live.height.store(cfg.taskbarHeight, std::memory_order_relaxed);
    g_live.iconSize.store(cfg.iconSize, std::memory_order_relaxed);
    g_live.buttonWidth.store(cfg.taskbarButtonWidth, std::memory_order_relaxed);
    g_live.trayIconSize.store(cfg.trayIconSize, std::memory_order_relaxed);
    RefreshTaskbarLayout();
}

// ---------------------------------------------------------------- установка хуков
static const uint8_t* g_viewBase = nullptr;

static bool InstallOne(const char* name, DWORD rva, void* detour, void** orig) {
    void* target = (void*)(g_viewBase + rva);
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    if (st != MH_OK) {
        LogLine(L"MH_CreateHook FAILED %hs rva=0x%lx st=%d", name, (unsigned long)rva, (int)st);
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        LogLine(L"MH_EnableHook FAILED %hs rva=0x%lx st=%d", name, (unsigned long)rva, (int)st);
        MH_RemoveHook(target);
        return false;
    }
    LogLine(L"hook ON %hs rva=0x%lx target=%p", name, (unsigned long)rva, target);
    return true;
}

bool InstallHooks(HMODULE /*unused*/, const PanelConfig& cfg) {
    HMODULE viewMod = GetModuleHandleW(L"Taskbar.View.dll");
    if (!viewMod) {
        LogLine(L"InstallHooks: Taskbar.View.dll not loaded");
        return false;
    }
    g_viewBase = (const uint8_t*)viewMod;
    if (!VerifyViewGuid(g_viewBase)) {
        LogLine(L"InstallHooks refused: Taskbar.View.dll PDB GUID/age mismatch");
        return false;
    }
    auto dos = (const IMAGE_DOS_HEADER*)g_viewBase;
    auto nt = (const IMAGE_NT_HEADERS64*)(g_viewBase + dos->e_lfanew);
    LogLine(L"Taskbar.View.dll imageSize=0x%lx", (unsigned long)nt->OptionalHeader.SizeOfImage);

    if (MH_Initialize() != MH_OK) {
        LogLine(L"MH_Initialize FAILED");
        return false;
    }
    LogLine(L"MH_Initialize ok");

    bool ok = false;
    if (IsRvaInText(nt, kRvaGetFrameSize)) {
        ok |= InstallOne("GetFrameSize", kRvaGetFrameSize, (void*)GetFrameSize_Hook,
                         (void**)&GetFrameSize_Orig);
    } else {
        LogLine(L"skip GetFrameSize: rva not in .text");
    }
    if (IsRvaInText(nt, kRvaGetIconHeightInViewPixels)) {
        ok |= InstallOne("GetIconHeightInViewPixels", kRvaGetIconHeightInViewPixels,
                         (void*)GetIconHeight_Hook, (void**)&GetIconHeight_Orig);
    } else {
        LogLine(L"skip GetIconHeightInViewPixels: rva not in .text");
    }
    if (IsRvaInText(nt, kRvaDefaultButtonWidth)) {
        ok |= InstallOne("DefaultButtonWidth", kRvaDefaultButtonWidth, (void*)GetButtonWidth_Hook,
                         (void**)&GetButtonWidth_Orig);
    } else {
        LogLine(L"skip DefaultButtonWidth: rva not in .text");
    }
    if (IsRvaInText(nt, kRvaTrayIconSize)) {
        ok |= InstallOne("TrayIconSize", kRvaTrayIconSize, (void*)GetTrayIconSize_Hook,
                         (void**)&GetTrayIconSize_Orig);
    } else {
        LogLine(L"skip TrayIconSize: rva not in .text");
    }

    // Значения до старта панели.
    g_live.height.store(cfg.taskbarHeight, std::memory_order_relaxed);
    g_live.iconSize.store(cfg.iconSize, std::memory_order_relaxed);
    g_live.buttonWidth.store(cfg.taskbarButtonWidth, std::memory_order_relaxed);
    g_live.trayIconSize.store(cfg.trayIconSize, std::memory_order_relaxed);

    RefreshTaskbarLayout();
    return ok;
}

void RemoveHooks() {
    if (!g_viewBase) return;
    struct Item {
        DWORD rva;
        const char* name;
    };
    const Item items[] = {
        {kRvaGetFrameSize, "GetFrameSize"},
        {kRvaGetIconHeightInViewPixels, "GetIconHeightInViewPixels"},
        {kRvaDefaultButtonWidth, "DefaultButtonWidth"},
        {kRvaTrayIconSize, "TrayIconSize"},
    };
    for (const Item& it : items) {
        void* target = (void*)(g_viewBase + it.rva);
        if (MH_DisableHook(target) == MH_ERROR_NOT_INITIALIZED) continue;
        MH_RemoveHook(target);
        LogLine(L"hook OFF %hs rva=0x%lx", it.name, (unsigned long)it.rva);
    }
    MH_Uninitialize();
    g_viewBase = nullptr;
    LogLine(L"RemoveHooks done");
}
