<#
.SYNOPSIS
    Готовит "SDK" OBS для сборки плагинов без сборки самого OBS Studio.

.DESCRIPTION
    1. Находит установленный OBS Studio и определяет его версию.
    2. Скачивает заголовки libobs и obs-frontend-api ровно этой версии (только нужные папки).
    3. Создаёт библиотеки импорта obs.lib и obs-frontend-api.lib из установленных DLL
       (dumpbin + lib из Visual Studio).
    4. С ключом -WithQt скачивает тот же Qt, на котором собран OBS (официальный пакет
       obs-deps, сверяется по SHA-256) — нужен плагинам со своим интерфейсом.
    5. Записывает _sdk/current.cmake, который подхватывают все плагины.

    Скрипт общий для всех плагинов в папке "Плагины OBS".
    Запускать заново нужно только после обновления OBS (с ключом -Force).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File _shared\scripts\setup-obs-sdk.ps1 -WithQt
#>
param(
    [string]$ObsDir = "",
    [string]$Version = "",
    [switch]$WithQt,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$sdkRoot = Join-Path $root "_sdk"
New-Item -ItemType Directory -Force $sdkRoot | Out-Null

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

# --- 1. OBS Studio ------------------------------------------------------------
if (-not $ObsDir) {
    $candidates = @()
    try { $candidates += (Get-ItemProperty "HKLM:\SOFTWARE\OBS Studio" -ErrorAction Stop).'(default)' } catch {}
    $candidates += "$env:ProgramFiles\obs-studio", "${env:ProgramFiles(x86)}\obs-studio"
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c "bin\64bit\obs.dll"))) { $ObsDir = $c; break }
    }
}
if (-not $ObsDir -or -not (Test-Path (Join-Path $ObsDir "bin\64bit\obs.dll"))) {
    throw "OBS Studio не найден. Укажите путь вручную: -ObsDir 'C:\Program Files\obs-studio'"
}
$obsBin = Join-Path $ObsDir "bin\64bit"

if (-not $Version) {
    $raw = (Get-Item (Join-Path $obsBin "obs64.exe")).VersionInfo.ProductVersion
    $Version = ([regex]::Match($raw, "^\d+\.\d+\.\d+")).Value
}
if (-not $Version) { throw "Не удалось определить версию OBS. Укажите её: -Version 32.2.2" }
Write-Step "OBS Studio ${Version}: $ObsDir"

# --- 2. Заголовки libobs и obs-frontend-api -------------------------------------
$srcDir = Join-Path $sdkRoot "obs-studio-$Version"
if ($Force -and (Test-Path $srcDir)) { Remove-Item -Recurse -Force $srcDir }
if (-not (Test-Path (Join-Path $srcDir "libobs\obs-module.h"))) {
    Write-Step "Скачиваю заголовки OBS $Version с GitHub"
    git -c advice.detachedHead=false clone --depth 1 --branch $Version --filter=blob:none --sparse `
        https://github.com/obsproject/obs-studio.git "$srcDir"
    if ($LASTEXITCODE -ne 0) { throw "git clone завершился с ошибкой" }
    git -C "$srcDir" sparse-checkout set libobs frontend/api
    if ($LASTEXITCODE -ne 0) { throw "git sparse-checkout завершился с ошибкой" }
} elseif (-not (Test-Path (Join-Path $srcDir "frontend\api\obs-frontend-api.h"))) {
    Write-Step "Докачиваю заголовки obs-frontend-api"
    git -C "$srcDir" sparse-checkout add frontend/api
    if ($LASTEXITCODE -ne 0) { throw "git sparse-checkout завершился с ошибкой" }
} else {
    Write-Step "Заголовки OBS уже скачаны"
}

# --- 3. obsconfig.h и библиотеки импорта ------------------------------------------
$outDir = Join-Path $sdkRoot "obs-$Version"
New-Item -ItemType Directory -Force (Join-Path $outDir "include"), (Join-Path $outDir "lib") | Out-Null

# obsconfig.h генерируется при сборке OBS; плагинам нужны только эти определения.
@"
#pragma once

#define OBS_DATA_PATH "../../data"
#define OBS_PLUGIN_PATH "../../obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
"@ | Set-Content -Encoding ascii (Join-Path $outDir "include\obsconfig.h")

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Не найден Visual Studio (vswhere.exe). Установите Visual Studio 2022 с компонентом C++." }
$libExe = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find "VC\Tools\MSVC\**\bin\Hostx64\x64\lib.exe" | Select-Object -Last 1
if (-not $libExe) { throw "Не найден lib.exe. Установите в Visual Studio компонент 'Разработка классических приложений на C++'." }
$dumpbin = Join-Path (Split-Path $libExe) "dumpbin.exe"

function New-ImportLib([string]$dllName, [int]$minExports) {
    $dll = Join-Path $obsBin $dllName
    $base = [System.IO.Path]::GetFileNameWithoutExtension($dllName)
    Write-Step "Создаю $base.lib из $dll"
    $exports = & $dumpbin /nologo /exports "$dll"
    $names = @(foreach ($line in $exports) {
        $m = [regex]::Match($line, "^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+([A-Za-z_][A-Za-z0-9_@?$]*)")
        if ($m.Success) { $m.Groups[1].Value }
    })
    if ($names.Count -lt $minExports) { throw "Не удалось прочитать экспорт $dllName (найдено: $($names.Count))" }
    $defPath = Join-Path $outDir "lib\$base.def"
    (@("LIBRARY $dllName", "EXPORTS") + ($names | ForEach-Object { "    $_" })) | Set-Content -Encoding ascii $defPath
    & $libExe /nologo /def:"$defPath" /machine:x64 /out:"$(Join-Path $outDir "lib\$base.lib")" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib.exe завершился с ошибкой для $dllName" }
    return $names.Count
}
$obsCount = New-ImportLib "obs.dll" 100
$frontendCount = New-ImportLib "obs-frontend-api.dll" 20

# --- 4. Qt (по желанию) -----------------------------------------------------------
$qtDir = ""
$existingQt = Get-ChildItem $sdkRoot -Directory -Filter "qt6-*" -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "lib\cmake\Qt6\Qt6Config.cmake") } | Select-Object -First 1
if ($WithQt) {
    # Версия и SHA-256 пакета Qt — из CMakePresets.json той же версии OBS.
    $presets = (git -C "$srcDir" show HEAD:CMakePresets.json) -join "`n" | ConvertFrom-Json
    $deps = ($presets.configurePresets | Where-Object { $_.name -eq "dependencies" }).vendor.'obsproject.com/obs-studio'.dependencies
    $qtVersion = $deps.qt6.version
    $qtHash = $deps.qt6.hashes.'windows-x64'
    if (-not $qtVersion -or -not $qtHash) { throw "Не удалось узнать версию Qt для OBS $Version" }
    $qtDir = Join-Path $sdkRoot "qt6-$qtVersion"

    if (-not (Test-Path (Join-Path $qtDir "lib\cmake\Qt6\Qt6Config.cmake"))) {
        $file = "windows-deps-qt6-$qtVersion-x64.zip"
        $url = "$($deps.qt6.baseUrl)/$qtVersion/$file"
        $downloads = Join-Path $sdkRoot "downloads"
        New-Item -ItemType Directory -Force $downloads | Out-Null
        $zip = Join-Path $downloads $file
        if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $qtHash.ToUpper()) {
            Write-Step "Скачиваю Qt для OBS ($file, ~250 МБ)"
            Invoke-WebRequest -Uri $url -OutFile "$zip.tmp" -UseBasicParsing
            Move-Item -Force "$zip.tmp" $zip
        }
        $actual = (Get-FileHash $zip -Algorithm SHA256).Hash
        if ($actual -ne $qtHash.ToUpper()) {
            Remove-Item -Force $zip
            throw "Контрольная сумма Qt не совпала (ожидалась $qtHash, получена $actual)"
        }
        Write-Step "Распаковываю Qt в $qtDir"
        if (Test-Path $qtDir) { Remove-Item -Recurse -Force $qtDir }
        New-Item -ItemType Directory -Force $qtDir | Out-Null
        tar -xf "$zip" -C "$qtDir"
        if ($LASTEXITCODE -ne 0) { throw "Не удалось распаковать Qt" }
    } else {
        Write-Step "Qt уже подготовлен: $qtDir"
    }
} elseif ($existingQt) {
    $qtDir = $existingQt.FullName
}

# --- 5. current.cmake -----------------------------------------------------------
$cmakeObsDir = $ObsDir -replace "\\", "/"
$lines = @(
    "# Создано _shared/scripts/setup-obs-sdk.ps1 — не редактируйте вручную.",
    "set(OBS_SDK_VERSION `"$Version`")",
    "set(OBS_INSTALL_DIR `"$cmakeObsDir`")"
)
if ($qtDir) { $lines += "set(OBS_QT6_DIR `"$($qtDir -replace '\\', '/')`")" }
$lines | Set-Content -Encoding utf8 (Join-Path $sdkRoot "current.cmake")

Write-Step "Готово: libobs $obsCount функций, frontend-api $frontendCount функций$(if ($qtDir) { ', Qt: ' + $qtDir })"
