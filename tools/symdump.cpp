// symdump: list Taskbar.dll private symbols matching FrameSize/IconHeight/
// MeasureOverride/Tray via MS Symbol Server. Read-only, touches nothing.
// Usage: symdump.exe (writes symbols.txt next to exe)
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <string>
#pragma comment(lib, "dbghelp.lib")

static FILE* g_out = nullptr;

static BOOL CALLBACK EnumCb(PSYMBOL_INFO sym, ULONG, PVOID) {
    if (sym && sym->Name)
        fprintf(g_out ? g_out : stdout, "0x%llx %s\n",
                (unsigned long long)sym->Address, sym->Name);
    return TRUE;
}

int wmain() {
    wchar_t sysdir[MAX_PATH]{};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    std::wstring taskbar = std::wstring(sysdir) + L"\\Taskbar.dll";
    HANDLE h = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    wchar_t sympath[512]{};
    wsprintfW(sympath,
              L"srv*C:\\Symbols\\s-panel*http://msdl.microsoft.com/download/symbols");
    if (!SymInitializeW(h, sympath, FALSE)) {
        wprintf(L"SymInitialize failed GLE=%lu\n", GetLastError());
        return 1;
    }
    DWORD64 base =
        SymLoadModuleExW(h, nullptr, taskbar.c_str(), nullptr, 0, 0, nullptr, 0);
    if (!base) {
        wprintf(L"SymLoadModuleEx failed GLE=%lu (no network? symbols missing?)\n",
                GetLastError());
        SymCleanup(h);
        return 2;
    }
    wchar_t out[MAX_PATH]{};
    GetModuleFileNameW(nullptr, out, MAX_PATH);
    std::wstring s = out;
    s.resize(s.find_last_of(L"\\/") + 1);
    s += L"symbols.txt";
    g_out = _wfopen(s.c_str(), L"w");
    const char* masks[] = {"*FrameSize*",  "*IconHeight*", "*MeasureOverride*",
                           "*Tray*",       "*TaskbarHeight*", "*ButtonExtent*"};
    for (auto m : masks) {
        fprintf(g_out ? g_out : stdout, "=== %s ===\n", m);
        SymEnumSymbols(h, base, m, EnumCb, nullptr);
    }
    if (g_out) fclose(g_out);
    wprintf(L"wrote %s\n", s.c_str());
    SymUnloadModule64(h, base);
    SymCleanup(h);
    return 0;
}
