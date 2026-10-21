// loader.exe — injects taskbar.dll into explorer.exe only, then exits.
// Usage: loader.exe --check | --inject | --install | --uninstall
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include "../shared/config.h"

static DWORD FindExplorerPid() {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                // Session explorer (shell), not a stray file-manager window host.
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static std::wstring DllPathNextToExe() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s = exe;
    auto pos = s.find_last_of(L"\\/");
    s.resize(pos + 1);
    return s + L"taskbar.dll";
}

static bool IsX64Process(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    BOOL wow = FALSE;
    IsWow64Process(h, &wow);
    CloseHandle(h);
    return !wow; // non-WOW = native x64
}

static bool Inject(DWORD pid, const std::wstring& dll) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                PROCESS_VM_READ,
                            FALSE, pid);
    if (!h) {
        wprintf(L"OpenProcess failed GLE=%lu\n", GetLastError());
        return false;
    }
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        wprintf(L"VirtualAllocEx failed GLE=%lu\n", GetLastError());
        CloseHandle(h);
        return false;
    }
    if (!WriteProcessMemory(h, remote, dll.c_str(), bytes, nullptr)) {
        wprintf(L"WriteProcessMemory failed GLE=%lu\n", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }
    auto* loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(h, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) {
        wprintf(L"CreateRemoteThread failed GLE=%lu (run as admin?)\n",
                GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }
    WaitForSingleObject(th, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    if (!exitCode) {
        wprintf(L"LoadLibraryW in explorer returned NULL (arch mismatch?)\n");
        return false;
    }
    wprintf(L"injected OK, remote HMODULE=0x%lx\n", exitCode);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring cmd = argc > 1 ? argv[1] : L"--check";
    DWORD pid = FindExplorerPid();
    if (cmd == L"--check") {
        wprintf(L"explorer pid=%lu x64=%d\n", pid, (int)IsX64Process(pid));
        wprintf(L"dll=%s\n", DllPathNextToExe().c_str());
        PanelConfig c;
        ReadPanelConfig(nullptr, &c);
        wprintf(L"config default probe: height=%d icon=%d btn=%d tray=%d\n", c.taskbarHeight,
                c.iconSize, c.taskbarButtonWidth, c.trayIconSize);
        return pid ? 0 : 1;
    }
    if (cmd == L"--inject") {
        if (!pid) {
            wprintf(L"explorer.exe not found\n");
            return 1;
        }
        if (!IsX64Process(pid)) {
            wprintf(L"explorer is not x64, abort\n");
            return 1;
        }
        return Inject(pid, DllPathNextToExe()) ? 0 : 2;
    }
    wprintf(L"TODO: --install/--uninstall in Task 5 (scheduler + FreeLibrary)\n");
    return 3;
}
