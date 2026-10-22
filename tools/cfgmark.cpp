// Probe SetProcessValidCallTargets semantics in this process (safe, own memory).
#include <windows.h>
#include <cstdio>
#include <cstdint>

typedef BOOL(WINAPI* Fn_t)(HANDLE, PVOID, SIZE_T, ULONG, PCFG_CALL_TARGET_INFO);

static void Try(Fn_t fn, const char* tag, void* base, SIZE_T region, ULONG n,
                PCFG_CALL_TARGET_INFO info) {
    SetLastError(0);
    BOOL r = fn(GetCurrentProcess(), base, region, n, info);
    printf("%-28s -> %d GLE=%lu\n", tag, (int)r, GetLastError());
}

int wmain() {
    Fn_t fn = (Fn_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),
                                   "SetProcessValidCallTargets");
    if (!fn) fn = (Fn_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                       "SetProcessValidCallTargets");
    printf("api=%p\n", (void*)fn);
    if (!fn) return 2;

    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    printf("page=%p\n", page);

    CFG_CALL_TARGET_INFO one[1] = {{0, CFG_CALL_TARGET_VALID}};
    Try(fn, "single offset0, region=page", page, 4096, 1, one);

    CFG_CALL_TARGET_INFO slots[256];
    for (int i = 0; i < 256; i++) {
        slots[i].Offset = (ULONG_PTR)(i * 16);
        slots[i].Flags = CFG_CALL_TARGET_VALID;
    }
    Try(fn, "256 slots, region=page", page, 4096, 256, slots);
    Try(fn, "256 slots, region=256", page, 256, 256, slots);
    Try(fn, "1 slot, region=16", page, 16, 1, one);

    // Unaligned target inside page, region = one page from aligned start.
    void* mid = (uint8_t*)page + 123;
    CFG_CALL_TARGET_INFO m[1] = {{(ULONG_PTR)((uint8_t*)mid - (uint8_t*)page),
                                   CFG_CALL_TARGET_VALID}};
    Try(fn, "mid-page offset, region=page", page, 4096, 1, m);

    // Two pages.
    void* two = VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CFG_CALL_TARGET_INFO s2[512];
    for (int i = 0; i < 512; i++) {
        s2[i].Offset = (ULONG_PTR)(i * 16);
        s2[i].Flags = CFG_CALL_TARGET_VALID;
    }
    Try(fn, "512 slots, region=2 pages", two, 8192, 512, s2);

    // Non-executable page.
    void* rw = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Try(fn, "RW page, 256 slots", rw, 4096, 256, slots);
    return 0;
}
