# s-panel — что осталось доделать (22.10.2026, день)

## Статус: работает и проверено (A/B скриншот + pixel-diff, tools/shot_diff.ps1)
| Фича | Как | Статус |
|---|---|---|
| Высота панели | хук `GetFrameSize` @0x7F4B0 | ✅ РАБОТАЕТ |
| Размер иконок панели | `GetIconHeightInViewPixels` @0x1CA628 + @0x280BD4, `IconUtils::GetIconSize` @0x2D0CC, прямой Width/Height у Image «Icon» | ✅ РАБОТАЕТ (16/24/48) |
| Ширина кнопок | `Lookup` @0x98970 (Medium/SmallTaskbarButtonExtent) + `GetMetrics` @0x37F3D8 | ✅ РАБОТАЕТ |
| Конфиг на лету | вотчер 250мс, сравнение значений | ✅ РАБОТАЕТ |
| Попап на 6 полей | в `loader.exe --tray` / `--inject-and-tray` | ✅ РАБОТАЕТ |
| Откат | `loader --uninstall` (SpanelUninstall → FreeLibrary) | ✅ РАБОТАЕТ |
| Тесты | test_config, test_hooktargets, run_tests.bat | ✅ PASS |

ВНИМАНИЕ про иконки: тестировать на высоте 48. На высоте 34 панель ОБРЕЗАЕТ иконки — вчера на этом
built ложный вывод «иконки не работают».

## Осталось доделать
### 1. Размер иконок трея (главное, в работе)
Механизм (эталон-мод taskbar-notification-icon-spacing v1.4): не сеттер размера, а
`NotifyItemView.MinWidth(ширина)` + обнуление `Padding` у «ContainerGrid» контента.
- [x] хук `IconView::IconView(IconConfiguration*)` @0x484A8 (Taskbar.View.dll) — для НОВЫХ иконок
- [x] захват XamlRoot панели (из TaskbarFrame в хуке `GetMetrics`)
- [x] разобрана структура дерева трея (см. ниже)
- [ ] дойти до элементов иконок: контейнер — ContentControl, его дети не видны через
      VisualTreeHelper; содержимое берём через `IContentControl::get_Content` (через ABI,
      обход C3779) — **правка сделана, но НЕ проверена сборкой/рантаймом**
- [ ] применять стиль: MinWidth = trayIconSize + traySpacing; Padding=0 у контента
- [ ] переприменение при смене значения (уже есть примитив RunOnTaskbarUiThread)

Структура дерева трея (найдена дампом, SystemTrayFrameGrid → …):
```
SystemTray.Stack name=NotifyIconStack        (и MainStack, NonActivatableStack, SecondaryClockStack)
  → Grid name=Content
    → SystemTray.StackListView name=IconStack
      → ItemsPresenter
        → ContentControl          ← контейнер иконки, дети не видны (нужен Content)
          → StackPanel
            → ContentControl (на каждый элемент)
SystemTray.OmniButton name=ControlCenterButton → Grid → Border "BackgroundBorder"
  → ContentPresenter → ItemsPresenter → ContentControl → StackPanel → ContentControl
    → ContentPresenter → SystemTray.IconView name=SystemTrayIcon
```
Имя пер-иконки в эталоне — «NotifyItemIcon» (класс SystemTray.NotifyIconView), у нас в дереве
пока не найдено — искать в поддереве контейнера.

### 2. Отступы
- Панель: уже реализовано как добавка к ширине кнопки (TaskbarButtonExtent = buttonWidth + taskbarSpacing)
- Трей: MinWidth = trayIconSize + traySpacing (вместе с п.1)

### 3. Тест-покрытие
- Есть: tools/shot.ps1 (снимок), tools/shot_diff.ps1 (diff + bbox)
- Нужен авто-сценарий: значение → скриншот → diff → assert по порогу; вшить в run_tests.bat

### 4. Чистка перед релизом
- Удалить: SpanelCfgProbe, SpanelDummyTarget, tools/cfgmark.cpp, tools/mdstack.cpp, MAP-файл из CMakeLists
- Обновить tests/test_hooktargets.cpp под финальный набор RVA
- README + автозапуск (schtasks; без «да» пользователя не создавать)

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

## Координаты
- Эталон-моды: taskbar-icon-size → `C:\Users\sfx\.local\share\opencode\tool-output\tool_16095f3620015V9g4Pw15l24d2`
  taskbar-notification-icon-spacing → `C:\Users\sfx\.local\share\opencode\tool-output\tool_1633122a70012ZlNf5dAzBM7ZD`
- PDB: `C:\Users\sfx\AppData\Local\Temp\opencode\symbols\{Taskbar,Taskbar.View}.pdb\...`
- RVA Taskbar.dll (GUID 1C6067D3BC7AABA7CDE214B3AE66C6D3 age 1): IconUtils_GetIconSize 0x2D0CC,
  IsStorageRecreationRequired 0x914D0
- RVA Taskbar.View.dll (GUID 0AE0E302E8E34BE29CC303A7A62ABCA7 age 1): GetFrameSize 0x7F4B0,
  GetIconHeight 0x1CA628, GetIconHeightMethod 0x280BD4, GetMetrics 0x37F3D8, Lookup 0x98970,
  TrayGetFrameSize 0x26CE2C, UpdateButtonPadding 0x376AFC, UpdateIconColumnDefinition 0x376DF4,
  UpdateVisualStates 0x13E54, IconView::IconView 0x484A8
