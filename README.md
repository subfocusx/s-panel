# s-panel — компактная панель задач Windows 11

Производная от Windhawk-модов `taskbar-icon-size` и `taskbar-notification-icon-spacing`
(m417z, GPLv3). Лицензия проекта — **GPLv3**, см. [LICENSE](LICENSE).
MinHook в `vendor/` — BSD-2-Clause (`vendor/minhook/LICENSE.txt`).

## Что делает

Позволяет менять шесть параметров панели задач и трея без перезапуска explorer:

| Поле | Ключ ini | Смысл | Значение Win11 |
|---|---|---|---|
| Taskbar height | `TaskbarHeight` | высота панели задач, px | 48 |
| Icon size | `IconSize` | размер иконок приложений, px | 24 |
| Button width | `TaskbarButtonWidth` | ширина кнопки приложения, px | 44 |
| Tray icons | `TrayIconSize` | ширина ячейки иконки трея, px | 32 |
| Taskbar spacing | `TaskbarIconSpacing` | доп. отступ между иконками панели, px | 0 |
| Tray spacing | `TrayIconSpacing` | доп. отступ между иконками трея, px | 0 |

Правки применяются на лету: DLL следит за `config.ini` и пересобирает панель без
перезапуска оболочки. Вертикальная панель (слева/справа) не трогается.
Только x64, только сборка Windows 11 24H2 (10.0.26100).

### Как устроен трей

Оболочка рисует область уведомлений в XAML-острове панели. Мы идём от корня
острова (`XamlRoot.Content`) по дереву:

```
XamlRoot.Content
  └─ SystemTray.SystemTrayFrame
       └─ SystemTrayFrameGrid
            └─ NotificationAreaIcons        (ItemsControl)
                 └─ ItemsPresenter → StackPanel → … → SystemTray.NotifyIconView
```

Элементу иконки (`SystemTray.NotifyIconView`) задаётся `MinWidth` — это ширина
ячейки; она же определяет расстояние между иконками трея. Внутренний `Padding`
контента обнуляется, чтобы иконка занимала всю ячейку.

Особенности сборки 24H2, на которые натыкается обход:

- дети `ContentControl` не видны через `VisualTreeHelper` — содержимое берётся
  через ABI `IContentControl::get_Content` (обычный `consume<>` даёт C3779);
- `Padding` у `Grid` ставится через ABI `IGrid2::put_Padding` (по той же причине);
- в мультимониторной системе панелей задач несколько — собираются **все** корни
  XAML, стиль применяется в каждом;
- оболочка задаёт высоту рамки лотка (`SystemTray.SystemTrayFrame`) только основной
  панели; у вторичных панелей она остаётся заводной (48 px), и блок часов
  центрируется по ней — из-за этого на разных мониторах часы стояли на разной
  высоте (на вторичной ниже на `(48 - H)/2` px). Высота рамки выравнивается по
  нашей во всех корнях; повторяется ещё пару секунд после смены высоты, потому
  что оболочка перекладывает остров XAML с задержкой в кадр;
- корни XAML появляются не сразу: оболочка пересчитывает метрики панели только
  при изменении высоты, поэтому при смене настроек высота на 1 px уменьшается и
  возвращается обратно — это заставляет оболочку пересчитать метрики и отдать
  нам корень дерева;
- оболочка периодически пересобирает область уведомлений и сбрасывает `MinWidth`,
  поэтому стиль переприменяется раз в ~4 с.

## Хуки

Все цели — в `Taskbar.View.dll` (сборка 26100.8457), список проверяется
`tests\test_hooktargets.exe` (попадание RVA в `.text` + GUID/age PDB):

| Символ | RVA | Назначение |
|---|---|---|
| `TaskbarConfiguration::GetFrameSize` | 0x7F4B0 | высота панели |
| `TaskbarConfiguration::GetTrayFrameSize` | 0x26CE2C | размер области трея |
| `TaskbarConfiguration::GetIconHeightInViewPixels` | 0x1CA628 | размер иконок |
| `TaskbarConfiguration::GetIconHeightMethod` | 0x280BD4 | размер иконок (запасной путь) |
| `ResourceLookup` | 0x98970 | ширина кнопки (`SmallTaskbarButtonExtent`) |
| `TaskbarFrame::GetMetrics` | 0x37F3D8 | ширина кнопки + доступ к дереву XAML |
| `TaskListButton::UpdateButtonPadding` | 0x376AFC | ресайз иконок панели |
| `TaskListButton::UpdateIconColumnDefinition` | 0x376DF4 | ресайз иконок панели |
| `TaskListButton::UpdateVisualStates` | 0x13E54 | ресайз иконок панели |
| `IconView::IconView` | 0xB6A50 | конструктор иконки трея |
| `NotifyIconView::NotifyIconView` | 0x2DE748 | конструктор иконки трея |

Перед установкой проверяется GUID/age PDB (`Taskbar.View.pdb`
`0AE0E302E8E34BE29CC303A7A62ABCA7/1`). Несовпадение = хуки не ставятся,
инжект остаётся no-op.

Хуки конструкторов иконок трея в 24H2 не вызываются (оболочка переиспользует
элементы), поэтому рабочий путь — обход дерева XAML; хуки оставлены для других
сборок.

## Как устроено

- `taskbar.dll` (MinHook, `/MT`) загружается **внутрь `explorer.exe`**.
- `loader.exe` инжектит DLL через `CreateRemoteThread + LoadLibraryW` в
  `explorer.exe` и завершается.
- Иконка и всплывающая панель настроек живут в `loader.exe` (`--tray`), а не в
  оболочке: окна и GDI в процессе explorer периодически роняли её.
- Связь панели настроек с DLL — через `config.ini`: DLL следит за файлом.
- Конфиг `config.ini` лежит рядом с DLL, шаблон — `config.example.ini`.

## Сборка

Нужны Visual Studio 2022 Build Tools (Desktop C++), Windows 11 SDK 26100, CMake, Ninja.

```
tools\check_env.bat
build.bat [--clean]
```

Результат — `build\bin\taskbar.dll`, `build\bin\loader.exe`.

## Тесты

```
tests\run_tests.bat            # сухие тесты, без обращения к explorer
tests\run_tests.bat --live     # + живой тест трея (нужна загруженная DLL)
```

Сухие: `test_config`, `test_host`, `test_resolve`, `test_hooktargets`,
`loader.exe --check` (только чтение).

Живой тест (`tools\auto_test.ps1`): меняет `TrayIconSize` в конфиге, снимает
область уведомлений до и после и проверяет, что расстояние между иконками
выросло; конфиг восстанавливается в любом случае.

Команды `loader.exe`: `--check`, `--inject`, `--uninstall`, `--tray`,
`--inject-and-tray`, `--install [--with-autostart]`, `--uninstall-task`.
`--inject` не инжектит дважды; `--uninstall` сначала вызывает экспорт
`SpanelUninstall` (снять хуки), затем выгружает DLL циклом `FreeLibrary`.

### Запуск по двойному клику

```
powershell -ExecutionPolicy Bypass -File tools\install_shortcut.ps1
```

Скрипт раскладывает `loader.exe`/`taskbar.dll` в `%LOCALAPPDATA%\s-panel`,
создаёт `start-s-panel.vbs` и ярлык **s-panel** на рабочем столе. Двойной клик
по ярлыку (без окна консоли) внедряет DLL в explorer, поднимает иконку в трее
и сразу открывает панель настроек. Повторный клик, если s-panel уже запущен,
просто поднимает панель — второй иконки не появляется.

Где живёт иконка: в Windows 11 24H2 Win32-иконки (`Shell_NotifyIcon`) всегда
попадают в меню скрытых иконок (шеврон у часов), а не в видимый ряд панели —
в видимый ряд выводятся только системные XAML-иконки. Поэтому иконка s-panel
видна по клику на шеврон, а панель открывается кликом по самой иконке.

### Автозапуск при входе в систему

```
loader.exe --install --with-autostart
```

Создаёт задачу планировщика `s-panel` (запуск при входе в систему).
Удаление: `loader.exe --uninstall-task`. Задача намеренно не создаётся
автоматически — только по явной команде.

## Конфиг

Скопируй `config.example.ini` → `config.ini` рядом с DLL. Значения по умолчанию
в структуре — `34 / 16 / 28 / 32` (стандартный Win11: `48 / 24 / 44 / 32`),
кнопка Reset в панели возвращает заводские `48 / 24 / 44 / 32`.

## Риски

- **Инжект в `explorer.exe`.** Ошибка в хуке может уронить оболочку Windows.
  Откат — `loader.exe --uninstall`, иначе перезапуск `explorer.exe`.
- **SmartScreen и антивирусы.** Инжектор и DLL без подписи кода часто помечаются
  как подозрительные — это ожидаемо.
- **Обновления Windows.** После каждого KB-обновления офсеты и GUID/age PDB
  могут измениться; до обновления таблицы хуки не ставятся (инжект — no-op).
- **Не проверено:** потребление памяти и CPU внутри `explorer.exe` не измерялось.
