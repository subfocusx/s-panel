// trayui.h — иконка в трее + всплывающая панель настроек (6 полей).
// Живёт внутри explorer.exe, в выделенном потоке с собственным циклом сообщений.
#pragma once
#include <windows.h>
#include "../shared/config.h"

// Колбэк применения: вызывается из потока трея после изменения значения.
using TrayApplyFn = void(__cdecl*)(const PanelConfig& cfg);

// Поднимает иконку в трее и всплывающую панель настроек.
// cfgPath — путь к config.ini для записи (если пустой — рядом с DLL).
// applyFn — вызывается после каждого изменения конфига (может быть nullptr).
// Возвращает true, если UI поднялся.
bool TrayUiStart(const wchar_t* cfgPath, TrayApplyFn applyFn);

// Гасит иконку, останавливает поток, уничтожает окна. Идемпотентно.
void TrayUiStop();

// Обновить кэшированный конфиг после внешнего изменения (файл правил вручную).
// Не пишет на диск и не вызывает applyFn — только обновляет поля в UI.
void TrayUiNoteExternalChange(const PanelConfig& cfg);
