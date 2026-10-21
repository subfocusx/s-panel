#include "offsets.h"

void LogLastError(const wchar_t* what) {
    DWORD e = GetLastError();
    wchar_t buf[256]{};
    wsprintfW(buf, L"[s-panel] %s failed, GLE=%lu\n", what, e);
    OutputDebugStringW(buf);
}
