# s-panel — что доделать (статус на 22.10.2026, ночь)

## Что работает (проверено)
- Высота панели (GetFrameSize @0x7F4B0). Меняется на лету через config.ini / попап.
- Ширина кнопок (Lookup Medium/SmallTaskbarButtonExtent + GetMetrics).
- Трей-иконка s-panel + попап на 6 полей — ЖИВЁТ В loader.exe (`--tray`, `--inject-and-tray`), НЕ в explorer (в explorer трей падал 3 раза с fastfail в C++ unwind — дампы в Temp\opencode\dumps).
- Конфиг на лету (вотчер 250мс, сравнение значений), Reset → заводские 48/24/44/24.
- Откат `--uninstall` через SpanelUninstall, защита от двойного инжекта.
- Тесты: test_config, test_hooktargets (9 RVA + GUID/age), run_tests.bat — все PASS.
- CFG: трамплины MinHook маркируются через SetProcessValidCallTargets (страница целиком, n=256).
- Explorer НЕ падает со вчерашнего вечера (последний краш 01:17, причина найдена и исправлена — вызов сеттеров с чужого потока).

## Что НЕ работает
1. **Размер иконок панели (главное).** Хуки стоят, значения пишутся (readback подтверждает 48.0 в поле), Image-элементы получают Width/Height (лог подтверждает 48×48), bitmap масштабируется в IconUtils_GetIconSize (лог подтверждает). Но пиксели на экране НЕ меняются. Подозрения: Stretch=None в шаблоне, не тот элемент рендерится (DefaultIcon?), layout не перезапускается после установки. Следующий шаг: в walk добавить `InvalidateMeasure()` на кнопку после установки размеров; проверить `Stretch` у найденного Image; сравнить побайтово скриншоты до/после.
2. **Иконки трея (размер).** Хуки put_GlyphFontSize стоят, но ни разу не сработали — путь обновления трея не найден. Отдельное исследование (возможно, как в моде «Taskbar tray icon spacing and grid»).
3. **Отступы между иконками (оба).** Поля есть в конфиге и UI, не применяются. Gap задаётся XAML-привязками StackLayout::Spacing — нужен рантайм-замер.
4. **Ширина кнопок** — применяется (Lookup+GetMetrics в логе), но визуально НЕ сверена. Нужен A/B скриншот.

## Техдолг
- `SpanelCfgProbe` / `SpanelDummyTarget` / `cfgmark` / `mdstack` / `taskview.asm` / `ntdll.asm` — отладочный мусор, удалить перед релизом (и MAP-файл из CMake).
- `test_hooktargets` обновить под финальный набор RVA (сейчас там старый список из 9).
- Включить LocalDumps для explorer сейчас (ключ реестра) — оставить до стабилизации.
- Автозапуск: `loader --install [--with-autostart]` готов, но задача НЕ создавалась (по требованию). Создать, когда всё заработает.
- Коммит: последние изменения (v3 хуки, переезд трея, Lookup) НЕ закоммичены.

## Координаты для продолжения
- Эталон: исходник Windhawk-мода taskbar-icon-size v1.3.10 — `C:\Users\sfx\.local\share\opencode\tool-output\tool_16095f3620015V9g4Pw15l24d2` (полный текст, 3274 строки).
- PDB: `C:\Users\sfx\AppData\Local\Temp\opencode\symbols\Taskbar.View.pdb\...\Taskbar.View.pdb`, `...\Taskbar.pdb\...\Taskbar.pdb`.
- RVA Taskbar.dll: IconUtils_GetIconSize 0x2D0CC, IsStorageRecreationRequired 0x914D0 (GUID 1C6067D3BC7AABA7CDE214B3AE66C6D3 age 1).
- RVA Taskbar.View: GetFrameSize 0x7F4B0, GetIconHeight 0x1CA628, GetIconHeightMethod 0x280BD4, GetMetrics 0x37F3D8, Lookup 0x98970, TrayGetFrameSize 0x26CE2C, UpdateButtonPadding 0x376AFC, UpdateIconColumnDefinition 0x376DF4, UpdateVisualStates 0x13E54 (GUID 0AE0E302... age 1).
- Ключевая находка: put_IconHeight пишет [impl+0x78] == [produce+0x60] (одно поле), notify срабатывает, но рендер его игнорирует. Рабочий путь мода — IconUtils (bitmap) + прямая установка Width/Height у Icon-элемента.
