; Inno Setup script for the TBCAD installer.
; Build:  iscc /DAppVersion=0.1.0 /DBuildDir=<folder with tbcad.exe> /DRepoDir=<repo root> /DOutDir=<out> tbcad.iss
; Inno Setup is only a build tool here; nothing of it ships inside TBCAD's own binaries.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildDir
  #error Pass /DBuildDir=<folder that holds tbcad.exe, tbcad-convert.exe, tbcad-mcp.exe and assets>
#endif
#ifndef RepoDir
  #define RepoDir "..\.."
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{6F1C2D7A-3B54-4E6B-9E0A-7B1D5C0A9D21}
AppName=TBCAD
AppVersion={#AppVersion}
AppVerName=TBCAD {#AppVersion}
AppPublisher=TBCAD contributors
AppPublisherURL=https://github.com/snabs1993/TBCAD
AppSupportURL=https://github.com/snabs1993/TBCAD/issues
DefaultDirName={autopf}\TBCAD
DefaultGroupName=TBCAD
DisableProgramGroupPage=yes
; Per-user install by default; the user may choose "for all users" on the first page.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile={#RepoDir}\LICENSE
OutputDir={#OutDir}
OutputBaseFilename=TBCAD-{#AppVersion}-setup
SetupIconFile=tbcad.ico
UninstallDisplayIcon={app}\tbcad.exe
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
ChangesAssociations=yes
VersionInfoVersion={#AppVersion}
VersionInfoCompany=TBCAD contributors
VersionInfoDescription=TBCAD setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: "assoc_tbcad"; Description: "Open .&tbcad drawings with TBCAD"; GroupDescription: "File types:"
Name: "assoc_dxf"; Description: "Add TBCAD to the ""Open with"" list of .&dxf files"; GroupDescription: "File types:"

[Files]
Source: "{#BuildDir}\tbcad.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\tbcad-convert.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\tbcad-mcp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "tbcad-open-dxf.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\docs\user-guide\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoDir}\samples\*"; DestDir: "{app}\samples"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\TBCAD"; Filename: "{app}\tbcad.exe"
Name: "{autodesktop}\TBCAD"; Filename: "{app}\tbcad.exe"; Tasks: desktopicon

[Registry]
; .tbcad: default program with our icon.
Root: HKA; Subkey: "Software\Classes\.tbcad"; ValueType: string; ValueName: ""; ValueData: "TBCAD.Drawing"; Flags: uninsdeletevalue; Tasks: assoc_tbcad
Root: HKA; Subkey: "Software\Classes\TBCAD.Drawing"; ValueType: string; ValueName: ""; ValueData: "TBCAD drawing"; Flags: uninsdeletekey; Tasks: assoc_tbcad
Root: HKA; Subkey: "Software\Classes\TBCAD.Drawing\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\tbcad.exe,0"; Tasks: assoc_tbcad
Root: HKA; Subkey: "Software\Classes\TBCAD.Drawing\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\tbcad.exe"" ""%1"""; Tasks: assoc_tbcad
; .dxf: only an "Open with" entry (we never take over the default program of DXF files). It
; converts the DXF with tbcad-convert and opens the result.
Root: HKA; Subkey: "Software\Classes\.dxf\OpenWithProgids"; ValueType: string; ValueName: "TBCAD.DxfImport"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc_dxf
Root: HKA; Subkey: "Software\Classes\TBCAD.DxfImport"; ValueType: string; ValueName: ""; ValueData: "DXF drawing (import into TBCAD)"; Flags: uninsdeletekey; Tasks: assoc_dxf
Root: HKA; Subkey: "Software\Classes\TBCAD.DxfImport\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\tbcad.exe,0"; Tasks: assoc_dxf
Root: HKA; Subkey: "Software\Classes\TBCAD.DxfImport\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\tbcad-open-dxf.cmd"" ""%1"""; Tasks: assoc_dxf

[Run]
Filename: "{app}\tbcad.exe"; Description: "Start TBCAD"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Settings and drawings in the user's folders are never removed; only what we put in {app}.
Type: filesandordirs; Name: "{app}\docs"
