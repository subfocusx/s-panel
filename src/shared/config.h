#pragma once
#include <string>

// Индексы полей панели — общий порядок для UI, конфига и хуков.
enum PanelField {
    kFieldHeight = 0,      // TaskbarHeight
    kFieldIconSize = 1,    // IconSize
    kFieldButtonWidth = 2, // TaskbarButtonWidth
    kFieldTrayIconSize = 3,// TrayIconSize
    kFieldTaskbarSpacing = 4, // TaskbarIconSpacing (доп. отступ между иконками панели)
    kFieldTraySpacing = 5,    // TrayIconSpacing    (доп. отступ между иконками трея)
    kFieldCount = 6
};

struct PanelConfig {
    int taskbarHeight = 34;      // default Win11: 48
    int iconSize = 16;           // default Win11: 24
    int taskbarButtonWidth = 28; // default Win11: 44
    int trayIconSize = 32;       // ширина ячейки иконки трея, default Win11: 32
    int taskbarIconSpacing = 0;  // доп. отступ между иконками панели, px
    int trayIconSpacing = 0;     // доп. отступ между иконками трея, px
};

// iniPath may be nullptr -> "<dll-or-exe-dir>\\config.ini"
bool ReadPanelConfig(const wchar_t* iniPath, PanelConfig* out);
std::wstring DefaultIniPath(void* hModuleOrNull);

// Заводские значения Windows 11 (48/24/44/32, отступы 0) — цель кнопки Reset.
PanelConfig WindowsDefaults();

// Ключ ini для поля (nullptr при неверном индексе).
const wchar_t* PanelFieldKey(int field);

// Значение поля по индексу (для UI).
int PanelFieldValue(const PanelConfig& cfg, int field);
void PanelFieldSet(PanelConfig* cfg, int field, int value);

// Границы значений поля (min/max). Учитывают «родные» значения Windows.
void PanelFieldRange(int field, int* minValue, int* maxValue);

// Записать конфиг в ini. Пишет атомарно: временный файл + MoveFileEx.
// При неудаче записи по iniPath пробует %LOCALAPPDATA%\\s-panel\\config.ini.
// Возвращает фактический путь записи (пустой — запись не удалась).
std::wstring WritePanelConfig(const wchar_t* iniPath, const PanelConfig& cfg);

// Путь к конфигу пользователя: %LOCALAPPDATA%\\s-panel\\config.ini
std::wstring UserConfigPath();
