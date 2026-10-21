// test_resolve_loaded: проверяет резолвер на ОБРАЗЕ, загруженном как LoadLibraryEx
// (isLoaded=true), то же, что делает DLL в explorer. Без DllMain, без explorer.
#include "../src/dll/resolve.h"
#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1] : L"C:\\Windows\\System32\\Taskbar.dll";
    HMODULE m = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) {
        printf("FAIL LoadLibraryEx GLE=%lu\n", GetLastError());
        return 1;
    }
    auto base = (const uint8_t*)((uintptr_t)m & ~(uintptr_t)3);
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    size_t imgSize = nt->OptionalHeader.SizeOfImage;
    printf("base=%p imageSize=0x%zx\n", (void*)base, imgSize);
    ResolvedAddrs r{};
    bool ok = ResolveTaskbarAddrs(base, imgSize, true, &r);
    if (!ok) {
        printf("FAIL resolve refused (isLoaded=true)\n");
        FreeLibrary(m);
        return 2;
    }
    printf("frame=0x%x icon=0x%x metrics=0x%x hash=0x%llx\n",
           r.getFrameSizeRva, r.getIconHeightRva, r.getMetricsRva,
           (unsigned long long)r.imageHash);
    FreeLibrary(m);
    printf("loaded-resolve PASS\n");
    return 0;
}
