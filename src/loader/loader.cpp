// loader.exe — injects taskbar.dll into explorer.exe only, then exits.
// Usage: loader.exe --check | --inject | --install | --uninstall
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <shlobj.h>
#include "../shared/config.h"
#include "../dll/trayui.h"
#include "../dll/offsets.h"

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

static HMODULE FindRemoteModule(DWORD pid, const std::wstring& dllPath) {    // Сравниваем по ПОЛНОМУ пути: имя taskbar.dll совпадает с системной
    // C:\Windows\System32\Taskbar.dll, поиск по имени даёт ложные срабатывания.
    HMODULE found = nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szExePath, dllPath.c_str()) == 0) {
                found = me.hModule;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// Ждём, пока оболочка поднимет Taskbar.View.dll (модуль XAML-панели). Без него
// хуки в DLL не ставятся, поэтому инжек делаем только после его появления.
static bool WaitForTaskbarView(DWORD pid, DWORD timeoutMs) {
    for (DWORD waited = 0; waited <= timeoutMs; waited += 500) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            if (Module32FirstW(snap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, L"Taskbar.View.dll") == 0) {
                        CloseHandle(snap);
                        return true;
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
        }
        Sleep(500);
    }
    return false;
}

static DWORD ExportRva(const wchar_t* dllFile, const char* name) {
    HANDLE f = CreateFileW(dllFile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz{};
    DWORD rva = 0;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0x1000 && sz.QuadPart < 64 * 1024 * 1024) {
        size_t n = (size_t)sz.QuadPart;
        uint8_t* img = new (std::nothrow) uint8_t[n];
        DWORD rd = 0;
        if (img && ReadFile(f, img, (DWORD)n, &rd, nullptr) && rd == n) {
            auto dos = (IMAGE_DOS_HEADER*)img;
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                auto nt = (IMAGE_NT_HEADERS64*)(img + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE) {
                    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                    if (dir.VirtualAddress && dir.Size) {
                        // Найти raw-смещение таблицы экспорта через секции.
                        auto secs = IMAGE_FIRST_SECTION(nt);
                        DWORD expOff = 0;
                        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                            DWORD va = secs[i].VirtualAddress;
                            if (dir.VirtualAddress >= va &&
                                dir.VirtualAddress < va + secs[i].Misc.VirtualSize) {
                                expOff = secs[i].PointerToRawData + (dir.VirtualAddress - va);
                                break;
                            }
                        }
                        if (expOff) {
                            auto exp = (IMAGE_EXPORT_DIRECTORY*)(img + expOff);
                            auto rvaToOff = [&](DWORD v) -> DWORD {
                                for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                                    DWORD va = secs[i].VirtualAddress;
                                    if (v >= va && v < va + secs[i].Misc.VirtualSize)
                                        return secs[i].PointerToRawData + (v - va);
                                }
                                return 0;
                            };
                            DWORD namesOff = rvaToOff(exp->AddressOfNames);
                            DWORD ordOff = rvaToOff(exp->AddressOfNameOrdinals);
                            DWORD funcOff = rvaToOff(exp->AddressOfFunctions);
                            if (namesOff && ordOff && funcOff) {
                                auto names = (DWORD*)(img + namesOff);
                                auto ords = (WORD*)(img + ordOff);
                                auto funcs = (DWORD*)(img + funcOff);
                                size_t wantLen = strlen(name);
                                for (DWORD i = 0; i < exp->NumberOfNames; i++) {
                                    DWORD no = rvaToOff(names[i]);
                                    if (!no) continue;
                                    const char* s = (const char*)(img + no);
                                    if (strncmp(s, name, wantLen + 1) == 0) {
                                        rva = funcs[ords[i]];
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        delete[] img;
    }
    CloseHandle(f);
    return rva;
}

static bool RemoteCall(DWORD pid, void* addr, void* arg, DWORD timeoutMs, bool checkExit) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) {
        wprintf(L"OpenProcess failed GLE=%lu\n", GetLastError());
        return false;
    }
    HANDLE th = CreateRemoteThread(h, nullptr, 0, (LPTHREAD_START_ROUTINE)addr, arg, 0, nullptr);
    if (!th) {
        wprintf(L"CreateRemoteThread failed GLE=%lu\n", GetLastError());
        CloseHandle(h);
        return false;
    }
    DWORD w = WaitForSingleObject(th, timeoutMs);
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    CloseHandle(h);
    if (w != WAIT_OBJECT_0) {
        wprintf(L"remote call timed out\n");
        return false;
    }
    // checkExit=false для void-функций (SpanelUninstall): важен факт завершения.
    if (checkExit && (exitCode == 0 || exitCode == STILL_ACTIVE)) return false;
    return true;
}

// Вызвать SpanelUninstall внутри explorer: снять хуки и остановить потоки DLL.
static bool RemoteUninstall(DWORD pid, HMODULE remoteBase, const std::wstring& dllFile) {
    DWORD rva = ExportRva(dllFile.c_str(), "SpanelUninstall");
    if (!rva) {
        wprintf(L"SpanelUninstall export not found (old build?), skip graceful stop\n");
        return true; // не фатально: дальше FreeLibrary-цикл
    }
    void* addr = (uint8_t*)remoteBase + rva;
    wprintf(L"calling SpanelUninstall at %p...\n", addr);
    return RemoteCall(pid, addr, nullptr, 8000, false);
}

static bool Uninject(DWORD pid) {
    HMODULE mod = FindRemoteModule(pid, DllPathNextToExe());
    if (!mod) {
        wprintf(L"taskbar.dll not loaded in explorer, nothing to do\n");
        return true;
    }
    std::wstring dllFile = DllPathNextToExe();
    if (!RemoteUninstall(pid, mod, dllFile)) {
        wprintf(L"warning: SpanelUninstall call failed, trying FreeLibrary anyway\n");
    }
    auto* freeLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                          "FreeLibrary");
    // Счётчик ссылок может быть >1 (повторный инжект старых сборок) — выгружаем до конца.
    for (int i = 0; i < 3; i++) {
        HMODULE cur = FindRemoteModule(pid, DllPathNextToExe());
        if (!cur) break;
        if (!RemoteCall(pid, (void*)freeLib, cur, 10000, true)) {
            wprintf(L"FreeLibrary in explorer failed (try %d)\n", i + 1);
            break;
        }
    }
    if (FindRemoteModule(pid, DllPathNextToExe())) {
        wprintf(L"taskbar.dll STILL loaded — restart explorer to fully unload\n");
        return false;
    }
    wprintf(L"taskbar.dll unloaded from explorer\n");
    return true;
}

static HANDLE g_trayExit = nullptr;

static BOOL WINAPI TrayCtrlHandler(DWORD type) {
    if ((type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) &&
        g_trayExit) {
        SetEvent(g_trayExit);
        return TRUE;
    }
    return FALSE;
}

// Именованное событие «показать панель»: позволяет двойным кликом по ярлыку
// открыть панель настроек, не поднимая вторую копию трея.
static const wchar_t* kShowPopupEvent = L"Local\\s-panel-show-popup";

// Поток-слушатель: ждёт сигнала и просит поток трея показать панель.
static DWORD WINAPI ShowPopupWaitThread(LPVOID param) {
    // Ручку события передаём извне: OpenEventW без SYNCHRONIZE не даёт ждать.
    HANDLE ev = (HANDLE)param;
    if (!ev) return 0;
    for (;;) {
        DWORD w = WaitForSingleObject(ev, INFINITE);
        if (w != WAIT_OBJECT_0) break;
        ResetEvent(ev);
        LogLine(L"loader: show-popup signal received");
        TrayUiShowPopup();
        LogLine(L"loader: show-popup requested");
    }
    return 0;
}

// Постоянный режим: иконка в трее + попап настроек. Пишет config.ini,
// DLL в explorer подхватывает изменения вотчером (applyFn=null — применение
// идёт через файл, а не прямым вызовом).
static int RunTray(bool showPopup) {
    // Если демон уже работает — сигналим ему и выходим.
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, kShowPopupEvent);
    if (ev && GetLastError() == ERROR_ALREADY_EXISTS) {
        SetEvent(ev);
        CloseHandle(ev);
        wprintf(L"s-panel already running, popup requested\n");
        return 0;
    }
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini = exe;
    auto pos = ini.find_last_of(L"\\/");
    ini = (pos == std::wstring::npos) ? L"config.ini" : ini.substr(0, pos + 1) + L"config.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        PanelConfig dflt;
        WritePanelConfig(ini.c_str(), dflt);
    }
    if (!TrayUiStart(ini.c_str(), nullptr)) {
        wprintf(L"TrayUiStart failed\n");
        return 2;
    }
    wprintf(L"s-panel tray running (config=%s). Ctrl+C to stop.\n", ini.c_str());
    if (ev) {
        HANDLE th = CreateThread(nullptr, 0, ShowPopupWaitThread, ev, 0, nullptr);
        if (th) CloseHandle(th);
        LogLine(L"loader: show-popup waiter started, wantPopup=%d", (int)showPopup);
        if (showPopup) {
            SetEvent(ev);
            LogLine(L"loader: show-popup event set");
        }
    } else {
        LogLine(L"loader: show-popup event creation failed GLE=%lu", GetLastError());
    }
    g_trayExit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(TrayCtrlHandler, TRUE);
    WaitForSingleObject(g_trayExit, INFINITE);
    TrayUiStop();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring cmd = argc > 1 ? argv[1] : L"--check";
    DWORD pid = FindExplorerPid();
    if (cmd == L"--check") {
        wprintf(L"explorer pid=%lu x64=%d\n", pid, (int)IsX64Process(pid));
        wprintf(L"dll=%s\n", DllPathNextToExe().c_str());
        std::wstring ini = DllPathNextToExe();
        auto pos = ini.find_last_of(L"\\/");
        ini = (pos == std::wstring::npos) ? L"config.ini" : ini.substr(0, pos + 1) + L"config.ini";
        PanelConfig c;
        ReadPanelConfig(ini.c_str(), &c);
        wprintf(L"config=%s\n", ini.c_str());
        wprintf(L"  height=%d icon=%d btn=%d tray=%d taskbarSpacing=%d traySpacing=%d\n",
                c.taskbarHeight, c.iconSize, c.taskbarButtonWidth, c.trayIconSize,
                c.taskbarIconSpacing, c.trayIconSpacing);
        wprintf(L"loaded in explorer: %s\n",
                FindRemoteModule(pid, DllPathNextToExe()) ? L"yes" : L"no");
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
        if (FindRemoteModule(pid, DllPathNextToExe())) {
            wprintf(L"taskbar.dll already loaded in explorer, skip inject\n");
            return 0;
        }
        if (!WaitForTaskbarView(pid, 30000)) {
            wprintf(L"Taskbar.View.dll not loaded in explorer after 30s, abort\n");
            return 2;
        }
        return Inject(pid, DllPathNextToExe()) ? 0 : 2;
    }
    if (cmd == L"--uninstall") {
        if (!pid) {
            wprintf(L"explorer.exe not found\n");
            return 1;
        }
        if (!IsX64Process(pid)) {
            wprintf(L"explorer is not x64, abort\n");
            return 1;
        }
        return Uninject(pid) ? 0 : 2;
    }
    if (cmd == L"--install") {
        bool withAutostart = (argc > 2 && std::wstring(argv[2]) == L"--with-autostart");
        wchar_t appdata[MAX_PATH]{};
        // SHGetFolderPathW из shell32 — отдельной линковки не требует (уже в умолчаниях).
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata))) {
            wprintf(L"cannot resolve LOCALAPPDATA\n");
            return 2;
        }
        std::wstring dir = std::wstring(appdata) + L"\\s-panel";
        CreateDirectoryW(dir.c_str(), nullptr);
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring srcDir = exe;
        auto p = srcDir.find_last_of(L"\\/");
        srcDir = (p == std::wstring::npos) ? L"" : srcDir.substr(0, p + 1);
        bool ok = CopyFileW((srcDir + L"taskbar.dll").c_str(), (dir + L"\\taskbar.dll").c_str(), FALSE) &&
                  CopyFileW(exe, (dir + L"\\loader.exe").c_str(), FALSE);
        if (!ok) {
            wprintf(L"copy failed GLE=%lu\n", GetLastError());
            return 2;
        }
        std::wstring ini = dir + L"\\config.ini";
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
            PanelConfig dflt;
            WritePanelConfig(ini.c_str(), dflt);
        }
        wprintf(L"installed to %s\n", dir.c_str());
        std::wstring taskCmd = L"schtasks /Create /TN \"s-panel\" /SC ONLOGON /RL LIMITED /TR \"\\\"" +
                               dir + L"\\loader.exe\\\" --inject-and-tray\"";
        wprintf(L"autostart command:\n  %s\n", taskCmd.c_str());
        wprintf(L"(--inject-and-tray: injects the DLL, then keeps the tray icon alive)\n");
        if (withAutostart) {
            int rc = _wsystem(taskCmd.c_str());
            wprintf(L"schtasks exit=%d\n", rc);
            return rc == 0 ? 0 : 2;
        }
        wprintf(L"task NOT created (pass --with-autostart to create)\n");
        return 0;
    }
    if (cmd == L"--uninstall-task") {
        int rc = _wsystem(L"schtasks /Delete /TN \"s-panel\" /F");
        wprintf(L"schtasks exit=%d\n", rc);
        return rc == 0 ? 0 : 2;
    }

    bool wantPopup = false;
    for (int i = 2; i < argc; i++) {
        if (std::wstring(argv[i]) == L"--show-popup") wantPopup = true;
    }
    if (cmd == L"--tray") {
        return RunTray(wantPopup);
    }
    if (cmd == L"--inject-and-tray") {
        if (!pid) {
            wprintf(L"explorer.exe not found\n");
            return 1;
        }
        if (!IsX64Process(pid)) {
            wprintf(L"explorer is not x64, abort\n");
            return 1;
        }
        if (!FindRemoteModule(pid, DllPathNextToExe())) {
            if (!WaitForTaskbarView(pid, 30000)) {
                wprintf(L"Taskbar.View.dll not loaded in explorer after 30s, abort\n");
                return 2;
            }
            if (!Inject(pid, DllPathNextToExe())) return 2;
        } else {
            wprintf(L"taskbar.dll already loaded in explorer, skip inject\n");
        }
        return RunTray(wantPopup);
    }
    wprintf(L"commands: --check --inject --uninstall\n"
            L"          --tray (tray icon only) --inject-and-tray\n"
            L"          --install [--with-autostart] --uninstall-task\n"
            L"          (--show-popup: открыть панель настроек сразу)\n");
    return 3;
}
