// hooks.cpp — Phase 1: GetFrameSize only (minimal blast radius).
// Signature (from Windhawk mod 1.3.10): static double GetFrameSize(int).
#include "offsets.h"
#include "../shared/config.h"
#include "MinHook.h"
#include <shellapi.h>
#include <winuser.h>
#include <cstring>
#include <cstdint>

static constexpr const char* kViewGuid = "0AE0E302E8E34BE29CC303A7A62ABCA7";
static constexpr DWORD kViewAge = 1;
static constexpr DWORD kGetFrameSizeRva = 0x7F4B0;

static PanelConfig g_cfg;
static DWORD g_frameRva = 0;
static volatile LONG g_vertCache = 0;
static volatile ULONGLONG g_vertCheckedAt = 0;
static constexpr ULONGLONG kVertRefreshMs = 1000;

using GetFrameSize_t = double(WINAPI*)(int);
static GetFrameSize_t GetFrameSize_Orig = nullptr;

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

static volatile LONG g_hookCalls = 0;

static double WINAPI GetFrameSize_Hook(int e) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    bool vertical = IsVerticalTaskbarCached();
    double ret;
    if (!vertical && (e == 1 || e == 2))
        ret = (double)g_cfg.taskbarHeight;
    else
        ret = GetFrameSize_Orig(e);
    if (n == 1 || n % 500 == 0)
        LogLine(L"GetFrameSize_Hook call=%ld e=%d vertical=%d ret=%f", n, e, vertical ? 1 : 0, ret);
    return ret;
}

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

static LONG g_lastRefreshedHeight = -1;

static HWND FindMSTaskSwWClass(HWND hTaskbarWnd) {
    HWND hRebar = FindWindowExW(hTaskbarWnd, nullptr, L"ReBarWindow32", nullptr);
    if (!hRebar) return nullptr;
    return FindWindowExW(hRebar, nullptr, L"MSTaskSwWClass", nullptr);
}

static void RefreshTaskbarLayout() {
    if (IsVerticalTaskbarCached()) {
        LogLine(L"RefreshTaskbarLayout skipped: vertical taskbar");
        return;
    }
    LONG target = (LONG)g_cfg.taskbarHeight;
    if (InterlockedExchange(&g_lastRefreshedHeight, target) == target) {
        LogLine(L"RefreshTaskbarLayout skipped: height %ld already applied", (long)target);
        return;
    }
    HWND hTaskbarWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTaskbarWnd) {
        LogLine(L"RefreshTaskbarLayout: Shell_TrayWnd not found");
        return;
    }
    LogLine(L"RefreshTaskbarLayout start hwnd=%p height=%ld", (void*)hTaskbarWnd, (long)target);
    SendMessageW(hTaskbarWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);
    LogLine(L"RefreshTaskbarLayout sent WM_SETTINGCHANGE #1");
    g_cfg.taskbarHeight = (int)(target - 1);
    SendMessageW(hTaskbarWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);
    LogLine(L"RefreshTaskbarLayout sent WM_SETTINGCHANGE #2 (height=%ld)", (long)(target - 1));
    g_cfg.taskbarHeight = (int)target;
    SendMessageW(hTaskbarWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);
    LogLine(L"RefreshTaskbarLayout sent WM_SETTINGCHANGE #3 (height=%ld)", (long)target);
    HWND hMSTaskSwWClass = FindMSTaskSwWClass(hTaskbarWnd);
    if (hMSTaskSwWClass) {
        SendMessageW(hMSTaskSwWClass, 0x452, 3, 0);
        LogLine(L"RefreshTaskbarLayout sent 0x452 to MSTaskSwWClass=%p", (void*)hMSTaskSwWClass);
    } else {
        LogLine(L"RefreshTaskbarLayout: MSTaskSwWClass not found");
    }
}

bool InstallHooks(HMODULE /*unused*/, const PanelConfig& cfg) {
    g_cfg = cfg;
    HMODULE viewMod = GetModuleHandleW(L"Taskbar.View.dll");
    if (!viewMod) {
        LogLine(L"InstallHooks: Taskbar.View.dll not loaded");
        return false;
    }
    auto base = (const uint8_t*)viewMod;
    if (!VerifyViewGuid(base)) {
        LogLine(L"InstallHooks refused: Taskbar.View.dll PDB GUID/age mismatch");
        return false;
    }
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    LogLine(L"Taskbar.View.dll imageSize=0x%lx", (unsigned long)nt->OptionalHeader.SizeOfImage);
    bool inText = IsRvaInText(nt, kGetFrameSizeRva);
    LogLine(L"IsRvaInText(frame=0x%lx)=%d", (unsigned long)kGetFrameSizeRva, inText ? 1 : 0);
    if (!inText) return false;
    if (MH_Initialize() != MH_OK) {
        LogLine(L"MH_Initialize FAILED");
        return false;
    }
    LogLine(L"MH_Initialize ok");
    g_frameRva = kGetFrameSizeRva;
    void* target = (void*)(base + kGetFrameSizeRva);
    if (MH_CreateHook(target, GetFrameSize_Hook,
                      (void**)&GetFrameSize_Orig) != MH_OK) {
        LogLine(L"MH_CreateHook FAILED target=%p", target);
        MH_Uninitialize();
        return false;
    }
    LogLine(L"MH_CreateHook ok target=%p", target);
    if (MH_EnableHook(target) != MH_OK) {
        LogLine(L"MH_EnableHook FAILED");
        MH_RemoveHook(target);
        MH_Uninitialize();
        return false;
    }
    LogLine(L"MH_EnableHook ok");
    RefreshTaskbarLayout();
    return true;
}

void RemoveHooks() {
    if (g_frameRva == 0) return;
    HMODULE tb = GetModuleHandleW(L"Taskbar.View.dll");
    if (tb) {
        void* target = (void*)((const uint8_t*)tb + g_frameRva);
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    MH_Uninitialize();
    g_frameRva = 0;
    LogLine(L"RemoveHooks done");
}
