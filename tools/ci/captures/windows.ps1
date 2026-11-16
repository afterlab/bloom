# Captures the installed Bloom's icon on Windows (#670): the Start menu folder, the Start menu search,
# a project file in Explorer and the taskbar with Bloom running. PowerShell 7, in the logged-in
# desktop session of a hosted runner. Bloom is installed for the current user by its installer.
#
#   windows.ps1 -Out <folder> -Installed <install folder> [-Project <a .bloom file>]
#
# Without -Project, the project is made with bloom-cli.exe from the install folder.
param(
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$Installed,
    [string]$Project
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -Namespace Native -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(System.IntPtr hWnd, out int processId);
'@
[void][Native.User32]::SetProcessDPIAware()

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$exe = Join-Path $Installed 'Bloom.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    [Console]::Error.WriteLine("windows.ps1: $exe does not exist")
    exit 1
}
$work = Join-Path $env:USERPROFILE ("bloom-captures-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$script:failed = $false
$captures = Join-Path $Out 'captures.txt'
Set-Content -LiteralPath $captures -Value @() -Encoding utf8

function Fail([string]$Message) {
    [Console]::Error.WriteLine("windows.ps1: step failed: $Message")
    $script:failed = $true
}

# Shoot <file name> <what it shows>: the primary screen, checked for being more than one flat color.
function Shoot([string]$Name, [string]$What) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
        $graphics.Dispose()
        $colors = @{}
        foreach ($x in 0..15) {
            foreach ($y in 0..15) {
                $pixel = $bitmap.GetPixel([int]($x * ($bounds.Width - 1) / 15), [int]($y * ($bounds.Height - 1) / 15))
                $colors[$pixel.ToArgb()] = $true
            }
        }
        if ($colors.Count -lt 3) {
            throw "the screen is a single flat color ($($colors.Count) sampled), so the session has no usable display"
        }
        $bitmap.Save((Join-Path $Out $Name), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
    if ((Get-Item -LiteralPath (Join-Path $Out $Name)).Length -eq 0) { throw "$Name is empty" }
    Add-Content -LiteralPath $captures -Value "${Name}: $What" -Encoding utf8
}

function Bring-ToFront([int64]$Handle) {
    [void][Native.User32]::ShowWindow([IntPtr]$Handle, 9)
    [void][Native.User32]::SetForegroundWindow([IntPtr]$Handle)
}

# Opens a folder in an Explorer window in icon view with the given icon size and returns the window.
function Open-Explorer([string]$Path, [int]$IconSize, [string]$Select) {
    $shell = New-Object -ComObject Shell.Application
    Start-Process explorer.exe -ArgumentList ('"{0}"' -f $Path)
    $window = $null
    for ($try = 0; $try -lt 60 -and -not $window; $try++) {
        Start-Sleep -Milliseconds 500
        $window = @($shell.Windows()) | Where-Object {
            try { $_.Document.Folder.Self.Path -eq $Path } catch { $false }
        } | Select-Object -First 1
    }
    if (-not $window) { throw "Explorer showed no window for $Path" }
    $window.Left = 40; $window.Top = 40; $window.Width = 1100; $window.Height = 700
    $view = $window.Document
    $view.CurrentViewMode = 1   # FVM_ICON
    $view.IconSize = $IconSize
    if ($Select) {
        # Select, deselect the rest, scroll into view and focus.
        $view.SelectItem($view.Folder.ParseName($Select), 0x1D)
    }
    Bring-ToFront $window.HWND
    Start-Sleep -Seconds 2
    return $window
}

# --- 1. The Start menu folder --------------------------------------------------------------------

$name = '670-windows-start-menu.png'
try {
    $programs = [Environment]::GetFolderPath('Programs')
    $shortcut = Join-Path $programs 'Bloom.lnk'
    if (-not (Test-Path -LiteralPath $shortcut)) {
        $listing = (Get-ChildItem -LiteralPath $programs -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', '
        throw "the installer made no shortcut at $shortcut (the folder holds: $listing)"
    }
    $window = Open-Explorer -Path $programs -IconSize 96 -Select 'Bloom.lnk'
    try {
        Shoot $name 'An Explorer window of the Start menu Programs folder with the Bloom shortcut and its icon, large icons.'
    } finally {
        $window.Quit()
    }
} catch {
    Fail "${name}: $($_.Exception.Message)"
}

# --- 2. The Start menu search (the one optional picture) -----------------------------------------

$name = '670-windows-start-search.png'
function Start-MenuIsOpen {
    $handle = [Native.User32]::GetForegroundWindow()
    $processId = 0
    [void][Native.User32]::GetWindowThreadProcessId($handle, [ref]$processId)
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    return ($process -and $process.ProcessName -match '^(StartMenuExperienceHost|SearchHost|SearchApp|ShellExperienceHost)$')
}
try {
    [System.Windows.Forms.SendKeys]::SendWait('^{ESC}')
    $open = $false
    for ($try = 0; $try -lt 20 -and -not $open; $try++) {
        Start-Sleep -Milliseconds 500
        $open = Start-MenuIsOpen
    }
    if (-not $open) {
        $reason = 'the Start menu did not open in this session (Ctrl+Esc through SendKeys gave no Start menu window in front); this picture is optional'
        Add-Content -LiteralPath $captures -Value "${name}: not taken: $reason" -Encoding utf8
        [Console]::Error.WriteLine("windows.ps1: $name not taken: $reason")
    } else {
        [System.Windows.Forms.SendKeys]::SendWait('Bloom')
        Start-Sleep -Seconds 3
        Shoot $name 'The Start menu with Bloom typed into the search, showing the Bloom entry.'
        [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
    }
} catch {
    $reason = $_.Exception.Message
    Add-Content -LiteralPath $captures -Value "${name}: not taken: $reason" -Encoding utf8
    [Console]::Error.WriteLine("windows.ps1: $name not taken (optional): $reason")
    try { [System.Windows.Forms.SendKeys]::SendWait('{ESC}') } catch { }
}

# --- 3. A project file in Explorer ---------------------------------------------------------------

$name = '670-windows-project-file.png'
try {
    $folder = Join-Path $work 'Projects'
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $file = Join-Path $folder 'Example.bloom'
    if ($Project) {
        Copy-Item -LiteralPath $Project -Destination $file
    } else {
        $cli = Join-Path $Installed 'bloom-cli.exe'
        if (-not (Test-Path -LiteralPath $cli)) { throw "no -Project was given and $cli does not exist" }
        $log = & $cli new $file 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "bloom-cli new failed: $log" }
    }
    if (-not (Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).Length -eq 0) {
        throw "no project file was made at $file"
    }
    $window = Open-Explorer -Path $folder -IconSize 256 -Select 'Example.bloom'
    try {
        Shoot $name 'An Explorer window of a folder with one Bloom project file and its icon, large icons.'
    } finally {
        $window.Quit()
    }
} catch {
    Fail "${name}: $($_.Exception.Message)"
}

# --- 4. The taskbar with Bloom running -----------------------------------------------------------

$name = '670-windows-taskbar.png'
$bloom = $null
try {
    $bloom = Start-Process -FilePath $exe -PassThru
    $handle = 0
    for ($try = 0; $try -lt 120 -and $handle -eq 0; $try++) {
        Start-Sleep -Milliseconds 500
        $bloom.Refresh()
        if ($bloom.HasExited) { throw "Bloom exited at once with code $($bloom.ExitCode)" }
        $handle = [int64]$bloom.MainWindowHandle
    }
    if ($handle -eq 0) { throw 'Bloom showed no main window in 60 seconds' }
    Bring-ToFront $handle
    Start-Sleep -Seconds 3
    Shoot $name 'The desktop with Bloom running: its window, and its icon on the taskbar.'
} catch {
    Fail "${name}: $($_.Exception.Message)"
} finally {
    if ($bloom -and -not $bloom.HasExited) {
        [void]$bloom.CloseMainWindow()
        [void]$bloom.WaitForExit(10000)
        if (-not $bloom.HasExited) { Stop-Process -Id $bloom.Id -Force }
    }
}

# --- Result --------------------------------------------------------------------------------------

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
foreach ($required in '670-windows-start-menu.png', '670-windows-project-file.png', '670-windows-taskbar.png') {
    $path = Join-Path $Out $required
    if (-not (Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -eq 0) {
        [Console]::Error.WriteLine("windows.ps1: $required was not produced")
        $script:failed = $true
    }
}
if ($script:failed) { exit 1 }
exit 0
