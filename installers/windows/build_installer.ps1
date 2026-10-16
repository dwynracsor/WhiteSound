# Builds white-sound.exe and packages it into a per-user Windows installer.
#
# Usage:  powershell -ExecutionPolicy Bypass -File installers\windows\build_installer.ps1
# Output: installers\WhiteSoundSetup-<version>.exe
#
# Requires Inno Setup 6 (ISCC.exe). Install with:
#     winget install JRSoftware.InnoSetup

$ErrorActionPreference = "Stop"

$Here     = $PSScriptRoot
$Root     = Split-Path -Parent (Split-Path -Parent $Here)
$OutDir   = Join-Path $Root "installers"
$BuildDir = Join-Path $Root "build"
$Iss      = Join-Path $Here "white_sound.iss"

Write-Host "Stopping any running instance..."
Get-Process -Name white-sound -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    Write-Host "Configuring (no build cache yet)..."
    cmake -G Ninja -B $BuildDir -S $Root -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "Configure failed (cmake exited $LASTEXITCODE)" }
}

Write-Host "Building the application..."
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed (cmake --build exited $LASTEXITCODE)" }

$candidates = @(
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
)
$Iscc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Iscc) {
    throw "ISCC.exe not found. Install Inno Setup 6: winget install JRSoftware.InnoSetup"
}

Write-Host "Compiling the installer with $Iscc"
& $Iscc $Iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

Write-Host ""
Write-Host "Done. Installer(s) in $OutDir`:"
Get-ChildItem $OutDir -Filter "WhiteSoundSetup-*.exe" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 Name, Length, LastWriteTime
