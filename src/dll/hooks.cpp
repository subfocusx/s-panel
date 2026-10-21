// hooks.cpp — Phase 1: GetFrameSize only (minimal blast radius).
// Signature (from Windhawk mod 1.3.10): static double GetFrameSize(int).
#include "resolve.h"
#include "../shared/config.h"
#include "MinHook.h"
#include <shellapi.h>
#include <cstring>

// FNV-1a over Taskbar.dll .text for the single allowlisted build 10.0.26100.
// 0 = not pinned -> hooks refused. MUST be set to the value confirmed on the
// reference Taskbar.dll before shipping; it is intentionally left unset here.
static constexpr uint64_t kExpectedTextHash = 0;

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

static double WINAPI GetFrameSize_Hook(int e) {
    if (!IsVerticalTaskbarCached() && (e == 1 || e == 2))
        return (double)g_cfg.taskbarHeight;
    return GetFrameSize_Orig(e);
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
    if (kExpectedTextHash == 0) return false;
    g_cfg = cfg;
    auto base = (const uint8_t*)taskbarMod;
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    size_t imgSize = nt->OptionalHeader.SizeOfImage;
    ResolvedAddrs r{};
    if (!ResolveTaskbarAddrs(base, imgSize, true, &r)) return false;
    if (r.imageHash != kExpectedTextHash) return false;
    if (!IsRvaInText(nt, r.getFrameSizeRva)) return false;
    if (MH_Initialize() != MH_OK) return false;
    g_frameRva = r.getFrameSizeRva;
    void* target = (void*)(base + r.getFrameSizeRva);
    if (MH_CreateHook(target, GetFrameSize_Hook,
                      (void**)&GetFrameSize_Orig) != MH_OK) {
        MH_Uninitialize();
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        MH_Uninitialize();
        return false;
    }
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
}
