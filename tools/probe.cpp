// probe: call a candidate Taskbar.dll function in a SACRIFICIAL process.
// Usage: probe.exe <dllpath> <rva-hex> <int-arg>
// Calls double f(int) and prints the result. Any crash dies HERE,
// explorer.exe is never touched.
#include <windows.h>
#include <cstdio>
#include <cstdlib>

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        printf("usage: probe.exe <dll> <rva-hex> <int-arg>\n");
        return 3;
    }
    HMODULE m = LoadLibraryExW(argv[1], nullptr,
                               DONT_RESOLVE_DLL_REFERENCES);
    if (!m) {
        printf("LoadLibrary failed GLE=%lu\n", GetLastError());
        return 2;
    }
    printf("mapped without DllMain/imports (leaf-safe only)\n");
    fflush(stdout);
    unsigned long rva = wcstoul(argv[2], nullptr, 16);
    int arg = _wtoi(argv[3]);
    auto fn = (double(WINAPI*)(int))((BYTE*)m + rva);
    printf("calling...\n");
    fflush(stdout);
    double r = fn(arg);
    printf("call rva=0x%lx arg=%d -> %f\n", rva, arg, r);
    return 0;
}
