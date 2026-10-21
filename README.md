# s-panel — уменьшение высоты панели задач Windows 11

Производная от Windhawk-мода `taskbar-icon-size` (m417z, GPLv3). Лицензия проекта — **GPLv3**, см. [LICENSE](LICENSE). MinHook в `vendor/` — BSD-2-Clause (`vendor/minhook/LICENSE.txt`).

## Что делает

- Перехватывает одну функцию `TaskbarConfiguration::GetFrameSize` в `Taskbar.dll` и отдаёт заданную высоту панели (`taskbarHeight` из конфига).
- Вертикальную панель (слева/справа) не трогает.
- Только x64, только сборка Windows 10.0.26100 (24H2).

## Состояние

**Хуки отключены.** Проверка хэша `.text` в `src/dll/hooks.cpp` (`kExpectedTextHash`) не подтверждена — значение `0`, и `InstallHooks` сразу возвращает `false`. Пока значение не будет снято с эталонного `Taskbar.dll` и вписано, DLL ничего не патчит.

Резолвер ищет RVA функции по байтовой сигнатуре и принимает результат только при уникальном совпадении: несколько кандидатов = отказ, а не угадывание.

## Как устроено

- `taskbar.dll` (MinHook, `/MT`) загружается **внутрь `explorer.exe`**.
- `loader.exe` инжектит DLL через `CreateRemoteThread + LoadLibraryW` в `explorer.exe` и завершается.
- Конфиг `config.ini` лежит рядом с DLL. Шаблон — `config.example.ini`.

## Сборка

Нужны Visual Studio 2022 Build Tools (Desktop C++), Windows 11 SDK 26100, CMake, Ninja.

```
tools\check_env.bat
build.bat [--clean]
```

Результат — `build\bin\taskbar.dll`, `build\bin\loader.exe`.

## Тесты

```
tests\run_tests.bat
```

Сухие тесты: `test_config`, `test_host`, `test_resolve`, а также `loader.exe --check` (только чтение, без обращения к explorer).

Доступные команды `loader.exe`: `--check`, `--inject`. `--install` и `--uninstall` ещё не реализованы.

## Конфиг

Скопируй `config.example.ini` → `config.ini` рядом с DLL. Значения по умолчанию: `34 / 16 / 28` (стандартный Win11: `48 / 24 / 44`).

## Риски

- **Инжект в `explorer.exe`.** Ошибка в хуке может уронить оболочку Windows. Откат через `loader.exe --uninstall` пока не реализован (заглушка до Task 5); для отката перезапусти `explorer.exe`.
- **SmartScreen и антивирусы.** Инжектор и DLL без подписи кода часто помечаются как подозрительные — это ожидаемо.
- **Обновления Windows.** После каждого KB-обновления офсеты и хэш `Taskbar.dll` могут измениться. Нужна повторная проверка и обновление сигнатуры/хэша; до этого хуки не работают.
- **Не проверено:** потребление памяти и CPU внутри `explorer.exe` не измерялось.
