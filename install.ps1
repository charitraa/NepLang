# NepLang installer for Windows (PowerShell).
#
#   irm https://raw.githubusercontent.com/charitraa/NepLang/main/install.ps1 | iex
#
# Downloads the prebuilt neplang.exe from the latest GitHub Release into
# %LOCALAPPDATA%\Programs\neplang and adds that folder to your user PATH.
# Set $env:NEPLANG_VERSION (e.g. "v0.5.0") to install a specific release.
$ErrorActionPreference = "Stop"

$Repo = "charitraa/NepLang"
$Asset = "neplang-windows-x86_64.exe"
$InstallDir = Join-Path $env:LOCALAPPDATA "Programs\neplang"
$Version = if ($env:NEPLANG_VERSION) { $env:NEPLANG_VERSION } else { "latest" }

if ($Version -eq "latest") {
    $Url = "https://github.com/$Repo/releases/latest/download/$Asset"
} else {
    $Url = "https://github.com/$Repo/releases/download/$Version/$Asset"
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$Target = Join-Path $InstallDir "neplang.exe"

Write-Host "Downloading $Asset ..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $Url -OutFile $Target -UseBasicParsing

$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not $UserPath) { $UserPath = "" }
if (($UserPath -split ";") -notcontains $InstallDir) {
    $NewPath = if ($UserPath) { "$UserPath;$InstallDir" } else { $InstallDir }
    [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    Write-Host "Added $InstallDir to your PATH."
}
$env:Path = "$env:Path;$InstallDir"

Write-Host "Installed neplang to $Target"
Write-Host "Open a new terminal and run:  neplang yourfile.np"
