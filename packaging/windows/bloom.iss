; The Windows installer of Bloom (Inno Setup 6). The build makes it with
; cmake/package/make_windows_installer.cmake; docs/architecture/platform-support.md,
; "Windows Package", says what it does.
;
; Command lines of the installer it makes (Inno Setup's own switches):
;
;   Interactive           Bloom-<version>-windows-x86_64-setup.exe
;   Silent, deployment    Bloom-<version>-windows-x86_64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES
;                             /NORESTART [/DIR="<folder>"] [/TASKS="addtopath"]
;   Upgrade, as Bloom's   Bloom-<version>-windows-x86_64-setup.exe /SILENT /SUPPRESSMSGBOXES
;   updater runs it           /NOCANCEL /NORESTART /NOCLOSEAPPLICATIONS /RELAUNCH=1
;   (tools/release/release.py, INSTALLER_ARGUMENTS)
;   Silent uninstall      <install folder>\unins000.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
;
; /RELAUNCH=1 starts Bloom again after a silent install. Without it a silent install starts
; nothing. An interactive install offers to start Bloom on its last page.
;
; An update (/RELAUNCH=1, #788) shows only its progress window, titled "Updating Bloom", from the
; moment Bloom closes until the new version opens, and asks nothing (/SILENT, /SUPPRESSMSGBOXES,
; /NOCANCEL). It never installs without keeping the version it replaces (PrepareToInstall), never
; closes a Bloom (/NOCLOSEAPPLICATIONS), and when it cannot finish it puts that version back,
; leaves it a notice and starts it (DeinitializeSetup).
;
; While any run of this installer goes on it holds the mutex SetupMutexName. A Bloom opened by
; hand meanwhile shows this installer's window and ends at once (apps/bloom/main.cpp,
; src/update/installer_activity.cpp), so it neither runs the version being replaced nor holds
; its folder. The installer starts Bloom with StartedByInstaller, the one start that goes on.
;
; The installer never asks for administrator rights: it installs for the current user only.
;
; The build gives these values with the compiler's /D option. Each has a default, so the
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
;   AppId  AppName  ProjectExtension  ProjectProgId  PathValueName  SetupMutexName

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

; The mutex this installer holds while it runs, and the argument it starts Bloom with. Bloom's own
; copies of both are in src/update/include/bloom/update/installer_activity.hpp
; (kWindowsSetupMutex, kStartedByInstallerArgument); tools/release checks that they agree.
#ifndef SetupMutexName
  #define SetupMutexName "AfterlabBloomSetup"
#endif
#define StartedByInstaller "--started-by-installer"

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
SetupMutex={#SetupMutexName}
VersionInfoVersion={#NumericVersion}
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}

; The files beside the program first, then the program, then the folders. A Bloom.exe that is
; opened while an update installs then always finds the libraries it starts with, and ends at once
; (see the top of this file). The folders are listed one entry each when the script is compiled.
[Files]
Source: "{#SourceDir}\*"; Excludes: "\Bloom.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Bloom.exe"; DestDir: "{app}"; Flags: ignoreversion
#define FindHandle
#define FindResult
#define FolderName
#sub AddFolder
  #define FolderName FindGetFileName(FindHandle)
  #if FolderName != "." && FolderName != ".." && DirExists(SourceDir + "\" + FolderName)
Source: "{#SourceDir}\{#FolderName}\*"; DestDir: "{app}\{#FolderName}"; Flags: recursesubdirs createallsubdirs ignoreversion skipifsourcedoesntexist
  #endif
#endsub
#for {FindHandle = FindResult = FindFirst(SourceDir + "\*", faDirectory); FindResult; FindResult = FindNext(FindHandle)} AddFolder
#if FindHandle
  #call FindClose(FindHandle)
#endif

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
Filename: "{app}\Bloom.exe"; Parameters: "{#StartedByInstaller}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
; After a silent install: only when the command line has /RELAUNCH=1 (Bloom's updater passes it).
Filename: "{app}\Bloom.exe"; Parameters: "{#StartedByInstaller}"; Flags: nowait postinstall skipifnotsilent; Check: RelaunchRequested

[Code]
const
  EnvironmentKey = 'Environment';
  PathValue = '{#PathValueName}';
  // The notice an update that could not be installed leaves for the version put back
  // (src/update/installer_activity.hpp, kInstallFailureNoticeName).
  FailureNoticeName = 'update-failed';

var
  // The installed folder and the name its version is kept under, once PrepareToInstall knows them.
  InstalledFolder, KeptFolder: String;
  // The installed folder was renamed to KeptFolder.
  KeptAside: Boolean;
  // Every file is installed (ssPostInstall).
  InstallDone: Boolean;
  // Why an update did not install, in words for the artist; empty when nothing is known.
  FailureReason: String;

function RelaunchRequested(): Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

// An update's window says what happens, from the moment Bloom closes until it opens again (#788).
// Its title is the one a Bloom opened meanwhile looks for (installer_activity.hpp).
procedure InitializeWizard();
begin
  if RelaunchRequested() then
    WizardForm.Caption := 'Updating {#AppName}';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if not RelaunchRequested() then
    Exit;
  if CurPageID = wpPreparing then
  begin
    WizardForm.PageNameLabel.Caption := 'Updating {#AppName}';
    WizardForm.PageDescriptionLabel.Caption :=
      '{#AppName} is closing so that it can be updated to {#AppVersion}. It opens again by itself when the update is done.';
  end
  else if CurPageID = wpInstalling then
  begin
    WizardForm.PageNameLabel.Caption := 'Updating {#AppName}';
    WizardForm.PageDescriptionLabel.Caption :=
      'Installing {#AppName} {#AppVersion}. {#AppName} opens again by itself when it is done.';
  end;
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
// Bloom's updater can put it back when the new version does not start (ADR 0041).
//
// Bloom's updater starts this installer and then closes Bloom (/RELAUNCH=1), so the folder is
// still in use for a moment. In that run the rename is tried again for up to a minute, until
// Bloom has gone; a Bloom opened meanwhile ends at once (see the top of this file). An update
// never goes on without the kept version (#788): when the folder still cannot be renamed, it
// installs nothing, and DeinitializeSetup starts the version that is there and says why.
//
// An install the artist or a deployment started goes on over the existing folder when it cannot
// be renamed (a file in it is in use), and keeps no previous version.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Tries: Integer;
begin
  Result := '';
  InstalledFolder := RemoveBackslash(ExpandConstant('{app}'));
  KeptFolder := InstalledFolder + '.previous';
  if not FileExists(InstalledFolder + '\Bloom.exe') then
    Exit;
  if DirExists(KeptFolder) then
    DelTree(KeptFolder, True, True, True);
  Tries := 1;
  if RelaunchRequested() then
    Tries := 120;
  while not RenameFile(InstalledFolder, KeptFolder) do
  begin
    Tries := Tries - 1;
    if Tries <= 0 then
    begin
      if RelaunchRequested() then
      begin
        FailureReason := 'Another program kept its folder in use, so the update could not keep ' +
          'the version you had for going back.';
        Log('The installed folder could not be renamed; the update installs nothing.');
        Result := '{#AppName} was not updated: ' + FailureReason;
        Exit;
      end;
      Log('The installed folder could not be renamed; installing over it.');
      Exit;
    end;
    Sleep(500);
  end;
  KeptAside := True;
end;

// The folder the installer installs into, or '' before it is known.
function InstallFolder(): String;
begin
  Result := InstalledFolder;
  if Result = '' then
  begin
    try
      Result := RemoveBackslash(ExpandConstant('{app}'));
    except
      Result := '';
    end;
  end;
end;

// An update that did not install (it failed, or stopped): the version that was there is put back
// in its place, is told what happened (src/update/installer_activity.hpp) and is started, so the
// artist has a working Bloom again (#788). The update helper's watch, which Bloom started before
// it closed, sees that version start and ends.
procedure DeinitializeSetup();
var
  Folder, Reason: String;
  ResultCode: Integer;
begin
  if (not RelaunchRequested()) or InstallDone then
    Exit;
  Folder := InstallFolder();
  if Folder = '' then
  begin
    Log('The update stopped before its folder was known; nothing to start.');
    Exit;
  end;
  if KeptAside then
  begin
    // What the stopped install left in the folder goes, and the kept version comes back.
    if DirExists(Folder) then
      DelTree(Folder, True, True, True);
    if not RenameFile(KeptFolder, Folder) then
    begin
      // The update helper puts the kept version back when no version confirms its start.
      Log('The kept version could not be put back; the update helper restores it.');
      Exit;
    end;
  end;
  if not FileExists(Folder + '\Bloom.exe') then
    Exit;
  Reason := FailureReason;
  if Reason = '' then
    Reason := 'The installer stopped before the update was installed.';
  if not SaveStringToFile(Folder + '\' + FailureNoticeName,
                          'version={#AppVersion}' + #13#10 + 'reason=' + Reason + #13#10, False) then
    Log('Could not write the notice of the update that was not installed.');
  if not Exec(Folder + '\Bloom.exe', '{#StartedByInstaller}', Folder, SW_SHOWNORMAL, ewNoWait,
              ResultCode) then
    Log('Could not start the version that was put back.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    InstallDone := True;
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
