; MoonTune installer for Windows (Inno Setup 6). Built by GitHub Actions:
;   ISCC.exe /DAppVersion=x.y.z /DSourceDir=<folder containing MoonTune.vst3> Installer\Windows\MoonTune.iss
#ifndef AppVersion
  #define AppVersion "0.3.1"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\MoonTune_artefacts\Release\VST3"
#endif

[Setup]
AppId={{6E2B6F7E-1B0F-4F4B-9C6A-4D6F6E54756E}
AppName=MoonTune
AppVersion={#AppVersion}
AppPublisher=Moon Music Label
AppPublisherURL=https://www.moonmusiclabel.com
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputBaseFilename=MoonTune-Windows-Setup
Compression=lzma2
SolidCompression=yes
UninstallDisplayName=MoonTune (VST3)

[Files]
Source: "{#SourceDir}\MoonTune.vst3\*"; DestDir: "{commoncf64}\VST3\MoonTune.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Messages]
FinishedLabel=MoonTune is installed in C:\Program Files\Common Files\VST3. Restart your DAW and let it rescan plug-ins.
