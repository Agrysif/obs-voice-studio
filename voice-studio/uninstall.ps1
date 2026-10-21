<#
.SYNOPSIS
    Удаляет «Голосовую студию» из OBS Studio. / Removes Voice Studio from OBS Studio.
    Фильтры в сценах останутся в настройках, но перестанут работать, пока плагин не установлен снова.
#>
$ErrorActionPreference = "Stop"
$dest = Join-Path $env:ProgramData "obs-studio\plugins\voice-studio"
if (-not (Test-Path $dest)) {
    Write-Host "Плагин не установлен / Plugin is not installed." -ForegroundColor Yellow
    return
}
if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "Закройте OBS перед удалением / Close OBS first."
}
Remove-Item -Recurse -Force $dest
Write-Host "OK  Плагин удалён / Plugin removed: $dest" -ForegroundColor Green
