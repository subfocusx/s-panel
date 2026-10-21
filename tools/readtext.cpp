// readtext: READ-ONLY. Читает .text-секцию Taskbar.dll из памяти процесса и
// печатает FNV-1a хэш + тот же вывод резолвера. Ничего не пишет и не инжектит.
// Usage: readtext.exe <pid>
#include "../src/dll/resolve.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <vector>

static bool FindModule(DWORD pid, const wchar_t* name, MODULEENTRY32W* out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool ok = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) {
                *out = me;
                ok = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: readtext.exe <pid>\n");
        return 3;
    }
    DWORD pid = (DWORD)_wtoi(argv[1]);
    MODULEENTRY32W me{};
    if (!FindModule(pid, L"Taskbar.dll", &me)) {
        wprintf(L"Taskbar.dll not found in pid %lu\n", pid);
        return 1;
    }
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        wprintf(L"OpenProcess GLE=%lu\n", GetLastError());
        return 2;
    }
    const uint8_t* base = me.modBaseAddr;
    std::vector<uint8_t> img(me.modBaseSize);
    SIZE_T got = 0;
    if (!ReadProcessMemory(h, base, img.data(), img.size(), &got)) {
        wprintf(L"ReadProcessMemory GLE=%lu got=%zu\n", GetLastError(), got);
        CloseHandle(h);
        return 4;
    }
    CloseHandle(h);
    wprintf(L"base=%p size=0x%zx read=0x%zx\n", (void*)base, (size_t)me.modBaseSize, (size_t)got);
    ResolvedAddrs r{};
    bool ok = ResolveTaskbarAddrs(img.data(), img.size(), true, &r);
    if (!ok) {
        wprintf(L"resolver refused on live memory\n");
        return 5;
    }
    wprintf(L"frame=0x%x icon=0x%x metrics=0x%x hash=0x%llx\n", r.getFrameSizeRva,
            r.getIconHeightRva, r.getMetricsRva, (unsigned long long)r.imageHash);
    return 0;
}
