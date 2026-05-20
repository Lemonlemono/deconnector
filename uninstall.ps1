$ErrorActionPreference = "Stop"

$installDir = Join-Path $env:LOCALAPPDATA "Programs\Deconnector"
$startMenuDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\Deconnector"

if (Test-Path $installDir) {
    Remove-Item -LiteralPath $installDir -Recurse -Force
}

if (Test-Path $startMenuDir) {
    Remove-Item -LiteralPath $startMenuDir -Recurse -Force
}

Write-Host "Deconnector was removed."
