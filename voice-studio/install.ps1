<#
.SYNOPSIS
    Устанавливает «Голосовую студию» в OBS Studio. / Installs Voice Studio into OBS Studio.

.DESCRIPTION
    Копирует плагин в %ProgramData%\obs-studio\plugins\voice-studio —
    это стандартная папка для сторонних плагинов OBS 28 и новее (права администратора не нужны).
    Работает и из папки проекта (после сборки), и из распакованного архива.
#>
param([string]$Config = "Release")

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot

# Откуда брать файлы: из сборки или из архива (папка voice-studio рядом со скриптом)
$fromBuild = Join-Path $here "build\$Config\voice-studio.dll"
$fromZip = Join-Path $here "voice-studio\bin\64bit\voice-studio.dll"
if (Test-Path $fromZip) {
    $dll = $fromZip
    $data = Join-Path $here "voice-studio\data"
} elseif (Test-Path $fromBuild) {
    $dll = $fromBuild
    $data = Join-Path $here "data"
} else {
    throw "voice-studio.dll не найден / not found. Сначала соберите плагин: .\build.ps1"
}

$dest = Join-Path $env:ProgramData "obs-studio\plugins\voice-studio"
Write-Host "==> Устанавливаю / Installing: $dest" -ForegroundColor Cyan
New-Item -ItemType Directory -Force (Join-Path $dest "bin\64bit"), (Join-Path $dest "data") | Out-Null

$binDir = Join-Path $dest "bin\64bit"
$target = Join-Path $binDir "voice-studio.dll"
# Остатки прошлых обновлений «на горячую» (удалятся, когда OBS их отпустит).
Get-ChildItem $binDir -Filter "voice-studio.dll.old-*" -ErrorAction SilentlyContinue |
    ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch {} }

$hotSwapped = $false
try {
    Copy-Item $dll $target -Force -ErrorAction Stop
} catch {
    # OBS открыт и держит файл. Загруженную DLL Windows разрешает переименовать —
    # кладём новую рядом, она включится при следующем запуске OBS.
    try {
        Rename-Item $target ("voice-studio.dll.old-" + (Get-Date -Format "yyyyMMddHHmmss")) -ErrorAction Stop
        Copy-Item $dll $target -Force -ErrorAction Stop
        $hotSwapped = $true
    } catch {
        throw "Не удалось записать плагин. Закройте OBS и запустите установку снова. / Close OBS and run the installer again."
    }
}
$pdb = [System.IO.Path]::ChangeExtension($dll, ".pdb")
if (Test-Path $pdb) { try { Copy-Item $pdb $binDir -Force -ErrorAction Stop } catch {} }
Copy-Item -Recurse -Force (Join-Path $data "*") (Join-Path $dest "data")

Write-Host "OK  Плагин установлен / Plugin installed." -ForegroundColor Green
if ($hotSwapped -or (Get-Process obs64 -ErrorAction SilentlyContinue)) {
    Write-Host "!!  OBS открыт — перезапустите его / OBS is running — restart it." -ForegroundColor Yellow
}
Write-Host ""
Write-Host "RU: Микшер аудио -> ⋮ у микрофона -> Фильтры -> + -> 🎙 Голосовая студия."
Write-Host "EN: Audio Mixer -> ⋮ next to the mic -> Filters -> + -> 🎙 Voice Studio."
Write-Host "Обновления приходят сами / Updates arrive automatically (Tools -> 🎙 Voice Studio)."
