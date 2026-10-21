// Dry test 1: config parsing + clamping. Must exit 0, touches no system state.
#include "../src/shared/config.h"
#include <windows.h>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            g_fail++;                                      \
        }                                                  \
    } while (0)

int wmain() {
    wchar_t tmp[MAX_PATH]{}, dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    wsprintfW(tmp, L"%ss-panel-test-%lu.ini", dir, GetCurrentProcessId());
    const char* ini =
        "[Panel]\nTaskbarHeight=10\nIconSize=99\nTaskbarButtonWidth=5\n"
        "TrayIconSize=100\n";
    FILE* f = _wfopen(tmp, L"w");
    fputs(ini, f);
    fclose(f);

    PanelConfig c;
    CHECK(ReadPanelConfig(tmp, &c));
    CHECK(c.taskbarHeight == 24); // clamped up from 10
    CHECK(c.iconSize == 48);      // clamped down from 99
    CHECK(c.taskbarButtonWidth == 20);
    CHECK(c.trayIconSize == 48);
    DeleteFileW(tmp);

    // Upper clamp for TaskbarHeight (72) and low value -> 24.
    wsprintfW(tmp, L"%ss-panel-test-hi-%lu.ini", dir, GetCurrentProcessId());
    f = _wfopen(tmp, L"w");
    fputs("[Panel]\nTaskbarHeight=999\n", f);
    fclose(f);
    PanelConfig hi;
    CHECK(ReadPanelConfig(tmp, &hi));
    CHECK(hi.taskbarHeight == 72);
    DeleteFileW(tmp);

    // Missing file -> safe defaults, still true.
    PanelConfig d;
    CHECK(ReadPanelConfig(L"C:\\nonexistent\\s-panel.ini", &d));
    CHECK(d.taskbarHeight == 34 && d.iconSize == 16 &&
          d.taskbarButtonWidth == 28 && d.trayIconSize == 16);

    if (!g_fail) printf("config tests PASS\n");
    return g_fail ? 1 : 0;
}
