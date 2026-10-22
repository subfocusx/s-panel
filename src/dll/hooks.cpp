// hooks.cpp — хуки Taskbar.dll (System32) и Taskbar.View.dll (24H2 build 26100).
//
// Механизм повторяет проверенный мод Windhawk taskbar-icon-size v1.3.10 (GPLv3):
//  - IconUtils::GetIconSize (Taskbar.dll): масштабирует SIZE иконок.
//  - IconContainer::IsStorageRecreationRequired (Taskbar.dll): true во время
//    применения — иконки пересоздаются с новым размером.
//  - GetIconHeightInViewPixels x2 (Taskbar.View): отдаёт наш размер иконок.
//  - ResourceDictionary::Lookup (Taskbar.View): подменяет Medium/SmallTaskbarButtonExtent.
//  - TaskbarFrame::GetMetrics (Taskbar.View): правит кэшированный extent кнопок.
//  - GetFrameSize (Taskbar.View): высота панели (уже работало).
//  - SystemTrayController::GetFrameSize (Taskbar.View): высота области трея.
//
// Точки хука берутся из PDB (GUID/age проверяются) и проверяются на попадание
// в .text. Символ не найден — хук не ставится, остальные работают (частичный режим).
// explorer.exe работает с CFG: косвенные вызовы проверяются по bitmap. Оригиналы
// MinHook живут в частной памяти — каждый трамплин маркируется через
// SetProcessValidCallTargets (страница целиком, n = 256; Marquez: частичное
// покрытие и мультистраничные вызовы возвращают GLE=87/487).
#include "offsets.h"
#include "../shared/config.h"
#include "MinHook.h"
#include <psapi.h>
#include <unknwn.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <shellapi.h>
#include <winuser.h>
#include <atomic>
#include <cstring>
#include <cstdint>

static constexpr const char* kViewGuid = "0AE0E302E8E34BE29CC303A7A62ABCA7";
static constexpr DWORD kViewAge = 1;
static constexpr const char* kTaskbarGuid = "1C6067D3BC7AABA7CDE214B3AE66C6D3";
static constexpr DWORD kTaskbarAge = 1;

// RVA:
//  Taskbar.dll (System32, GUID 1C6067D3... age 1, imageSize 0x309000):
//   0x2D0CC IconUtils::GetIconSize (void(bool, IconType, SIZE*))
//           ?GetIconSize@IconUtils@@YAX_NW4IconType@1@PEAUtagSIZE@@@Z
//   0x914D0 IconContainer::IsStorageRecreationRequired (bool(this, array&, flags))
//  Taskbar.View.dll (GUID 0AE0E302... age 1):
//   0x7F4B0  TaskbarConfiguration::GetFrameSize (static double(TaskbarSize))
//   0x1CA628 TaskbarConfiguration::GetIconHeightInViewPixels (static double(TaskbarSize))
//   0x280BD4 TaskbarConfiguration::GetIconHeightInViewPixels (double(this), с KB5044384)
//   0x37F3D8 TaskbarFrame::GetMetrics (metrics+8 = button extent)
//   0x98970  ResourceDictionary IMap::Lookup consume (перехват extent-ключей)
//   0x26CE2C SystemTrayController::GetFrameSize (double(this, TaskbarSize))
//   0x376AFC TaskListButton::UpdateButtonPadding (void(this), точка входа для ресайза)
//   0x376DF4 TaskListButton::UpdateIconColumnDefinition (void(this), точка входа)
static constexpr DWORD kRvaGetIconSize = 0x2D0CC;
static constexpr DWORD kRvaIsStorageRecreationRequired = 0x914D0;
static constexpr DWORD kRvaGetFrameSize = 0x7F4B0;
static constexpr DWORD kRvaGetIconHeight = 0x1CA628;
static constexpr DWORD kRvaGetIconHeightMethod = 0x280BD4;
static constexpr DWORD kRvaGetMetrics = 0x37F3D8;
static constexpr DWORD kRvaLookup = 0x98970;
static constexpr DWORD kRvaTrayGetFrameSize = 0x26CE2C;
static constexpr DWORD kRvaUpdateButtonPadding = 0x376AFC;
static constexpr DWORD kRvaUpdateIconColumnDefinition = 0x376DF4;
static constexpr DWORD kRvaUpdateVisualStates = 0x13E54;

// Живые значения: читаются хуками из потока UI explorer, пишутся из вотчера.
struct LiveValues {
    std::atomic<int> height{34};
    std::atomic<int> iconSize{16};
    std::atomic<int> buttonWidth{28};
    std::atomic<int> trayIconSize{16};
};
static LiveValues g_live;

// Флаг применения: IsStorageRecreationRequired возвращает true, пока идёт apply.
static std::atomic<bool> g_applying{false};

// Окно применения (мс): обход дерева и пересоздание идут асинхронно следом
// за storm — покрываем их окном, а не мгновенным флагом.
static std::atomic<ULONGLONG> g_applyUntil{0};
static constexpr ULONGLONG kApplyWindowMs = 8000;

static bool ApplyingNow() {
    return GetTickCount64() < g_applyUntil.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------- вертикальная панель
static volatile LONG g_vertCache = 0;
static volatile ULONGLONG g_vertCheckedAt = 0;
static constexpr ULONGLONG kVertRefreshMs = 1000;

static bool QueryVerticalTaskbar() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) return false;
    return abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT;
}

static bool IsVerticalTaskbarCached() {
    ULONGLONG now = GetTickCount64();
    if (now - g_vertCheckedAt >= kVertRefreshMs) {
        InterlockedExchange(&g_vertCache, QueryVerticalTaskbar() ? 1 : 0);
        g_vertCheckedAt = now;
    }
    return g_vertCache != 0;
}

// ---------------------------------------------------------------- хуки
static volatile LONG g_hookCalls = 0;

// --- Taskbar.dll: размер иконок ---
using GetIconSize_t = void(WINAPI*)(bool isSmall, int type, SIZE* size);
static GetIconSize_t GetIconSize_Orig = nullptr;

static void WINAPI GetIconSize_Hook(bool isSmall, int type, SIZE* size) {
    GetIconSize_Orig(isSmall, type, size);
    if (isSmall) return; // мелкие иконки не трогаем (как в моде)
    int want = g_live.iconSize.load(std::memory_order_relaxed);
    if (size && size->cx > 0)
        size->cx = MulDiv(size->cx, want, 24);
    if (size && size->cy > 0)
        size->cy = MulDiv(size->cy, want, 24);
    LONG n = InterlockedIncrement(&g_hookCalls);
    if (n <= 3)
        LogLine(L"GetIconSize_Hook call=%ld small=%d type=%d size=%ldx%ld", n,
                (int)isSmall, type, size ? size->cx : -1, size ? size->cy : -1);
}

using IsStorageRecreationRequired_t = bool(WINAPI*)(void* pThis, void* param1, int flags);
static IsStorageRecreationRequired_t IsStorageRecreationRequired_Orig = nullptr;

static bool WINAPI IsStorageRecreationRequired_Hook(void* pThis, void* param1, int flags) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    if (n <= 10)
        LogLine(L"IsStorageRecreationRequired_Hook call=%ld this=%p applying=%d", n, pThis,
                (int)ApplyingNow());
    if (ApplyingNow()) return true;
    return IsStorageRecreationRequired_Orig(pThis, param1, flags);
}

// --- Taskbar.View: высота ---
using GetFrameSize_t = double(WINAPI*)(int);
static GetFrameSize_t GetFrameSize_Orig = nullptr;

static double WINAPI GetFrameSize_Hook(int e) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    bool vertical = IsVerticalTaskbarCached();
    double ret;
    if (!vertical && (e == 1 || e == 2))
        ret = (double)g_live.height.load(std::memory_order_relaxed);
    else
        ret = 48.0; // вертикаль/прочее — константа, Orig не вызываем (CFG)
    if (n == 1 || n % 500 == 0)
        LogLine(L"GetFrameSize_Hook call=%ld e=%d vertical=%d ret=%f", n, e, vertical ? 1 : 0, ret);
    return ret;
}

using GetTrayFrameSize_t = double(*)(void* thisPtr, int e);
static GetTrayFrameSize_t GetTrayFrameSize_Orig = nullptr;

static double GetTrayFrameSize_Hook(void* self, int e) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    bool vertical = IsVerticalTaskbarCached();
    double ret;
    if (!vertical && (e == 1 || e == 2))
        ret = (double)g_live.height.load(std::memory_order_relaxed);
    else
        ret = GetTrayFrameSize_Orig(self, e);
    if (n == 1 || n % 200 == 0)
        LogLine(L"GetTrayFrameSize_Hook call=%ld e=%d vertical=%d ret=%f", n, e,
                vertical ? 1 : 0, ret);
    return ret;
}

// --- Taskbar.View: размер иконок ---
using GetIconHeight_t = double(WINAPI*)(int);
static GetIconHeight_t GetIconHeight_Orig = nullptr;

static double WINAPI GetIconHeight_Hook(int e) {
    double ret;
    if (!IsVerticalTaskbarCached() && (e == 1 || e == 2))
        ret = (double)g_live.iconSize.load(std::memory_order_relaxed);
    else
        ret = 24.0;
    LONG n = InterlockedIncrement(&g_hookCalls);
    if (n <= 3 || n % 200 == 0)
        LogLine(L"GetIconHeight_Hook call=%ld e=%d ret=%f", n, e, ret);
    return ret;
}

using GetIconHeightMethod_t = double(*)(void* thisPtr);
static GetIconHeightMethod_t GetIconHeightMethod_Orig = nullptr;

static double GetIconHeightMethod_Hook(void* self) {
    double stock = GetIconHeightMethod_Orig(self);
    double ret = (double)g_live.iconSize.load(std::memory_order_relaxed);
    LONG n = InterlockedIncrement(&g_hookCalls);
    if (n <= 3 || n % 200 == 0)
        LogLine(L"GetIconHeightMethod_Hook call=%ld stock=%f ret=%f", n, stock, ret);
    (void)stock;
    return ret;
}

// --- Taskbar.View: ширина кнопок ---
using Lookup_t = winrt::Windows::Foundation::IInspectable*(WINAPI*)(
    void* pThis, void** result, winrt::Windows::Foundation::IInspectable* key);
static Lookup_t Lookup_Orig = nullptr;

// Зеркалит Windhawk taskbar-icon-size: подмена Medium/SmallTaskbarButtonExtent.
// Параметры — уже спроецированные типы (как в моде), ABI совпадает.
static winrt::Windows::Foundation::IInspectable* WINAPI Lookup_Hook(
    void* pThis, void** result, winrt::Windows::Foundation::IInspectable* key) {
    auto ret = Lookup_Orig(pThis, result, key);
    if (!ret) return ret;
    const auto keyString = key->try_as<winrt::hstring>();
    if (!keyString) return ret;
    double want = (double)g_live.buttonWidth.load(std::memory_order_relaxed);
    if (*keyString == L"MediumTaskbarButtonExtent" ||
        *keyString == L"SmallTaskbarButtonExtent") {
        const auto cur = ret->try_as<double>();
        if (cur && *cur != want) {
            LogLine(L"Lookup override %s: %f->%f", keyString->c_str(), *cur, want);
            *ret = winrt::box_value(want);
        }
    }
    return ret;
}

using GetMetrics_t = void*(WINAPI*)(void* pThis, void* metrics);
static GetMetrics_t GetMetrics_Orig = nullptr;

static void ResizeSubtreeIcons(winrt::Windows::UI::Xaml::FrameworkElement parent);

static void* WINAPI GetMetrics_Hook(void* pThis, void* metrics) {
    void* ret = GetMetrics_Orig(pThis, metrics);
    // Button extent — второй double в TaskbarFrameMetrics.
    double* extent = (double*)((uint8_t*)metrics + sizeof(double));
    double want = (double)g_live.buttonWidth.load(std::memory_order_relaxed);
    if (*extent >= 1 && *extent < 10000 && *extent != want) {
        LONG n = InterlockedIncrement(&g_hookCalls);
        if (n <= 8)
            LogLine(L"GetMetrics: button extent %f->%f", *extent, want);
        *extent = want;
    }
    // Обход дерева от кадра: GetMetrics срабатывает при каждом пересчёте.
    if (ApplyingNow()) {
        try {
            winrt::Windows::UI::Xaml::FrameworkElement frameElement = nullptr;
            IUnknown* inner = (IUnknown*)((uint8_t*)pThis + 24);
            if (SUCCEEDED(inner->QueryInterface(
                    winrt::guid_of<winrt::Windows::UI::Xaml::FrameworkElement>(),
                    winrt::put_abi(frameElement))) &&
                frameElement) {
                LONG n = InterlockedIncrement(&g_hookCalls);
                if (n <= 8) {
                    try {
                        LogLine(L"GetMetrics walk root: %s",
                                winrt::get_class_name(frameElement).c_str());
                    } catch (...) {
                    }
                }
                ResizeSubtreeIcons(frameElement);
            }
        } catch (...) {
        }
    }
    return ret;
}

// --- Taskbar.View: прямой размер иконок через визуальное дерево ---
// Как в Windhawk-моде: кнопка -> IconPanel -> Icon, задаём Width/Height явно.
// Работает на UI-потоке (контекст хуков Update*), всё обёрнуто в try/catch.
static winrt::Windows::UI::Xaml::FrameworkElement FindChildByName(
    winrt::Windows::UI::Xaml::FrameworkElement parent, const wchar_t* name) {
    using namespace winrt::Windows::UI::Xaml::Media;
    int n = 0;
    try {
        n = VisualTreeHelper::GetChildrenCount(parent);
    } catch (...) {
        return nullptr;
    }
    for (int i = 0; i < n; i++) {
        winrt::Windows::UI::Xaml::FrameworkElement child = nullptr;
        try {
            child = VisualTreeHelper::GetChild(parent, i)
                        .try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
        } catch (...) {
            continue;
        }
        if (!child) continue;
        try {
            if (child.Name() == name) return child;
        } catch (...) {
            continue;
        }
    }
    return nullptr;
}

static void ResizeButtonIcon(void* buttonSelf) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    if (!ApplyingNow()) return;
    double want = (double)g_live.iconSize.load(std::memory_order_relaxed);
    if (n <= 12)
        LogLine(L"ResizeButtonIcon: button=%p want=%f", buttonSelf, want);
    try {
        winrt::Windows::UI::Xaml::FrameworkElement buttonElement = nullptr;
        IUnknown* inner = (IUnknown*)((uint8_t*)buttonSelf + 24);
        HRESULT hr = inner->QueryInterface(
            winrt::guid_of<winrt::Windows::UI::Xaml::FrameworkElement>(),
            winrt::put_abi(buttonElement));
        if (FAILED(hr) || !buttonElement) {
            LogLine(L"ResizeButtonIcon: QI failed hr=0x%lx", (unsigned long)hr);
            return;
        }
        auto iconPanel = FindChildByName(buttonElement, L"IconPanel");
        if (!iconPanel) {
            LogLine(L"ResizeButtonIcon: button %p has no IconPanel", buttonSelf);
            return;
        }
        // Диагностика: кто лежит внутри IconPanel.
        try {
            using namespace winrt::Windows::UI::Xaml::Media;
            int cn = VisualTreeHelper::GetChildrenCount(iconPanel);
            for (int ci = 0; ci < cn && ci < 8; ci++) {
                auto ch = VisualTreeHelper::GetChild(iconPanel, ci)
                              .try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
                if (!ch) continue;
                try {
                    LogLine(L"ResizeButtonIcon: child %d class=%s name=%s w=%f h=%f", ci,
                            winrt::get_class_name(ch).c_str(), ch.Name().c_str(),
                            ch.Width(), ch.Height());
                } catch (...) {
                }
            }
        } catch (...) {
        }
        auto iconEl = FindChildByName(iconPanel, L"Icon");
        if (!iconEl) {
            LogLine(L"ResizeButtonIcon: button %p has no Icon", buttonSelf);
            return;
        }
        iconEl.Width(want);
        iconEl.Height(want);
        LogLine(L"ResizeButtonIcon: button=%p icon set to %f", buttonSelf, want);
    } catch (...) {
        LogLine(L"ResizeButtonIcon: button %p threw, skip", buttonSelf);
    }
}

// Рекурсивный обход от кадра: ищем кнопки списка задач и ресайзим их иконки.
// Вызывается из UI-хуков (GetMetrics и др.), флаг снимается после применения.
static void ResizeSubtreeIcons(winrt::Windows::UI::Xaml::FrameworkElement parent) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Media;
    if (!g_layoutDirty.exchange(false, std::memory_order_relaxed)) return;
    double want = (double)g_live.iconSize.load(std::memory_order_relaxed);
    int n = 0;
    try {
        n = VisualTreeHelper::GetChildrenCount(parent);
    } catch (...) {
        return;
    }
    for (int i = 0; i < n; i++) {
        FrameworkElement child = nullptr;
        try {
            child = VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
        } catch (...) {
            continue;
        }
        if (!child) continue;
        bool isButton = false;
        try {
            isButton = (winrt::get_class_name(child) == L"Taskbar.TaskListButton");
        } catch (...) {
            continue;
        }
        if (isButton) {
            try {
                auto iconPanel = FindChildByName(child, L"IconPanel");
                if (iconPanel) {
                    auto iconEl = FindChildByName(iconPanel, L"Icon");
                    if (iconEl) {
                        iconEl.Width(want);
                        iconEl.Height(want);
                    }
                }
            } catch (...) {
            }
            continue;
        }
        ResizeSubtreeIcons(child);
    }
}

using UpdateVoidThis_t = void(*)(void* thisPtr);
static UpdateVoidThis_t UpdateButtonPadding_Orig = nullptr;
static UpdateVoidThis_t UpdateIconColumnDefinition_Orig = nullptr;
static UpdateVoidThis_t UpdateVisualStates_Orig = nullptr;

static void UpdateButtonPadding_Hook(void* self) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    if (n <= 6) LogLine(L"UpdateButtonPadding_Hook call=%ld self=%p", n, self);
    ResizeButtonIcon(self);
    UpdateButtonPadding_Orig(self);
}

static void UpdateIconColumnDefinition_Hook(void* self) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    if (n <= 6) LogLine(L"UpdateIconColumnDefinition_Hook call=%ld self=%p", n, self);
    ResizeButtonIcon(self);
    UpdateIconColumnDefinition_Orig(self);
}

static void UpdateVisualStates_Hook(void* self) {
    LONG n = InterlockedIncrement(&g_hookCalls);
    if (n <= 6) LogLine(L"UpdateVisualStates_Hook call=%ld self=%p", n, self);
    ResizeButtonIcon(self);
    UpdateVisualStates_Orig(self);
}

// ---------------------------------------------------------------- проверка образа
static bool IsRvaInText(const IMAGE_NT_HEADERS64* nt, DWORD rva) {
    auto secs = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(secs[i].Name, ".text", 5) != 0) continue;
        DWORD va = secs[i].VirtualAddress;
        return rva >= va && rva - va < secs[i].Misc.VirtualSize;
    }
    return false;
}

static bool VerifyModuleGuid(const uint8_t* base, const char* wantGuid, DWORD wantAge,
                             const char* tag) {
    auto dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LogLine(L"%hs: bad DOS signature", tag);
        return false;
    }
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        LogLine(L"%hs: bad NT signature", tag);
        return false;
    }
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY)) {
        LogLine(L"%hs: no debug directory", tag);
        return false;
    }
    auto entries = (const IMAGE_DEBUG_DIRECTORY*)(base + dir.VirtualAddress);
    size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < count; i++) {
        const IMAGE_DEBUG_DIRECTORY& e = entries[i];
        if (e.Type != IMAGE_DEBUG_TYPE_CODEVIEW || e.SizeOfData < 24 || e.AddressOfRawData == 0)
            continue;
        const uint8_t* p = base + e.AddressOfRawData;
        if (memcmp(p, "RSDS", 4) != 0) continue;

        const uint8_t* g = p + 4;
        uint32_t d1;
        uint16_t d2, d3;
        memcpy(&d1, g, 4);
        memcpy(&d2, g + 4, 2);
        memcpy(&d3, g + 6, 2);
        uint32_t age;
        memcpy(&age, p + 20, 4);

        char guid[33];
        int n = snprintf(guid, sizeof(guid), "%08lX%04X%04X", (unsigned long)d1, d2, d3);
        for (int b = 0; b < 8; b++) n += snprintf(guid + n, sizeof(guid) - n, "%02X", g[8 + b]);

        if (strcmp(guid, wantGuid) != 0 || age != wantAge) {
            LogLine(L"%hs GUID mismatch: guid=%hs age=%lu", tag, guid, (unsigned long)age);
            return false;
        }
        LogLine(L"%hs GUID ok: guid=%hs age=%lu", tag, guid, (unsigned long)age);
        return true;
    }
    LogLine(L"%hs: no RSDS record", tag);
    return false;
}

// Модуль по суффиксу пути (GetModuleHandle по имени путает наш taskbar.dll
// с системным C:\Windows\System32\Taskbar.dll).
static HMODULE ModuleByPathSuffix(const wchar_t* suffix) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
    DWORD count = needed / sizeof(HMODULE);
    if (count > 1024) count = 1024;
    wchar_t path[MAX_PATH];
    for (DWORD i = 0; i < count; i++) {
        if (!GetModuleFileNameExW(GetCurrentProcess(), mods[i], path, MAX_PATH)) continue;
        size_t lp = wcslen(path), ls = wcslen(suffix);
        if (lp < ls) continue;
        if (_wcsicmp(path + lp - ls, suffix) == 0) return mods[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------- пересчёт layout
static HWND FindMSTaskSwWClass(HWND hTaskbarWnd) {
    HWND hRebar = FindWindowExW(hTaskbarWnd, nullptr, L"ReBarWindow32", nullptr);
    if (!hRebar) return nullptr;
    return FindWindowExW(hRebar, nullptr, L"MSTaskSwWClass", nullptr);
}

static void RefreshTaskbarLayout() {
    if (IsVerticalTaskbarCached()) {
        LogLine(L"RefreshTaskbarLayout skipped: vertical taskbar");
        return;
    }
    HWND hTaskbarWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTaskbarWnd) {
        LogLine(L"RefreshTaskbarLayout: Shell_TrayWnd not found");
        return;
    }
    int target = g_live.height.load(std::memory_order_relaxed);
    LogLine(L"RefreshTaskbarLayout start hwnd=%p height=%d icon=%d btn=%d tray=%d", (void*)hTaskbarWnd,
            target, g_live.iconSize.load(), g_live.buttonWidth.load(), g_live.trayIconSize.load());

    g_live.height.store(target - 1, std::memory_order_relaxed);
    SendMessageW(hTaskbarWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);
    g_live.height.store(target, std::memory_order_relaxed);
    SendMessageW(hTaskbarWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);

    HWND hMSTaskSwWClass = FindMSTaskSwWClass(hTaskbarWnd);
    if (hMSTaskSwWClass) {
        SendMessageW(hMSTaskSwWClass, 0x452, 3, 0);
        LogLine(L"RefreshTaskbarLayout sent 0x452 to MSTaskSwWClass=%p", (void*)hMSTaskSwWClass);
    } else {
        LogLine(L"RefreshTaskbarLayout: MSTaskSwWClass not found");
    }
}

// Применить новый конфиг: значения + пересчёт. IsStorageRecreationRequired
// вернёт true, пока флаг взведён, — иконки пересоздадутся с новым размером.
void ApplyPanelConfig(const PanelConfig& cfg) {
    g_live.height.store(cfg.taskbarHeight, std::memory_order_relaxed);
    g_live.iconSize.store(cfg.iconSize, std::memory_order_relaxed);
    g_live.buttonWidth.store(cfg.taskbarButtonWidth, std::memory_order_relaxed);
    g_live.trayIconSize.store(cfg.trayIconSize, std::memory_order_relaxed);

    g_layoutDirty.store(true, std::memory_order_relaxed);
    RefreshTaskbarLayout();
}

// ---------------------------------------------------------------- установка хуков
static const uint8_t* g_viewBase = nullptr;
static const IMAGE_NT_HEADERS64* g_viewNt = nullptr;
static const uint8_t* g_taskbarBase = nullptr;
static const IMAGE_NT_HEADERS64* g_taskbarNt = nullptr;

// explorer.exe работает с включённым CFG: любой косвенный вызов проверяется
// по bitmap. Оригиналы MinHook живут в частной памяти, поэтому каждый трамплин
// маркируется через SetProcessValidCallTargets (ровно одна страница, n = 256;
// частичное покрытие и мультистраничные вызовы возвращают GLE=87/487).
// Образы (наш DLL, системные) уже валидны — их пропускаем.
typedef BOOL(WINAPI* SetValidTargets_t)(HANDLE, PVOID, SIZE_T, ULONG,
                                        PCFG_CALL_TARGET_INFO);

static bool MarkCfgValid(const char* name, void* addr) {
    static SetValidTargets_t fn = nullptr;
    static bool probed = false;
    if (!probed) {
        probed = true;
        fn = (SetValidTargets_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),
                                              "SetProcessValidCallTargets");
        if (!fn)
            fn = (SetValidTargets_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                   "SetProcessValidCallTargets");
    }
    if (!fn) {
        LogLine(L"MarkCfgValid %hs: API not available", name);
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE) return true;
    auto markPage = [&](uintptr_t page) -> bool {
        MEMORY_BASIC_INFORMATION q{};
        VirtualQuery((void*)page, &q, sizeof(q));
        if (q.State != MEM_COMMIT) {
            LogLine(L"markPage %hs page=%p not committed, skip", name, (void*)page);
            return true;
        }
        DWORD oldProt = 0;
        VirtualProtect((void*)page, 4096, PAGE_EXECUTE_READWRITE, &oldProt);
        CFG_CALL_TARGET_INFO info[256];
        for (ULONG i = 0; i < 256; i++) {
            info[i].Offset = (ULONG_PTR)(i * 16);
            info[i].Flags = CFG_CALL_TARGET_VALID;
        }
        if (!fn(GetCurrentProcess(), (PVOID)page, 4096, 256, info)) {
            LogLine(L"SetProcessValidCallTargets FAILED %hs page=%p GLE=%lu", name,
                    (void*)page, GetLastError());
            return false;
        }
        return true;
    };
    constexpr SIZE_T kTrampolineSpan = 96;
    uintptr_t p0 = (uintptr_t)addr & ~(uintptr_t)0xFFF;
    if (!markPage(p0)) return false;
    if ((((uintptr_t)addr + kTrampolineSpan) & ~(uintptr_t)0xFFF) != p0) {
        if (!markPage(p0 + 4096)) return false;
    }
    return true;
}

static bool InstallOne(const IMAGE_NT_HEADERS64* nt, const uint8_t* base, const char* name,
                       DWORD rva, void* detour, void** orig) {
    if (!IsRvaInText(nt, rva)) {
        LogLine(L"skip %hs: rva not in .text", name);
        return false;
    }
    void* target = (void*)(base + rva);
    // Создаём хук БЕЗ включения: цель не патчится, гонки нет.
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    if (st != MH_OK) {
        LogLine(L"MH_CreateHook FAILED %hs rva=0x%lx st=%d", name, (unsigned long)rva, (int)st);
        return false;
    }
    // Помечаем детур и трамплин валидными для CFG ДО первого вызова.
    if (!MarkCfgValid(name, detour) || !MarkCfgValid(name, *orig)) {
        MH_RemoveHook(target); // цель не была затронута — безопасно
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        LogLine(L"MH_EnableHook FAILED %hs rva=0x%lx st=%d", name, (unsigned long)rva, (int)st);
        MH_RemoveHook(target);
        return false;
    }
    LogLine(L"hook ON %hs rva=0x%lx target=%p", name, (unsigned long)rva, target);
    return true;
}

bool InstallHooks(HMODULE /*unused*/, const PanelConfig& cfg) {
    HMODULE viewMod = GetModuleHandleW(L"Taskbar.View.dll");
    if (!viewMod) {
        LogLine(L"InstallHooks: Taskbar.View.dll not loaded");
        return false;
    }
    g_viewBase = (const uint8_t*)viewMod;
    if (!VerifyModuleGuid(g_viewBase, kViewGuid, kViewAge, "Taskbar.View.dll")) {
        LogLine(L"InstallHooks refused: Taskbar.View.dll PDB GUID/age mismatch");
        return false;
    }
    auto dos = (const IMAGE_DOS_HEADER*)g_viewBase;
    g_viewNt = (const IMAGE_NT_HEADERS64*)(g_viewBase + dos->e_lfanew);

    HMODULE taskbarMod = ModuleByPathSuffix(L"\\System32\\Taskbar.dll");
    bool haveTaskbar = false;
    if (taskbarMod) {
        g_taskbarBase = (const uint8_t*)taskbarMod;
        if (VerifyModuleGuid(g_taskbarBase, kTaskbarGuid, kTaskbarAge, "Taskbar.dll")) {
            auto d2 = (const IMAGE_DOS_HEADER*)g_taskbarBase;
            g_taskbarNt = (const IMAGE_NT_HEADERS64*)(g_taskbarBase + d2->e_lfanew);
            haveTaskbar = true;
        } else {
            LogLine(L"InstallHooks: Taskbar.dll GUID mismatch, icon hooks off");
        }
    } else {
        LogLine(L"InstallHooks: System32 Taskbar.dll not loaded, icon hooks off");
    }

    if (MH_Initialize() != MH_OK) {
        LogLine(L"MH_Initialize FAILED");
        return false;
    }
    LogLine(L"MH_Initialize ok");

    bool ok = false;
    if (haveTaskbar) {
        ok |= InstallOne(g_taskbarNt, g_taskbarBase, "IconUtils_GetIconSize", kRvaGetIconSize,
                         (void*)GetIconSize_Hook, (void**)&GetIconSize_Orig);
        ok |= InstallOne(g_taskbarNt, g_taskbarBase, "IsStorageRecreationRequired",
                         kRvaIsStorageRecreationRequired,
                         (void*)IsStorageRecreationRequired_Hook,
                         (void**)&IsStorageRecreationRequired_Orig);
    }
    ok |= InstallOne(g_viewNt, g_viewBase, "GetFrameSize", kRvaGetFrameSize,
                     (void*)GetFrameSize_Hook, (void**)&GetFrameSize_Orig);
    ok |= InstallOne(g_viewNt, g_viewBase, "GetTrayFrameSize", kRvaTrayGetFrameSize,
                     (void*)GetTrayFrameSize_Hook, (void**)&GetTrayFrameSize_Orig);
    ok |= InstallOne(g_viewNt, g_viewBase, "GetIconHeightInViewPixels", kRvaGetIconHeight,
                     (void*)GetIconHeight_Hook, (void**)&GetIconHeight_Orig);
    ok |= InstallOne(g_viewNt, g_viewBase, "GetIconHeightMethod", kRvaGetIconHeightMethod,
                     (void*)GetIconHeightMethod_Hook, (void**)&GetIconHeightMethod_Orig);
    ok |= InstallOne(g_viewNt, g_viewBase, "ResourceLookup", kRvaLookup,
                     (void*)Lookup_Hook, (void**)&Lookup_Orig);
    ok |= InstallOne(g_viewNt, g_viewBase, "GetMetrics", kRvaGetMetrics,
                     (void*)GetMetrics_Hook, (void**)&GetMetrics_Orig);
    ok |= InstallOne(g_viewNt, g_viewBase, "UpdateButtonPadding", kRvaUpdateButtonPadding,
                     (void*)UpdateButtonPadding_Hook,
                     (void**)&UpdateButtonPadding_Orig);
    ok |= InstallOne(g_viewNt, g_viewBase, "UpdateIconColumnDefinition",
                     kRvaUpdateIconColumnDefinition, (void*)UpdateIconColumnDefinition_Hook,
                     (void**)&UpdateIconColumnDefinition_Orig);
    ok |= InstallOne(g_viewNt, g_viewBase, "UpdateVisualStates",
                     kRvaUpdateVisualStates, (void*)UpdateVisualStates_Hook,
                     (void**)&UpdateVisualStates_Orig);

    g_live.height.store(cfg.taskbarHeight, std::memory_order_relaxed);
    g_live.iconSize.store(cfg.iconSize, std::memory_order_relaxed);
    g_live.buttonWidth.store(cfg.taskbarButtonWidth, std::memory_order_relaxed);
    g_live.trayIconSize.store(cfg.trayIconSize, std::memory_order_relaxed);

    RefreshTaskbarLayout();
    return ok;
}

void RemoveHooks() {
    struct Item {
        const IMAGE_NT_HEADERS64* nt;
        const uint8_t* base;
        DWORD rva;
        const char* name;
    };
    const Item items[] = {
        {g_taskbarNt, g_taskbarBase, kRvaGetIconSize, "IconUtils_GetIconSize"},
        {g_taskbarNt, g_taskbarBase, kRvaIsStorageRecreationRequired,
         "IsStorageRecreationRequired"},
        {g_viewNt, g_viewBase, kRvaGetFrameSize, "GetFrameSize"},
        {g_viewNt, g_viewBase, kRvaTrayGetFrameSize, "GetTrayFrameSize"},
        {g_viewNt, g_viewBase, kRvaGetIconHeight, "GetIconHeightInViewPixels"},
        {g_viewNt, g_viewBase, kRvaGetIconHeightMethod, "GetIconHeightMethod"},
        {g_viewNt, g_viewBase, kRvaLookup, "ResourceLookup"},
        {g_viewNt, g_viewBase, kRvaGetMetrics, "GetMetrics"},
        {g_viewNt, g_viewBase, kRvaUpdateButtonPadding, "UpdateButtonPadding"},
        {g_viewNt, g_viewBase, kRvaUpdateIconColumnDefinition, "UpdateIconColumnDefinition"},
        {g_viewNt, g_viewBase, kRvaUpdateVisualStates, "UpdateVisualStates"},
    };
    for (const Item& it : items) {
        if (!it.nt || !it.base) continue;
        void* target = (void*)(it.base + it.rva);
        if (MH_DisableHook(target) == MH_ERROR_NOT_INITIALIZED) continue;
        MH_RemoveHook(target);
        LogLine(L"hook OFF %hs rva=0x%lx", it.name, (unsigned long)it.rva);
    }
    MH_Uninitialize();
    g_viewBase = nullptr;
    g_viewNt = nullptr;
    g_taskbarBase = nullptr;
    g_taskbarNt = nullptr;
    LogLine(L"RemoveHooks done");
}

void* SpanelTrampolineForProbe() {
    return (void*)GetFrameSize_Orig;
}
