<#
.SYNOPSIS
    Универсальная сборка плагина из шаблона. Имя плагина = имя папки.

.EXAMPLE
    .\build.ps1             # собрать
    .\build.ps1 -Install    # собрать и установить в OBS
#>
param(
    [switch]$Install,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\_shared\scripts\obs-build-tools.ps1")

$here = $PSScriptRoot
$name = (Split-Path $here -Leaf).TrimStart("_")
$build = Join-Path $here "build"

Initialize-ObsSdk
$cmake = Find-CMake

Write-Step "Сборка $name ($Config)"
& $cmake -S "$here" -B "$build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "Ошибка настройки CMake" }
& $cmake --build "$build" --config $Config -- /m /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Ошибка сборки" }
$dll = Join-Path $build "$Config\$name.dll"
Write-Ok "Собрано: $dll"

if ($Install) {
    $dest = Join-Path (Get-ObsPluginsDir) $name
    New-Item -ItemType Directory -Force (Join-Path $dest "bin\64bit"), (Join-Path $dest "data") | Out-Null
    Copy-Item $dll (Join-Path $dest "bin\64bit\") -Force
    Copy-Item -Recurse -Force (Join-Path $here "data\*") (Join-Path $dest "data")
    Write-Ok "Установлено в $dest"
    if (Test-ObsRunning) { Write-Warn "Перезапустите OBS, чтобы плагин появился." }
}
