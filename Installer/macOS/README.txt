MoonTune for macOS (Apple Silicon and Intel)

1. Double-click "MoonTune.pkg" and follow the installer (AU and VST3, you can untick one).

2. The installer is not signed with an Apple Developer ID yet, so the first time macOS says it is from
   an unidentified developer. Then: right-click (Control-click) MoonTune.pkg > Open > Open.
   On newer macOS: System Settings > Privacy & Security > "Open Anyway".

3. Restart the DAW and let it rescan plug-ins
   (Logic: Settings > Plug-in Manager > Reset & Rescan if MoonTune is marked as failed).

If a DAW still does not see MoonTune, run once in Terminal:
   sudo xattr -cr /Library/Audio/Plug-Ins/Components/MoonTune.component /Library/Audio/Plug-Ins/VST3/MoonTune.vst3
   killall -9 AudioComponentRegistrar
