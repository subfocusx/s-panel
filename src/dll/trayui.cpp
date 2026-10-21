// trayui.cpp — трей-иконка s-panel + popup с 6 полями.
// Один поток, собственный цикл сообщений. Иконка рисуется в рантайме через GDI.
#include "trayui.h"
#include "offsets.h"
#include <shellapi.h>
#include <commctrl.h>

namespace {

constexpr UINT kWmTrayIcon = WM_APP + 101;
constexpr UINT kTrayIconId = 0x5A1;
constexpr int kIconPx = 16;

const wchar_t* kFieldLabel[kFieldCount] = {
    L"Taskbar height", L"Icon size", L"Button width",
    L"Tray icons",     L"Taskbar spacing", L"Tray spacing",
};

// Состояние UI-потока.
struct TrayState {
    HANDLE thread = nullptr;
    DWORD threadId = 0;
    HWND popup = nullptr;
    HWND edits[kFieldCount] = {};
    HWND notifyWnd = nullptr; // скрытое окно для иконки
    NOTIFYICONDATAW nid = {};
    bool iconAdded = false;
    std::wstring cfgPath;
    PanelConfig cfg;
    TrayApplyFn applyFn = nullptr;
    bool running = false;
};
static TrayState g_tray;
static CRITICAL_SECTION g_trayLock;
static bool g_lockInit = false;

static void LockInit() {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_trayLock);
        g_lockInit = true;
    }
}

// Рисуем иконку 16x16: тёмный фон, синяя панель с двумя светлыми полосками.
static HICON MakeTrayIcon() {
    HDC hdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, kIconPx, kIconPx);
    HBITMAP mask = CreateBitmap(kIconPx, kIconPx, 1, 1, nullptr);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HGDIOBJ oldMask = SelectObject(CreateCompatibleDC(nullptr), mask);

    RECT rc{0, 0, kIconPx, kIconPx};
    HBRUSH bg = CreateSolidBrush(RGB(32, 36, 43));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    HBRUSH blue = CreateSolidBrush(RGB(10, 95, 180));
    RECT bar{1, 4, 15, 12};
    FillRect(mem, &bar, blue);
    DeleteObject(blue);
    HBRUSH lite = CreateSolidBrush(RGB(207, 228, 255));
    RECT r1{3, 6, 7, 10}, r2{8, 6, 13, 7}, r3{8, 9, 11, 10};
    FillRect(mem, &r1, lite);
    FillRect(mem, &r2, lite);
    FillRect(mem, &r3, lite);
    DeleteObject(lite);

    // Маска: всё непрозрачно.
    HDC mdc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldM = SelectObject(mdc, mask);
    PatBlt(mdc, 0, 0, kIconPx, kIconPx, WHITENESS);
    SelectObject(mdc, oldM);
    DeleteDC(mdc);

    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, hdc);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = bmp;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(bmp);
    DeleteObject(mask);
    return icon;
}

static void ApplyAndSave(int field, int value) {
    PanelFieldSet(&g_tray.cfg, field, value);
    WritePanelConfig(g_tray.cfgPath.c_str(), g_tray.cfg);
    if (g_tray.applyFn) g_tray.applyFn(g_tray.cfg);
}

static void RefreshEdits() {
    for (int f = 0; f < kFieldCount; f++) {
        if (!g_tray.edits[f]) continue;
        wchar_t buf[16];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", PanelFieldValue(g_tray.cfg, f));
        SetWindowTextW(g_tray.edits[f], buf);
    }
}

static int EditValue(int field) {
    wchar_t buf[32]{};
    GetWindowTextW(g_tray.edits[field], buf, _countof(buf));
    return _wtoi(buf);
}

static void StepField(int field, int delta) {
    int v = EditValue(field) + delta;
    int lo = 0, hi = 0;
    PanelFieldRange(field, &lo, &hi);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    ApplyAndSave(field, v);
    RefreshEdits();
}

static void CommitEdits() {
    for (int f = 0; f < kFieldCount; f++) {
        int v = EditValue(f);
        int lo = 0, hi = 0;
        PanelFieldRange(f, &lo, &hi);
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        PanelFieldSet(&g_tray.cfg, f, v);
    }
    WritePanelConfig(g_tray.cfgPath.c_str(), g_tray.cfg);
    if (g_tray.applyFn) g_tray.applyFn(g_tray.cfg);
    RefreshEdits();
}

// Позиция popup рядом с иконкой трея.
static void PlacePopup(HWND popup) {
    RECT rc{};
    if (Shell_NotifyIconGetRect != nullptr) {
        NOTIFYICONIDENTIFIER nii{};
        nii.cbSize = sizeof(nii);
        nii.hWnd = g_tray.notifyWnd;
        nii.uID = kTrayIconId;
        if (SUCCEEDED(Shell_NotifyIconGetRect(&nii, &rc))) {
            int w = 300, h = 280;
            HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(mon, &mi);
            int x = rc.left - w + (rc.right - rc.left) / 2;
            if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
            if (x < mi.rcWork.left) x = mi.rcWork.left;
            int y = rc.top - h - 8;
            if (y < mi.rcWork.top) y = rc.bottom + 8;
            SetWindowPos(popup, HWND_TOPMOST, x, y, w, h,
                         SWP_SHOWWINDOW | SWP_NOACTIVATE);
            return;
        }
    }
    // Fallback: правый нижний угол рабочей области.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(popup, HWND_TOPMOST, work.right - 300, work.bottom - 280, 300, 280,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

static LRESULT CALLBACK NotifyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kWmTrayIcon) {
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONUP || ev == WM_RBUTTONUP) {
            if (g_tray.popup && IsWindowVisible(g_tray.popup)) {
                ShowWindow(g_tray.popup, SW_HIDE);
            } else if (g_tray.popup) {
                RefreshEdits();
                PlacePopup(g_tray.popup);
                SetForegroundWindow(g_tray.popup);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id >= 1000 && id < 1000 + kFieldCount * 10) {
                int field = (id - 1000) / 10;
                int kind = (id - 1000) % 10; // 1 = minus, 2 = plus, 3 = edit
                if (kind == 1 && code == BN_CLICKED) {
                    StepField(field, -1);
                    return 0;
                }
                if (kind == 2 && code == BN_CLICKED) {
                    StepField(field, 1);
                    return 0;
                }
                if (kind == 3 && (code == EN_KILLFOCUS)) {
                    CommitEdits();
                    return 0;
                }
            } else if (id == 2001 && code == BN_CLICKED) {
                // Сброс.
                g_tray.cfg = PanelConfig{};
                WritePanelConfig(g_tray.cfgPath.c_str(), g_tray.cfg);
                if (g_tray.applyFn) g_tray.applyFn(g_tray.cfg);
                RefreshEdits();
                return 0;
            } else if (id == 2002 && code == BN_CLICKED) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                CommitEdits();
                return 0;
            }
            if (wp == VK_ESCAPE) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND MakeRow(HWND parent, int y, int field) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    CreateWindowExW(0, L"STATIC", kFieldLabel[field], WS_CHILD | WS_VISIBLE | SS_LEFT,
                   10, y + 4, 130, 18, parent, nullptr, hi, nullptr);
    HWND minus = CreateWindowExW(0, L"BUTTON", L"−", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 145, y, 24, 22, parent, (HMENU)(INT_PTR)(1000 + field * 10 + 1),
                                 hi, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                                WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
                                172, y, 56, 22, parent, (HMENU)(INT_PTR)(1000 + field * 10 + 3),
                                hi, nullptr);
    HWND plus = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                231, y, 24, 22, parent, (HMENU)(INT_PTR)(1000 + field * 10 + 2),
                                hi, nullptr);
    SendMessageW(edit, EM_SETLIMITTEXT, 3, 0);
    SendMessageW(minus, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(plus, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return edit;
}

static DWORD WINAPI TrayThread(LPVOID param) {
    std::wstring* cfgPath = (std::wstring*)param;
    {
        // Копия пути под замком уже снята вызывающим; здесь забираем владение.
        EnterCriticalSection(&g_trayLock);
        g_tray.cfgPath = *cfgPath;
        delete cfgPath;
        ReadPanelConfig(g_tray.cfgPath.c_str(), &g_tray.cfg);
        g_tray.running = true;
        LeaveCriticalSection(&g_trayLock);
    }

    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = NotifyWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"s-panel-tray";
    RegisterClassW(&wc);
    WNDCLASSW wp{};
    wp.lpfnWndProc = PopupWndProc;
    wp.hInstance = hi;
    wp.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wp.lpszClassName = L"s-panel-popup";
    RegisterClassW(&wp);

    g_tray.notifyWnd =
        CreateWindowExW(0, L"s-panel-tray", L"s-panel", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr);

    g_tray.popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"s-panel-popup", L"s-panel",
                                   WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 300, 280, nullptr,
                                   nullptr, hi, nullptr);

    int y = 12;
    for (int f = 0; f < kFieldCount; f++) {
        g_tray.edits[f] = MakeRow(g_tray.popup, y, f);
        y += 30;
    }
    int by = y + 6;
    HWND reset = CreateWindowExW(0, L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 120, by, 75, 24, g_tray.popup, (HMENU)2001, hi, nullptr);
    HWND close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 200, by, 75, 24, g_tray.popup, (HMENU)2002, hi, nullptr);
    SendMessageW(reset, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(close, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    RefreshEdits();

    // Иконка в трее.
    g_tray.nid.cbSize = sizeof(g_tray.nid);
    g_tray.nid.hWnd = g_tray.notifyWnd;
    g_tray.nid.uID = kTrayIconId;
    g_tray.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.nid.uCallbackMessage = kWmTrayIcon;
    g_tray.nid.hIcon = MakeTrayIcon();
    wcsncpy_s(g_tray.nid.szTip, L"s-panel", _TRUNCATE);
    g_tray.iconAdded = Shell_NotifyIconW(NIM_ADD, &g_tray.nid) ? true : false;
    if (!g_tray.iconAdded) {
        LogLine(L"TrayUi: NIM_ADD failed, popup still available");
    } else {
        LogLine(L"TrayUi: icon added");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Enter в поле ввода — применить.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND focus = GetFocus();
            for (int f = 0; f < kFieldCount; f++) {
                if (focus == g_tray.edits[f]) {
                    CommitEdits();
                    break;
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_tray.iconAdded) {
        NOTIFYICONDATAW del = g_tray.nid;
        Shell_NotifyIconW(NIM_DELETE, &del);
        g_tray.iconAdded = false;
    }
    if (g_tray.nid.hIcon) {
        DestroyIcon(g_tray.nid.hIcon);
        g_tray.nid.hIcon = nullptr;
    }
    if (g_tray.popup) {
        DestroyWindow(g_tray.popup);
        g_tray.popup = nullptr;
    }
    if (g_tray.notifyWnd) {
        DestroyWindow(g_tray.notifyWnd);
        g_tray.notifyWnd = nullptr;
    }
    LogLine(L"TrayUi thread exit");
    return 0;
}

} // namespace

bool TrayUiStart(const wchar_t* cfgPath, TrayApplyFn applyFn) {
    LockInit();
    EnterCriticalSection(&g_trayLock);
    if (g_tray.thread) {
        LeaveCriticalSection(&g_trayLock);
        return true;
    }
    g_tray.applyFn = applyFn;
    std::wstring* p = new std::wstring(cfgPath ? cfgPath : L"");
    g_tray.thread = CreateThread(nullptr, 0, TrayThread, p, 0, &g_tray.threadId);
    bool ok = g_tray.thread != nullptr;
    LeaveCriticalSection(&g_trayLock);
    LogLine(L"TrayUiStart %s", ok ? L"ok" : L"FAILED");
    return ok;
}

void TrayUiNoteExternalChange(const PanelConfig& cfg) {
    LockInit();
    EnterCriticalSection(&g_trayLock);
    g_tray.cfg = cfg;
    HWND popup = g_tray.popup;
    LeaveCriticalSection(&g_trayLock);
    if (popup && IsWindowVisible(popup)) RefreshEdits();
    LogLine(L"TrayUiNoteExternalChange h=%d", cfg.taskbarHeight);
}

void TrayUiStop() {
    LockInit();
    EnterCriticalSection(&g_trayLock);
    HANDLE th = g_tray.thread;
    DWORD tid = g_tray.threadId;
    LeaveCriticalSection(&g_trayLock);
    if (!th) return;
    PostThreadMessageW(tid, WM_QUIT, 0, 0);
    if (WaitForSingleObject(th, 2000) != WAIT_OBJECT_0) {
        LogLine(L"TrayUiStop: thread did not exit, terminating");
        TerminateThread(th, 0);
    }
    CloseHandle(th);
    EnterCriticalSection(&g_trayLock);
    g_tray.thread = nullptr;
    g_tray.threadId = 0;
    g_tray.running = false;
    for (int f = 0; f < kFieldCount; f++) g_tray.edits[f] = nullptr;
    LeaveCriticalSection(&g_trayLock);
    LogLine(L"TrayUiStop done");
}
