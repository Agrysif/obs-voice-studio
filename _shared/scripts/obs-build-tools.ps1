# Общие функции для скриптов сборки плагинов. Подключение:
#     . (Join-Path $PSScriptRoot "..\_shared\scripts\obs-build-tools.ps1")

# Самый свежий CMake из PATH и всех установленных Visual Studio
# (генератор "Visual Studio 17 2022" есть только в CMake 3.21+).
function Find-CMake {
    $candidates = @()
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $candidates += & $vswhere -products * -find "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    }
    $best = $null
    $bestVersion = [version]"0.0"
    foreach ($c in $candidates) {
        $m = [regex]::Match(((& $c --version) | Select-Object -First 1), "(\d+\.\d+(\.\d+)?)")
        if ($m.Success -and [version]$m.Value -gt $bestVersion) {
            $best = $c
            $bestVersion = [version]$m.Value
        }
    }
    if (-not $best -or $bestVersion -lt [version]"3.21") {
        throw "Не найден CMake 3.21+. Установите Visual Studio 2022 с компонентом 'Разработка классических приложений на C++' (CMake входит в него)."
    }
    return $best
}

function Get-ObsBinDir {
    $candidates = @()
    try { $candidates += (Get-ItemProperty "HKLM:\SOFTWARE\OBS Studio" -ErrorAction Stop).'(default)' } catch {}
    $candidates += "$env:ProgramFiles\obs-studio", "${env:ProgramFiles(x86)}\obs-studio"
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c "bin\64bit\obs.dll"))) { return (Join-Path $c "bin\64bit") }
    }
    return $null
}

# Папка, где OBS 28+ ищет сторонние плагины (права администратора не нужны).
function Get-ObsPluginsDir {
    return (Join-Path $env:ProgramData "obs-studio\plugins")
}

function Test-ObsRunning {
    return [bool](Get-Process obs64 -ErrorAction SilentlyContinue)
}

# Готовит SDK при первой сборке. -WithQt — плагину нужен свой интерфейс на Qt.
function Initialize-ObsSdk([switch]$WithQt) {
    $root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
    $current = Join-Path $root "_sdk\current.cmake"
    $hasQt = (Test-Path $current) -and (Select-String -Path $current -Pattern "OBS_QT6_DIR" -Quiet)
    if (-not (Test-Path $current) -or ($WithQt -and -not $hasQt)) {
        $setup = Join-Path $root "_shared\scripts\setup-obs-sdk.ps1"
        if ($WithQt) { & $setup -WithQt } else { & $setup }
    }
}

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Write-Ok($text) { Write-Host "OK  $text" -ForegroundColor Green }
function Write-Warn($text) { Write-Host "!!  $text" -ForegroundColor Yellow }
