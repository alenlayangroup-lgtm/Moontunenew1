# Сборка MoonTune (Windows VST3, macOS AU + VST3, ARA 2)

## Что нужно установить

1. **Visual Studio 2022** (Community подойдёт) с рабочей нагрузкой
   «Разработка классических приложений на C++» (MSVC v143 + Windows 10/11 SDK).
2. **CMake 3.22+** (идёт в составе Visual Studio; можно поставить отдельно с cmake.org).
3. **Git**: при первой конфигурации CMake сам скачает JUCE 8.0.10 и ARA SDK 2.2.0.

## Сборка из командной строки

Откройте «Developer PowerShell for VS 2022» в папке проекта:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Готовый плагин:

```
build\MoonTune_artefacts\Release\VST3\MoonTune.vst3
```

Установка — скопировать папку `MoonTune.vst3` целиком в
`C:\Program Files\Common Files\VST3\` и пересканировать плагины в DAW.

Если JUCE уже скачан локально, можно не качать его заново:

```powershell
cmake -S . -B build -A x64 -DMOONTUNE_JUCE_PATH=C:\path\to\JUCE
```

## Сборка через GitHub Actions (Windows + macOS)

Файл `.github/workflows/build.yml` при каждом push запускает три задания.

- **windows**: собирает VST3 с ARA, запускает автотесты и pluginval (strictness 10).
  Артефакт `MoonTune-Windows-VST3`.
- **macos**: универсальная сборка для Apple Silicon и Intel, AU и VST3 с ARA, подпись ad hoc,
  автотесты, auval и pluginval. Артефакт `MoonTune-macOS-AU-VST3` (внутри INSTALL-macOS.txt).
- **ara-check**: собирает тестовый ARA-хост Celemony и прогоняет на плагине все его тест-кейсы.

## macOS вручную

```bash
cmake -S . -B build -G Xcode
cmake --build build --config Release
```

Результат: `build/MoonTune_artefacts/Release/AU/MoonTune.component` и `.../VST3/MoonTune.vst3`.
Для распространения без предупреждений macOS нужны подпись Developer ID и нотаризация
(учётная запись Apple Developer). Без них установка описана в INSTALL-macOS.txt.

## Сайт для ARA

ARA требует адрес сайта производителя. Сейчас указана заглушка, её нужно заменить:
`-DMOONTUNE_WEBSITE=https://ваш-сайт` (или поменять значение в CMakeLists.txt).

## Проверка pluginval вручную (по желанию)

```powershell
pluginval.exe --strictness-level 10 --validate-in-process "build\MoonTune_artefacts\Release\VST3\MoonTune.vst3"
```

## Ручная проверка в DAW (Этап 1)

1. Вставить MoonTune на моно-вокальную дорожку (Studio One / Fender Studio Pro, Reaper, Cubase).
2. Звук должен проходить без изменений; Output меняет громкость плавно, без щелчков.
3. BYPASS (и обход в DAW) — без щелчков.
4. Изменить размер окна, прокрутить/масштабировать сетку, сохранить проект,
   закрыть, открыть снова — громкость, обход, размер окна и масштаб должны восстановиться.
5. Повторить при 44,1 / 48 / 88,2 / 96 кГц и разных размерах буфера.
