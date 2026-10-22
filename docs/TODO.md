# s-panel — статус (22.10.2026)

## Статус: всё работает и проверено (A/B скриншот + pixel-diff, tools/shot_diff.ps1, tools/auto_test.ps1)

| Фича | Как | Статус |
|---|---|---|
| Высота панели | хук `GetFrameSize` @0x7F4B0 | ✅ РАБОТАЕТ |
| Размер иконок панели | `GetIconHeightInViewPixels` @0x1CA628 + @0x280BD4, `IconUtils::GetIconSize` @0x2D0CC, прямой Width/Height у Image «Icon» | ✅ РАБОТАЕТ (16/24/48) |
| Ширина кнопок | `Lookup` @0x98970 (Medium/SmallTaskbarButtonExtent) + `GetMetrics` @0x37F3D8 | ✅ РАБОТАЕТ |
| Размер иконок трея | обход дерева XAML → `NotifyIconView.MinWidth` + Padding=0 у «ContainerGrid» | ✅ РАБОТАЕТ (applied=6 иконок) |
| Отступы | панель: +к ширине кнопки; трей: +к MinWidth ячейки | ✅ РАБОТАЕТ |
| Конфиг на лету | вотчер 250 мс, сравнение значений | ✅ РАБОТАЕТ |
| Попап на 6 полей | в `loader.exe --tray` / `--inject-and-tray` | ✅ РАБОТАЕТ |
| Откат | `loader --uninstall` (SpanelUninstall → FreeLibrary) | ✅ РАБОТАЕТ |
| Тесты | test_config, test_host, test_resolve, test_hooktargets, run_tests.bat | ✅ PASS |
| Живой тест трея | tools/auto_test.ps1 (значение → скриншот → измерение шага → assert) | ✅ PASS (шаг 268→304 px) |

ВНИМАНИЕ про иконки: тестировать на высоте 48. На высоте 34 панель ОБРЕЗАЕТ иконки — на этом
built ложный вывод «иконки не работают».

## Сделано 22.10.2026 (закрытие пунктов 1–4)

### 1. Размер иконок трея — ✅
Механизм (эталон-мод taskbar-notification-icon-spacing v1.4): не сеттер размера, а
`NotifyIconView.MinWidth(ширина)` + обнуление `Padding` у «ContainerGrid» контента.
- [x] обход дерева XAML от корня острова: `XamlRoot.Content` → `SystemTray.SystemTrayFrame`
      → `SystemTrayFrameGrid` → `NotificationAreaIcons` → `SystemTray.NotifyIconView`
- [x] содержимое `ContentControl` через ABI `IContentControl::get_Content` (C3779 обойдён)
- [x] `Padding` через ABI `IGrid2::put_Padding`
- [x] стиль: `MinWidth = trayIconSize + traySpacing`, Padding=0 у «ContainerGrid»
- [x] переприменение: раз в ~4 с (оболочка сбрасывает MinWidth при пересборке трея)
- [x] захват корней XAML: ВСЕ панели задач (в мультимониторной системе их несколько),
      корень появляется только после пересчёта метрик → высота на 1 px уменьшается
      и возвращается (заставляет оболочку пересчитать метрики)
- [x] хуки конструкторов иконки: `IconView::IconView` @0xB6A50, `NotifyIconView::NotifyIconView`
      @0x2DE748 — в 24H2 не вызываются (оболочка переиспользует элементы), рабочий путь —
      обход дерева; хуки оставлены для других сборок

### 2. Отступы — ✅
- Панель: `TaskbarButtonExtent = buttonWidth + taskbarSpacing`
- Трей: `MinWidth = trayIconSize + traySpacing`

### 3. Тест-покрытие — ✅
- `tools/shot.ps1` (снимок), `tools/shot_diff.ps1` (diff + bbox) — оставлены
- `tools/auto_test.ps1` — живой сценарий: меняет `TrayIconSize`, снимает область
  уведомлений до/после, измеряет расстояние между иконками, assert на прирост;
  конфиг восстанавливается в `finally`
- вшит в `tests\run_tests.bat --live`

### 4. Чистка — ✅
- Удалены `SpanelCfgProbe`, `SpanelDummyTarget`, `tools/probe.cpp`, `tools/cfgmark.cpp`,
  `tools/mdstack.cpp`, режим `loader --probe`, MAP-файл из CMakeLists
- `tests/test_hooktargets.cpp` — 12 актуальных целей из hooks.cpp
- README переписан, добавлен раздел про автозапуск (`loader --install --with-autostart`)

## Грабли (не повторять)
- CFG: вызов оригинала MinHook = fastfail 0xC0000409; трамплины маркируются
  SetProcessValidCallTargets — РОВНО одна страница, n=256 (частично → GLE=87, некоммиченная → 487)
- Сеттеры XAML нельзя звать с чужого потока (RPC_E_WRONG_THREAD → terminate → краш explorer);
  только UI-поток + try/catch
- Трей в explorer ронял оболочку (fastfail в C++ unwind) → трей живёт в loader.exe
- Модули искать по полному пути (taskbar.dll ≠ C:\Windows\System32\Taskbar.dll)
- C++/WinRT: consume<> (Grid.Padding, ContentControl.Content) → C3779, обходить через ABI
  (winrt::impl::abi<T>::type) или через IGrid2/IContentControl напрямую
- Explorer в этой сессии несколько раз перезапускался сам; `--uninstall` один раз упал с GLE=5
  (модуль успел выгрузиться/сменился PID) — переинжект после рестарта explorer обязателен
- RVA хука конструктора IconView из TODO (0x484A8) был неверным — реальный 0xB6A50
  (сверять с PDB через tools/pdbrva.exe)
- В 24H2 область уведомлений в дереве XAML панели пустая: иконки лежат в другом
  острове XAML (вторая панель задач) — снимать все корни, а не первый попавшийся
- .ps1 с русским текстом без BOM ломает парсер PowerShell 5.1 (CP1251: 0x94 = ”)

## Координаты
- Эталон-моды: taskbar-icon-size → `C:\Users\sfx\.local\share\opencode\tool-output\tool_16095f3620015V9g4Pw15l24d2`
  taskbar-notification-icon-spacing → `C:\Users\sfx\.local\share\opencode\tool-output\tool_1633122a70012ZlNf5dAzBM7ZD`
- PDB: `C:\Users\sfx\AppData\Local\Temp\opencode\symbols\{Taskbar,Taskbar.View}.pdb\...`
- RVA Taskbar.dll (GUID 1C6067D3BC7AABA7CDE214B3AE66C6D3 age 1): IconUtils_GetIconSize 0x2D0CC,
  IsStorageRecreationRequired 0x914D0
- RVA Taskbar.View.dll (GUID 0AE0E302E8E34BE29CC303A7A62ABCA7 age 1): GetFrameSize 0x7F4B0,
  GetIconHeight 0x1CA628, GetIconHeightMethod 0x280BD4, GetMetrics 0x37F3D8, Lookup 0x98970,
  TrayGetFrameSize 0x26CE2C, UpdateButtonPadding 0x376AFC, UpdateIconColumnDefinition 0x376DF4,
  UpdateVisualStates 0x13E54, IconView::IconView 0xB6A50, NotifyIconView::NotifyIconView 0x2DE748

## Что можно улучшить дальше (не обязательно)
- Измерить потребление CPU/памяти в explorer
- Перехватить реальный размер глифа иконки трея (сейчас меняется ширина ячейки/шаг,
  сам глиф 16 px задаётся оболочкой)
- Поддержка вертикальной панели задач для остальных параметров
