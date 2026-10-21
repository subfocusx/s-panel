// hooks.cpp — Phase 1: GetFrameSize only (minimal blast radius).
// Signature (from Windhawk mod 1.3.10): static double GetFrameSize(int).
#include "resolve.h"
#include "offsets.h"
#include "../shared/config.h"
#include "MinHook.h"
#include <shellapi.h>
#include <cstring>

// FNV-1a over Taskbar.dll .text for the single allowlisted build 10.0.26100.
// 0 = not pinned -> hooks refused. MUST be set to the value confirmed on the
// reference Taskbar.dll before shipping; it is intentionally left unset here.
static constexpr uint64_t kExpectedTextHash = 0xc7eded830e6b3b32ULL;

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

bool InstallHooks(HMODULE taskbarMod, const PanelConfig& cfg) {
    if (kExpectedTextHash == 0) {
        LogLine(L"InstallHooks refused: kExpectedTextHash unset");
        return false;
    }
    g_cfg = cfg;
    auto base = (const uint8_t*)taskbarMod;
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    size_t imgSize = nt->OptionalHeader.SizeOfImage;
    LogLine(L"Taskbar.dll imageSize=0x%zx", imgSize);
    ResolvedAddrs r{};
    if (!ResolveTaskbarAddrs(base, imgSize, true, &r)) {
        LogLine(L"ResolveTaskbarAddrs FAILED");
        return false;
    }
    LogLine(L"ResolveTaskbarAddrs ok: frame=0x%lx icon=0x%lx metrics=0x%lx hash=0x%016llx",
            r.getFrameSizeRva, r.getIconHeightRva, r.getMetricsRva,
            (unsigned long long)r.imageHash);
    if (r.imageHash != kExpectedTextHash) {
        LogLine(L"hash mismatch: expected=0x%016llx got=0x%016llx",
                (unsigned long long)kExpectedTextHash, (unsigned long long)r.imageHash);
        return false;
    }
    LogLine(L"hash matches kExpectedTextHash");
    bool inText = IsRvaInText(nt, r.getFrameSizeRva);
    LogLine(L"IsRvaInText(frame=0x%lx)=%d", r.getFrameSizeRva, inText ? 1 : 0);
    if (!inText) return false;
    if (MH_Initialize() != MH_OK) {
        LogLine(L"MH_Initialize FAILED");
        return false;
    }
    LogLine(L"MH_Initialize ok");
    g_frameRva = r.getFrameSizeRva;
    void* target = (void*)(base + r.getFrameSizeRva);
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
    return true;
}

void RemoveHooks() {
    if (g_frameRva == 0) return;
    HMODULE tb = GetModuleHandleW(L"Taskbar.dll");
    if (tb) {
        void* target = (void*)((const uint8_t*)tb + g_frameRva);
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    MH_Uninitialize();
    g_frameRva = 0;
    LogLine(L"RemoveHooks done");
}
