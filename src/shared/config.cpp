#include "config.h"
#include <windows.h>
#include <shlobj.h>

std::wstring DefaultIniPath(void* hModuleOrNull) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW((HMODULE)hModuleOrNull, path, MAX_PATH);
    std::wstring s = path;
    auto pos = s.find_last_of(L"\\/");
    if (pos != std::wstring::npos) s.resize(pos + 1);
    s += L"config.ini";
    return s;
}

std::wstring UserConfigPath() {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata)))
        return std::wstring();
    return std::wstring(appdata) + L"\\s-panel\\config.ini";
}

PanelConfig WindowsDefaults() {
    PanelConfig c;
    c.taskbarHeight = 48;
    c.iconSize = 24;
    c.taskbarButtonWidth = 44;
    c.trayIconSize = 32;
    c.taskbarIconSpacing = 0;
    c.trayIconSpacing = 0;
    return c;
}

const wchar_t* PanelFieldKey(int field) {
    switch (field) {
        case kFieldHeight: return L"TaskbarHeight";
        case kFieldIconSize: return L"IconSize";
        case kFieldButtonWidth: return L"TaskbarButtonWidth";
        case kFieldTrayIconSize: return L"TrayIconSize";
        case kFieldTaskbarSpacing: return L"TaskbarIconSpacing";
        case kFieldTraySpacing: return L"TrayIconSpacing";
        default: return nullptr;
    }
}

void PanelFieldRange(int field, int* minValue, int* maxValue) {
    int lo = 0, hi = 0;
    switch (field) {
        case kFieldHeight: lo = 24; hi = 72; break;
        case kFieldIconSize: lo = 12; hi = 48; break;
        case kFieldButtonWidth: lo = 20; hi = 120; break;
        case kFieldTrayIconSize: lo = 16; hi = 48; break;
        case kFieldTaskbarSpacing: lo = 0; hi = 32; break;
        case kFieldTraySpacing: lo = 0; hi = 32; break;
        default: break;
    }
    if (minValue) *minValue = lo;
    if (maxValue) *maxValue = hi;
}

static int ClampField(int field, int v) {
    int lo = 0, hi = 0;
    PanelFieldRange(field, &lo, &hi);
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int PanelFieldValue(const PanelConfig& cfg, int field) {
    switch (field) {
        case kFieldHeight: return cfg.taskbarHeight;
        case kFieldIconSize: return cfg.iconSize;
        case kFieldButtonWidth: return cfg.taskbarButtonWidth;
        case kFieldTrayIconSize: return cfg.trayIconSize;
        case kFieldTaskbarSpacing: return cfg.taskbarIconSpacing;
        case kFieldTraySpacing: return cfg.trayIconSpacing;
        default: return 0;
    }
}

void PanelFieldSet(PanelConfig* cfg, int field, int value) {
    if (!cfg) return;
    value = ClampField(field, value);
    switch (field) {
        case kFieldHeight: cfg->taskbarHeight = value; break;
        case kFieldIconSize: cfg->iconSize = value; break;
        case kFieldButtonWidth: cfg->taskbarButtonWidth = value; break;
        case kFieldTrayIconSize: cfg->trayIconSize = value; break;
        case kFieldTaskbarSpacing: cfg->taskbarIconSpacing = value; break;
        case kFieldTraySpacing: cfg->trayIconSpacing = value; break;
        default: break;
    }
}

bool ReadPanelConfig(const wchar_t* iniPath, PanelConfig* out) {
    if (!out) return false;
    std::wstring p;
    if (iniPath && *iniPath) {
        p = iniPath;
    } else {
        p = DefaultIniPath(nullptr);
        // For DLL: nullptr gives host (explorer) path — caller passes DLL path explicitly.
    }
    PanelConfig cfg; // defaults from header
    for (int f = 0; f < kFieldCount; f++) {
        const wchar_t* key = PanelFieldKey(f);
        if (!key) continue;
        int v = GetPrivateProfileIntW(L"Panel", key, PanelFieldValue(cfg, f), p.c_str());
        PanelFieldSet(&cfg, f, v);
    }
    *out = cfg;
    return true;
}

std::wstring WritePanelConfig(const wchar_t* iniPath, const PanelConfig& cfg) {
    std::wstring primary = (iniPath && *iniPath) ? iniPath : DefaultIniPath(nullptr);
    wchar_t body[512];
    // swprintf_s доступен без CRT-зависимостей, пишем весь файл целиком.
    _snwprintf_s(body, _countof(body), _TRUNCATE,
                 L"[Panel]\r\nTaskbarHeight=%d\r\nIconSize=%d\r\n"
                 L"TaskbarButtonWidth=%d\r\nTrayIconSize=%d\r\n"
                 L"TaskbarIconSpacing=%d\r\nTrayIconSpacing=%d\r\n",
                 cfg.taskbarHeight, cfg.iconSize, cfg.taskbarButtonWidth, cfg.trayIconSize,
                 cfg.taskbarIconSpacing, cfg.trayIconSpacing);

    auto tryWrite = [&](const std::wstring& path) -> bool {
        // Создать каталог при необходимости.
        auto pos = path.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring dir = path.substr(0, pos);
            CreateDirectoryW(dir.c_str(), nullptr); // ok if exists
        }
        std::wstring tmp = path + L".tmp";
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        int need = WideCharToMultiByte(CP_ACP, 0, body, -1, nullptr, 0, nullptr, nullptr);
        bool ok = false;
        if (need > 1) {
            std::string mb(static_cast<size_t>(need - 1), '\0');
            WideCharToMultiByte(CP_ACP, 0, body, -1, mb.data(), need, nullptr, nullptr);
            DWORD written = 0;
            ok = WriteFile(f, mb.data(), static_cast<DWORD>(mb.size()), &written, nullptr) &&
                 written == mb.size();
        }
        CloseHandle(f);
        if (!ok) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    };

    if (tryWrite(primary)) return primary;
    std::wstring user = UserConfigPath();
    if (!user.empty() && user != primary && tryWrite(user)) return user;
    return std::wstring();
}
