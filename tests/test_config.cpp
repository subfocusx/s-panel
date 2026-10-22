// Dry test 1: config parsing + clamping + write roundtrip. Must exit 0, touches no system state.
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

static void TmpPath(wchar_t* out, const wchar_t* tag) {
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    wsprintfW(out, L"%ss-panel-test-%s-%lu.ini", dir, tag, GetCurrentProcessId());
}

int wmain() {
    wchar_t tmp[MAX_PATH]{};
    TmpPath(tmp, L"clamp");
    const char* ini =
        "[Panel]\nTaskbarHeight=10\nIconSize=99\nTaskbarButtonWidth=5\n"
        "TrayIconSize=100\nTaskbarIconSpacing=-3\nTrayIconSpacing=99\n";
    FILE* f = _wfopen(tmp, L"w");
    fputs(ini, f);
    fclose(f);

    PanelConfig c;
    CHECK(ReadPanelConfig(tmp, &c));
    CHECK(c.taskbarHeight == 24); // clamped up from 10
    CHECK(c.iconSize == 48);      // clamped down from 99
    CHECK(c.taskbarButtonWidth == 20);
    CHECK(c.trayIconSize == 48);
    CHECK(c.taskbarIconSpacing == 0); // clamped up from -3
    CHECK(c.trayIconSpacing == 32);   // clamped down from 99
    DeleteFileW(tmp);

    // Upper clamp for TaskbarHeight (72) and low value -> 24.
    TmpPath(tmp, L"hi");
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
          d.taskbarButtonWidth == 28 && d.trayIconSize == 32 &&
          d.taskbarIconSpacing == 0 && d.trayIconSpacing == 0);

    // Windows factory defaults (Reset target).
    PanelConfig wf = WindowsDefaults();
    CHECK(wf.taskbarHeight == 48 && wf.iconSize == 24 && wf.taskbarButtonWidth == 44 &&
          wf.trayIconSize == 32 && wf.taskbarIconSpacing == 0 && wf.trayIconSpacing == 0);
    for (int fld = 0; fld < kFieldCount; fld++) {
        int lo = 0, hi2 = 0;
        PanelFieldRange(fld, &lo, &hi2);
        int v = PanelFieldValue(wf, fld);
        CHECK(v >= lo && v <= hi2); // factory values must survive clamping
    }

    // Field accessors agree with keys (6 fields).
    for (int fld = 0; fld < kFieldCount; fld++) {
        const wchar_t* key = PanelFieldKey(fld);
        CHECK(key != nullptr);
        PanelConfig t;
        PanelFieldSet(&t, fld, 12345); // clamped into range
        int lo = 0, hi2 = 0;
        PanelFieldRange(fld, &lo, &hi2);
        CHECK(PanelFieldValue(t, fld) >= lo && PanelFieldValue(t, fld) <= hi2);
        PanelFieldSet(&t, fld, lo);
        CHECK(PanelFieldValue(t, fld) == lo);
        PanelFieldSet(&t, fld, hi2);
        CHECK(PanelFieldValue(t, fld) == hi2);
    }
    CHECK(PanelFieldKey(-1) == nullptr && PanelFieldKey(99) == nullptr);

    // Roundtrip: write boundary values, read back.
    TmpPath(tmp, L"rw");
    PanelConfig w;
    PanelFieldSet(&w, kFieldHeight, 24);
    PanelFieldSet(&w, kFieldIconSize, 48);
    PanelFieldSet(&w, kFieldButtonWidth, 120);
    PanelFieldSet(&w, kFieldTrayIconSize, 16);
    PanelFieldSet(&w, kFieldTaskbarSpacing, 7);
    PanelFieldSet(&w, kFieldTraySpacing, 32);
    std::wstring written = WritePanelConfig(tmp, w);
    CHECK(!written.empty());
    PanelConfig r;
    CHECK(ReadPanelConfig(tmp, &r));
    CHECK(r.taskbarHeight == 24 && r.iconSize == 48 && r.taskbarButtonWidth == 120 &&
          r.trayIconSize == 16 && r.taskbarIconSpacing == 7 && r.trayIconSpacing == 32);
    DeleteFileW(tmp);

    if (!g_fail) printf("config tests PASS\n");
    return g_fail ? 1 : 0;
}
