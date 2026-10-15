# End-to-end test for the White Sound Windows installer.
#
#   powershell -ExecutionPolicy Bypass -File installer\test_install.ps1
#   powershell -ExecutionPolicy Bypass -File installer\test_install.ps1 -Setup dist\WhiteSoundSetup-1.0.0.exe
#
# Installs silently with the autostart and desktop-icon tasks, checks that the
# files, shortcuts, registry entries and version are all in place, launches the
# app and confirms the tray window exists, then uninstalls and checks that
# everything is gone. The uninstall always runs, even if a check fails.
#
# This is meant for a clean Windows machine (or VM) with no previous install. On
# a developer machine it still works: it removes a previous install first.
#
# Exit code is 0 only if every check passes.

[CmdletBinding()]
param(
    [string]$Setup = "",
    [int]$TimeoutSeconds = 45
)

$ErrorActionPreference = "Stop"

$AppName  = "White Sound"
$ExeName  = "white_sound_tray.exe"
$AppDir   = Join-Path $env:LOCALAPPDATA "Programs\$AppName"
$Exe      = Join-Path $AppDir $ExeName
$Unins    = Join-Path $AppDir "unins000.exe"
$StartLnk = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\$AppName.lnk"
$DeskLnk  = Join-Path ([Environment]::GetFolderPath("Desktop")) "$AppName.lnk"
$RunKey   = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"

# The uninstaller spawns a helper and exits early, so wait for the directory.
function Wait-Gone([string]$Path, [int]$Seconds) {
    for ($i = 0; $i -lt $Seconds; $i++) {
        if (-not (Test-Path $Path)) { return $true }
        Start-Sleep -Seconds 1
    }
    return -not (Test-Path $Path)
}

function Silent([string]$ExePath, [string]$Arguments) {
    return (Start-Process -FilePath $ExePath -ArgumentList $Arguments -Wait -PassThru).ExitCode
}

# The Add/Remove Programs entry Inno writes for the current user. It is found by
# DisplayName so the test does not depend on the exact AppId spelling.
function Get-UninstallEntry {
    Get-ChildItem "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall" -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue } |
        Where-Object { $_.DisplayName -like "$AppName*" }
}

# Enumerate the top-level window classes owned by a process. FindWindow cannot be
# used from PowerShell here because $null is marshalled as an empty string, not
# as NULL, so a class-only lookup never matches.
Add-Type -Namespace Ws -Name WinEnum -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, System.IntPtr lParam);
public delegate bool EnumWindowsProc(System.IntPtr hWnd, System.IntPtr lParam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr hWnd, System.Text.StringBuilder s, int n);
public static System.Collections.Generic.List<string> Classes(uint pid) {
    var list = new System.Collections.Generic.List<string>();
    EnumWindows((h, l) => {
        uint p; GetWindowThreadProcessId(h, out p);
        if (p == pid) {
            var c = new System.Text.StringBuilder(256); GetClassName(h, c, 256);
            list.Add(c.ToString());
        }
        return true;
    }, System.IntPtr.Zero);
    return list;
}
"@

$script:Results = New-Object System.Collections.Generic.List[object]
function Check([string]$Name, [scriptblock]$Test) {
    $ok = $false
    try { $ok = [bool](& $Test) } catch { $ok = $false }
    $script:Results.Add([pscustomobject]@{ Check = $Name; Pass = $ok })
    if ($ok) { Write-Host "  PASS  $Name" -ForegroundColor Green }
    else     { Write-Host "  FAIL  $Name" -ForegroundColor Red }
}

if (-not $Setup) {
    $Setup = Get-ChildItem (Join-Path $PSScriptRoot "..\dist") -Filter "WhiteSoundSetup-*.exe" `
        -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Setup -or -not (Test-Path $Setup)) {
    throw "No installer found. Build it first: installer\build_installer.ps1"
}
$Setup = (Resolve-Path $Setup).Path
# Inno pads the version resource with spaces, so compare trimmed strings.
$expectedVersion = ((Get-Item $Setup).VersionInfo.FileVersion).Trim()

Write-Host "Installer:         $Setup"
Write-Host "Expected version:  $expectedVersion"
Write-Host "Install location:  $AppDir"
Write-Host ""

try {
    # Start from a known state.
    if (Test-Path $Unins) {
        Write-Host "Removing a previous install first..."
        Silent $Unins "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART" | Out-Null
        Wait-Gone $AppDir $TimeoutSeconds | Out-Null
    }

    Write-Host "Installing silently (tasks: autostart + desktopicon)..."
    $code = Silent $Setup "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LANG=english /TASKS=`"autostart,desktopicon`""
    Check "installer exited 0" { $code -eq 0 }

    Write-Host "`nChecking the install..."
    Check "executable installed"          { Test-Path $Exe }
    Check "README.md installed"           { Test-Path (Join-Path $AppDir "README.md") }
    Check "LICENSE installed"             { Test-Path (Join-Path $AppDir "LICENSE") }
    Check "THIRD_PARTY_NOTICES installed" { Test-Path (Join-Path $AppDir "THIRD_PARTY_NOTICES.md") }
    Check "uninstaller installed"         { Test-Path $Unins }
    Check "Start Menu shortcut"           { Test-Path $StartLnk }
    Check "desktop shortcut"              { Test-Path $DeskLnk }
    Check "autostart Run entry"           { (Get-ItemProperty $RunKey -Name $AppName -ErrorAction Stop).$AppName -like "*$ExeName*" }
    Check "Add/Remove Programs entry"     { (Get-UninstallEntry | Measure-Object).Count -eq 1 }
    Check "registered DisplayVersion"     { ((Get-UninstallEntry).DisplayVersion).Trim() -eq $expectedVersion }
    Check "exe file version matches"      { ((Get-Item $Exe).VersionInfo.FileVersion).Trim() -eq $expectedVersion }

    Write-Host "`nLaunching the installed app..."
    $proc = Start-Process -FilePath $Exe -PassThru
    $pid_ = $proc.Id
    Start-Sleep -Seconds 3
    Check "app process is running" { $null -ne (Get-Process -Id $pid_ -ErrorAction SilentlyContinue) }
    Check "tray window exists"     { ([Ws.WinEnum]::Classes([uint32]$pid_)) -contains "WhiteSoundTrayClass" }
}
finally {
    Write-Host "`nStopping the app and uninstalling..."
    Get-Process -Name "white_sound_tray" -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    if (Test-Path $Unins) {
        Silent $Unins "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART" | Out-Null
        if (-not (Wait-Gone $AppDir $TimeoutSeconds)) {
            Write-Warning "The install directory did not disappear within $TimeoutSeconds s: $AppDir"
        }
    }
}

Write-Host "`nChecking the uninstall..."
Check "app directory removed"        { -not (Test-Path $AppDir) }
Check "Start Menu shortcut removed"  { -not (Test-Path $StartLnk) }
Check "desktop shortcut removed"     { -not (Test-Path $DeskLnk) }
Check "autostart Run entry removed"  { $null -eq (Get-ItemProperty $RunKey -Name $AppName -ErrorAction SilentlyContinue) }
Check "Add/Remove Programs removed"  { (Get-UninstallEntry | Measure-Object).Count -eq 0 }
Check "no process left running"      { $null -eq (Get-Process -Name "white_sound_tray" -ErrorAction SilentlyContinue) }

$passed = ($script:Results | Where-Object { $_.Pass }).Count
$total  = $script:Results.Count
Write-Host ""
if ($passed -eq $total) {
    Write-Host "All $total checks passed." -ForegroundColor Green
    exit 0
} else {
    Write-Host "$passed/$total checks passed." -ForegroundColor Red
    $script:Results | Where-Object { -not $_.Pass } | ForEach-Object { Write-Host "  failed: $($_.Check)" -ForegroundColor Red }
    exit 1
}
