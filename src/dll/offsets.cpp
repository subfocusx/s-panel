#include "offsets.h"
#include <cstdarg>
#include <cstdio>
#include <string>

static std::wstring LogPath() {
    wchar_t path[MAX_PATH]{};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LogPath), &self))
        return L"s-panel.log";
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return L"s-panel.log";
    std::wstring s = path;
    auto pos = s.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"s-panel.log" : s.substr(0, pos + 1) + L"s-panel.log";
}

void LogLine(const wchar_t* fmt, ...) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t body[512]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t line[640]{};
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%02u:%02u:%02u.%03u pid=%lu tid=%lu %s\r\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 GetCurrentProcessId(), GetCurrentThreadId(), body);

    HANDLE f = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        OutputDebugStringW(line);
        return;
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (need > 1) {
        std::string utf8(static_cast<size_t>(need - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), need, nullptr, nullptr);
        DWORD written = 0;
        if (GetFileSize(f, nullptr) == 0) {
            const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
            WriteFile(f, bom, sizeof(bom), &written, nullptr);
        }
        WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(f);
    OutputDebugStringW(line);
}

void LogLastError(const wchar_t* what) {
    DWORD e = GetLastError();
    LogLine(L"%s failed, GLE=%lu", what, e);
}
