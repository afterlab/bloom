; The Windows installer of Bloom (Inno Setup 6). The build makes it with
; cmake/package/make_windows_installer.cmake; docs/architecture/platform-support.md,
; "Windows Package", says what it does.
;
; Command lines of the installer it makes (Inno Setup's own switches):
;
;   Interactive           Bloom-<version>-windows-x86_64-setup.exe
;   Silent, deployment    Bloom-<version>-windows-x86_64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES
;                             /NORESTART [/DIR="<folder>"] [/TASKS="addtopath"]
;   Silent upgrade, as    Bloom-<version>-windows-x86_64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES
;   Bloom's updater runs      /NORESTART /RELAUNCH=1
;   it
;   Silent uninstall      <install folder>\unins000.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
;
; /RELAUNCH=1 starts Bloom again after a silent install. Without it a silent install starts
; nothing. An interactive install offers to start Bloom on its last page.
;
; The installer never asks for administrator rights: it installs for the current user only.
;
; The build gives these values with the compiler's --define option. Each has a default, so the
; script also compiles by hand from this folder.
;
;   AppVersion          the version label, for example 0.1.0-alpha.1
;   NumericVersion      the same as four numbers, for the file's version resource
;   SourceDir           the package folder to install
;   RepositoryDir       the source tree (pictures, icon, license)
;   OutputDir           where the installer is written
;   OutputBaseFilename  the installer's name without .exe
;
; Only the package check's own test changes these, so that it never touches an installed Bloom:
;
;   AppId  AppName  ProjectExtension  ProjectProgId  PathValueName

#ifndef AppVersion
  #define AppVersion "0.1.0-alpha.1"
#endif
#ifndef NumericVersion
  #define NumericVersion "0.1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\package\Bloom-" + AppVersion + "-windows-x86_64"
#endif
#ifndef RepositoryDir
  #define RepositoryDir "..\.."
#endif
#ifndef OutputDir
  #define OutputDir "Output"
#endif
#ifndef OutputBaseFilename
  #define OutputBaseFilename "Bloom-" + AppVersion + "-windows-x86_64-setup"
#endif

; The identity of the installation. It never changes: a newer installer finds the installed
; Bloom by it and upgrades it in place. (In Inno Setup a brace is written twice at the start.)
#ifndef AppId
  #define AppId "{{21EB1CCC-F264-41D4-ADFE-FACB3F41D273}"
#endif
#ifndef AppName
  #define AppName "Bloom"
#endif
#ifndef ProjectExtension
  #define ProjectExtension ".bloom"
#endif
#ifndef ProjectProgId
  #define ProjectProgId "Bloom.Project"
#endif
; The value of HKEY_CURRENT_USER\Environment that the "Add bloom-cli to PATH" option changes.
#ifndef PathValueName
  #define PathValueName "Path"
#endif

#define PictureDir RepositoryDir + "\apps\bloom\installer\windows"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Afterlab
AppPublisherURL=https://github.com/afterlab/bloom
AppSupportURL=https://github.com/afterlab/bloom
; Always for the current user. There is no PrivilegesRequiredOverridesAllowed on purpose: no
; command line and no page switches to an installation for all users.
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
UsePreviousAppDir=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=no
SetupIconFile={#RepositoryDir}\apps\bloom\windows\Bloom.ico
UninstallDisplayIcon={app}\Bloom.exe
UninstallDisplayName={#AppName}
; Setup picks the picture that fits the display scale best.
WizardImageFile={#PictureDir}\wizard-large-164x314.bmp,{#PictureDir}\wizard-large-192x386.bmp,{#PictureDir}\wizard-large-246x459.bmp,{#PictureDir}\wizard-large-273x556.bmp,{#PictureDir}\wizard-large-328x604.bmp,{#PictureDir}\wizard-large-355x700.bmp,{#PictureDir}\wizard-large-410x797.bmp
WizardSmallImageFile={#PictureDir}\wizard-small-55x55.bmp,{#PictureDir}\wizard-small-64x68.bmp,{#PictureDir}\wizard-small-83x80.bmp,{#PictureDir}\wizard-small-92x97.bmp,{#PictureDir}\wizard-small-110x106.bmp,{#PictureDir}\wizard-small-119x123.bmp,{#PictureDir}\wizard-small-138x140.bmp
LicenseFile={#RepositoryDir}\LICENSE
ChangesAssociations=yes
ChangesEnvironment=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#NumericVersion}
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\Bloom.exe"

[Tasks]
Name: "addtopath"; Description: "Add bloom-cli to PATH"; GroupDescription: "Command line:"; Flags: unchecked

; The project file type, for the current user only. Uninstall removes the keys.
[Registry]
Root: HKCU; Subkey: "Software\Classes\{#ProjectExtension}"; ValueType: string; ValueName: ""; ValueData: "{#ProjectProgId}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\{#ProjectProgId}"; ValueType: string; ValueName: ""; ValueData: "Bloom Project"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\{#ProjectProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Bloom.exe,0"
Root: HKCU; Subkey: "Software\Classes\{#ProjectProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Bloom.exe"" ""%1"""

; When files stay behind from an upgrade over the old folder (see PrepareToInstall), the
; folders Bloom's own parts live in are made new first.
[InstallDelete]
Type: filesandordirs; Name: "{app}\python"
Type: filesandordirs; Name: "{app}\bloom-python"
Type: filesandordirs; Name: "{app}\qt"
Type: filesandordirs; Name: "{app}\media"

[UninstallDelete]
Type: files; Name: "{app}\install-kind"
; The interpreter writes its caches into these folders.
Type: filesandordirs; Name: "{app}\python"
Type: filesandordirs; Name: "{app}\bloom-python"
; The version an upgrade kept (see PrepareToInstall).
Type: filesandordirs; Name: "{app}.previous"

[Run]
; After an interactive install: the usual checkbox on the last page.
Filename: "{app}\Bloom.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
; After a silent install: only when the command line has /RELAUNCH=1 (Bloom's updater passes it).
Filename: "{app}\Bloom.exe"; Flags: nowait postinstall skipifnotsilent; Check: RelaunchRequested

[Code]
const
  EnvironmentKey = 'Environment';
  PathValue = '{#PathValueName}';

function RelaunchRequested(): Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

// The folder the PATH option puts into the user's PATH.
function CliFolder(): String;
begin
  Result := ExpandConstant('{app}\cli');
end;

// Two PATH entries are the same when they are the same folder, whatever the case, with or
// without a closing backslash.
function SamePathEntry(const A, B: String): Boolean;
var
  Left, Right: String;
begin
  Result := False;
  Left := Trim(A);
  Right := Trim(B);
  if (Left <> '') and (Right <> '') then
    Result := SameText(RemoveBackslash(Left), RemoveBackslash(Right));
end;

// Value without the entries that are Entry. Every other entry stays as it is, in its place.
// Found tells whether Entry was there.
function PathWithout(const Value, Entry: String; var Found: Boolean): String;
var
  Rest, Item: String;
  Split: Integer;
  More, First: Boolean;
begin
  Result := '';
  Found := False;
  Rest := Value;
  More := True;
  First := True;
  while More do
  begin
    Split := Pos(';', Rest);
    if Split = 0 then
    begin
      Item := Rest;
      Rest := '';
      More := False;
    end
    else
    begin
      Item := Copy(Rest, 1, Split - 1);
      Rest := Copy(Rest, Split + 1, Length(Rest));
    end;
    if SamePathEntry(Item, Entry) then
      Found := True
    else
    begin
      if not First then
        Result := Result + ';';
      Result := Result + Item;
      First := False;
    end;
  end;
end;

// Adds the cli folder to the user's PATH unless it is an entry already. The value keeps (or
// gets) the type REG_EXPAND_SZ. The system PATH is never touched.
procedure AddCliToPath();
var
  Value, Unused: String;
  Found: Boolean;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, PathValue, Value) then
    Value := '';
  Unused := PathWithout(Value, CliFolder(), Found);
  if Found then
    Exit;
  if (Value <> '') and (Value[Length(Value)] <> ';') then
    Value := Value + ';';
  if not RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvironmentKey, PathValue, Value + CliFolder()) then
    Log('Could not add the cli folder to PATH.');
end;

// Removes exactly the cli folder from the user's PATH, and nothing else. A value that is empty
// afterwards is removed.
procedure RemoveCliFromPath();
var
  Value, Remaining: String;
  Found: Boolean;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, PathValue, Value) then
    Exit;
  Remaining := PathWithout(Value, CliFolder(), Found);
  if not Found then
    Exit;
  if Remaining = '' then
    RegDeleteValue(HKEY_CURRENT_USER, EnvironmentKey, PathValue)
  else
    RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvironmentKey, PathValue, Remaining);
end;

// An upgrade keeps the version it replaces beside the new one, as <folder>.previous, so that
// Bloom's updater can put it back when the new version does not start (ADR 0041). When the
// folder cannot be renamed (a file in it is in use) the upgrade goes on over the existing
// folder and no previous version is kept.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Folder, Previous: String;
begin
  Result := '';
  Folder := RemoveBackslash(ExpandConstant('{app}'));
  if not FileExists(Folder + '\Bloom.exe') then
    Exit;
  Previous := Folder + '.previous';
  if DirExists(Previous) then
    DelTree(Previous, True, True, True);
  if not RenameFile(Folder, Previous) then
    Log('The installed folder could not be renamed; installing over it.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    // The mark of a copy this installer installed (src/update/install_kind.cpp).
    if not SaveStringToFile(ExpandConstant('{app}\install-kind'), 'kind=windows-installer' + #10, False) then
      Log('Could not write the install-kind file.');
    if WizardIsTaskSelected('addtopath') then
      AddCliToPath();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveCliFromPath();
end;
