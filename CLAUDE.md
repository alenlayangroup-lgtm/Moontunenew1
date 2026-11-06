# MoonTune — контекст для Claude Code

Этот файл — передача дел из облачной сессии Claude. Читай его целиком перед работой.

## Пользователь и правила работы
- Владелец бренда **Moon Music Label** (https://www.moonmusiclabel.com). Отвечай **по-русски**.
- **Не собирать и не пушить**, пока пользователь не скажет «собирай». Push в `main` запускает CI-сборку и обновляет релиз.
- Когда просят — **сначала показать пункты** (план), потом делать.
- Пользователь устал от ручных шагов: всё, что можешь проверить сам на этом Маке (файлы, auval, логи), делай сам.
- Главное — **качество и естественность звука**. Никогда не имитировать функциональность.

## Продукт
MoonTune — редактор вокала в духе Melodyne. VST3 (Windows, macOS), AU (macOS), **ARA 2**.
C++20, JUCE 8.0.10, CMake, ARA SDK 2.2.0 (FetchContent). Версия сейчас **0.3.2**.
Коды AU: тип `aufx`, subtype `MnTn`, manufacturer `MnML`. Bundle id `com.moonmusiclabel.moontune`.

## Сборка и релизы
- Репозиторий: `alenlayangroup-lgtm/Moontunenew1`. CI: `.github/workflows/build.yml`
  (windows: MSVC + Inno Setup + pluginval; macos: universal, ad-hoc подпись, .pkg внутри .dmg, auval + pluginval;
  ara-check: Celemony ARATestHost на Linux; release: тег `latest` с `MoonTune-macOS.dmg`,
  `MoonTune-macOS-plugins.zip`, `MoonTune-Windows-Setup.exe`).
- Локально на Маке: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`
  (тесты: `build/MoonTuneTests_artefacts/Release/MoonTuneTests`; фильтр — переменная `MOONTUNE_TEST_FILTER`).
  Тесты производительности имеют лимиты времени и падают в Debug-сборке — это нормально.
- Коммит только документации: добавляй `[skip ci]`, чтобы не пересобирать релиз.

## Архитектура (кратко)
- `Source/Analysis` — F0, ноты, вдохи/согласные (NoiseRegion: breath/sibilant/plosive, hfRatio).
- `Source/DSP/VocalRenderer` — движки: **Harmony Clean** (фазово-синхронный гармонический ресинтез + остаток через PSOLA),
  **True Voice** (фазово-синхронный PSOLA), **Hybrid**. Кэш модели голоса и кусков регионов; защита атак (б/п/д).
- `Source/DSP/Resampler.h` — SincResampler (Kaiser, 48 нулей), без задержки.
- `Source/DSP/RenderManager` — фоновый рендер; стерео сохраняется: `out_ch = base_ch + (rendered − originalMono)`.
- `Source/PluginProcessor` — параметры, прослушивание (audition/preview), **ARA**: `pollAra()` по таймеру 5 Гц
  собирает регионы хоста, `AraImportJob` читает аудио в фоне на временную шкалу песни, управление транспортом хоста
  (`requestHostLocate/Play/Stop`).
- `Source/ARA/MoonTuneARA.*` — DocumentController и фабрика ARA.
- `Source/UI/NoteGridView` — сетка нот, инструменты EDIT/SPLIT/GAIN, BREATH, волна, линейка/локатор.

## Открытая проблема (главная задача сейчас)
**Logic Pro: ARA не читает вокал.** Плагин виден в Менеджере плагинов как `MoonTune` (effect) и `MoonTune (ARA)`,
оба «successfully validated», галочки Use стоят. Окно `MoonTune (ARA)` открывается в первом слоте Audio FX
моно-аудиодорожки с вокалом (новый проект, одна дорожка), но показывает подсказку «No vocal yet / Press REC» —
то есть `araActive == false`, нот нет.

- В **0.3.1** причина предполагалась такая: код брал регионы только у `getPlaybackRenderer()`, а Logic может
  привязать экземпляр только с ролями редактора (форум JUCE: стерео-файл на моно-дорожке → нет playback renderer).
- В **0.3.2** (уже в релизе) `pollAra()` использует `isBoundToARA()` и берёт регионы у playback renderer →
  editor renderer (регионы и region sequences) → документа (если одна дорожка). **Результат в Logic ещё не проверен.**

Что проверить на этом Маке:
1. Какая версия установлена и нет ли дублей:
   `ls -la ~/Library/Audio/Plug-Ins/Components /Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/VST3 /Library/Audio/Plug-Ins/VST3 | grep -i moon`
   и `defaults read <путь>/MoonTune.component/Contents/Info.plist CFBundleShortVersionString`.
2. `auval -v aufx MnTn MnML` — проходит ли.
3. Если всё ещё нет нот: добавить временную диагностику (лог в `~/Library/Logs/MoonTune.log`: bound/роли,
   число регионов у каждой роли, isSampleAccessEnabled у источников), собрать локально, проверить в Logic.
   Возможные причины: Logic не включает sample access; регионы приходят позже; формат канала (стерео-файл на моно-дорожке).

## Другие пункты из 0.3.2 (проверить у пользователя)
- Tab / Shift+Tab и Alt+→ / Alt+← — переход к следующей/предыдущей ноте с переносом курсора.
- Клик по линейке в плагине должен двигать курсор Logic (работает только при подключённом ARA).
- След надписей на экране после закрытия окна: редактор сделан непрозрачным и прячется при закрытии.

## Планы на потом (из разговоров)
- Отдельный плагин для подгонки даблов (как Revoice), ARA, Mac и Windows.
