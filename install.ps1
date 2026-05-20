$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "build\x64\Release\Deconnector.exe"

if (-not (Test-Path $exe)) {
    throw "Release exe not found. Run .\build.ps1 first."
}

$installDir = Join-Path $env:LOCALAPPDATA "Programs\Deconnector"
New-Item -ItemType Directory -Force -Path $installDir | Out-Null

$targetExe = Join-Path $installDir "Deconnector.exe"
Copy-Item $exe $targetExe -Force

$startMenuDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\Deconnector"
New-Item -ItemType Directory -Force -Path $startMenuDir | Out-Null

$shortcutPath = Join-Path $startMenuDir "Deconnector.lnk"
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $targetExe
$shortcut.WorkingDirectory = $installDir
$shortcut.Description = "Temporarily disconnect a selected process"
$shortcut.Save()

Write-Host "Installed to: $targetExe"
Write-Host "Shortcut: $shortcutPath"
