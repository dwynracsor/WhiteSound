# Build the Linux installer by running the build inside WSL (Ubuntu).
#
#   powershell -ExecutionPolicy Bypass -File installers\linux\build_linux_via_wsl.ps1
#
# Output: installers\white-sound-<version>-linux-<arch>.tar.gz
#
# WSL must already be enabled (this needs a one-time reboot after the first
# `wsl --install`). Set WS_WSL_DISTRO to use a distro other than Ubuntu.
$ErrorActionPreference = "Stop"

$Here   = $PSScriptRoot
$Root   = Split-Path -Parent (Split-Path -Parent $Here)
$OutDir = Join-Path $Root "installers"
$Distro = if ($env:WS_WSL_DISTRO) { $env:WS_WSL_DISTRO } else { "Ubuntu" }

# Make sure the distro is registered.
wsl -d $Distro -u root -- true *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installing the $Distro distribution (one-time)..."
    wsl --install -d $Distro --no-launch
    if ($LASTEXITCODE -ne 0) {
        throw "could not install $Distro; a reboot may be required after the first WSL setup"
    }
}

$wslRoot = (wsl -d $Distro -u root -- wslpath -a "$Root").Trim()
Write-Host "Building Linux installer inside WSL from $wslRoot"

wsl -d $Distro -u root -- bash -lc "sh '$wslRoot/installers/linux/wsl_build.sh' '$wslRoot'"
if ($LASTEXITCODE -ne 0) { throw "Linux build failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "Done. Installer(s) in $OutDir`:"
Get-ChildItem $OutDir -Filter "white-sound-*-linux-*.tar.gz" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 Name, Length, LastWriteTime
