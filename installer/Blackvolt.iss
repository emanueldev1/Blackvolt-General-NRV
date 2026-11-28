; Blackvolt installer (Inno Setup 6).
;
; Built by .github/workflows/release.yml from the self-contained publish folder:
;   iscc /DAppVersion=1.2.3 /DSourceDir=publish\Blackvolt /DOutputDir=. installer\Blackvolt.iss
;
; Per-user install by default (no administrator prompt, and the in-app updater can replace the
; files in place). DLSS NR runtimes are not packed here: like Veyra's package they are large
; (165-276 MB each), so Blackvolt fetches only those files from Veyra's NVIDIA package on first
; launch (Services/NrRuntimeInstaller.cs); the "install DLSS NR" task starts that right away.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\publish\Blackvolt"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F1B7C2E-4A8D-4F3A-9B5E-B1AC4C7E2D90}
AppName=Blackvolt
AppVersion={#AppVersion}
AppVerName=Blackvolt {#AppVersion}
AppPublisher=Emanuel
DefaultDirName={localappdata}\Programs\Blackvolt
DefaultGroupName=Blackvolt
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir={#OutputDir}
OutputBaseFilename=Blackvolt-{#AppVersion}-setup-x64
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\Blackvolt.exe
UninstallDisplayName=Blackvolt

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
es.NrTask=Descargar e instalar DLSS NR al terminar (recomendado en GPUs NVIDIA RTX)
en.NrTask=Download and install DLSS NR when setup finishes (recommended on NVIDIA RTX GPUs)
es.Components=Componentes:
en.Components=Components:
es.Launch=Abrir Blackvolt
en.Launch=Launch Blackvolt

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "nrruntime"; Description: "{cm:NrTask}"; GroupDescription: "{cm:Components}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Leftovers of an older layout must not shadow the new files.
Type: filesandordirs; Name: "{app}\runtime\legacy"

[Icons]
Name: "{group}\Blackvolt"; Filename: "{app}\Blackvolt.exe"
Name: "{group}\{cm:UninstallProgram,Blackvolt}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Blackvolt"; Filename: "{app}\Blackvolt.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Blackvolt.exe"; Parameters: "--install-nr"; Description: "{cm:Launch}"; \
    Flags: nowait postinstall skipifsilent; Tasks: nrruntime
Filename: "{app}\Blackvolt.exe"; Description: "{cm:Launch}"; \
    Flags: nowait postinstall skipifsilent; Tasks: not nrruntime

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
; User data (%LOCALAPPDATA%\Blackvolt: settings, paired consoles, downloaded runtimes) is kept,
; so reinstalling does not need another 276 MB download.
