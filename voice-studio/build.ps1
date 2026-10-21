<#
.SYNOPSIS
    Сборка плагина «Голосовая студия».

.EXAMPLE
    .\build.ps1                 # только собрать
    .\build.ps1 -Test           # собрать и прогнать проверки
    .\build.ps1 -Install        # собрать и установить в OBS
    .\build.ps1 -Package        # собрать zip для переноса на другой компьютер
#>
param(
    [switch]$Install,
    [switch]$Test,
    [switch]$Package,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8 # тесты пишут по-русски в UTF-8
. (Join-Path $PSScriptRoot "..\_shared\scripts\obs-build-tools.ps1")

$here = $PSScriptRoot
$build = Join-Path $here "build"

Write-Step "Подготовка SDK OBS (заголовки, Qt)"
Initialize-ObsSdk -WithQt

$cmake = Find-CMake
Write-Step "Настройка проекта (CMake)"
& $cmake -S "$here" -B "$build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "Ошибка настройки CMake" }

Write-Step "Сборка ($Config)"
& $cmake --build "$build" --config $Config -- /m /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Ошибка сборки" }
$dll = Join-Path $build "$Config\voice-studio.dll"
Write-Ok "Плагин собран: $dll"

if ($Test) {
    Write-Step "Самопроверка DSP"
    & (Join-Path $build "$Config\voice-studio-test.exe") selftest
    if ($LASTEXITCODE -ne 0) { throw "Самопроверка не пройдена" }

    Write-Step "Самопроверка автообновления (подпись, пакет, установка)"
    & (Join-Path $build "$Config\voice-studio-update-tool.exe") selftest
    if ($LASTEXITCODE -ne 0) { throw "Самопроверка обновлений не пройдена" }

    $wav = Join-Path $build "test-speech.wav"
    if (-not (Test-Path $wav)) {
        # Тестовая речь от встроенного синтезатора Windows.
        Add-Type -AssemblyName System.Speech
        $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
        $ru = $synth.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Culture.Name -like "ru*" } | Select-Object -First 1
        if ($ru) { $synth.SelectVoice($ru.VoiceInfo.Name) }
        $fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000,
            [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
        $synth.SetOutputToWaveFile($wav, $fmt)
        $synth.Speak("Привет! Это проверка плагина. Я говорю обычным голосом и делаю паузы между фразами. Шипящие звуки: сосиски, шишки, щука. Раз, два, три, четыре, пять. Голос должен звучать чисто и разборчиво.")
        $synth.Dispose()
    }

    Write-Step "Проверка внутри libobs (без окна OBS и без звука)"
    $obsBin = Get-ObsBinDir
    if (-not $obsBin) { throw "Не найден установленный OBS Studio" }
    $oldPath = $env:PATH
    $env:PATH = "$obsBin;$env:PATH"
    Push-Location $obsBin
    try {
        & (Join-Path $build "$Config\voice-studio-obs-test.exe") "$dll" (Join-Path $here "data") "$wav" `
            (Join-Path $build "panel-test.png") | Select-String -Pattern "^\[|OBS TEST" | ForEach-Object { $_.Line }
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
        $env:PATH = $oldPath
    }
    if ($code -ne 0) { throw "Проверка в libobs не пройдена" }
    Write-Ok "Все проверки пройдены"
}

if ($Package) {
    Write-Step "Упаковка"
    $version = (Select-String -Path (Join-Path $here "CMakeLists.txt") -Pattern "project\(voice-studio VERSION ([0-9.]+)").Matches[0].Groups[1].Value
    $stage = Join-Path $here "dist\stage"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force "$stage\voice-studio\bin\64bit" | Out-Null
    Copy-Item $dll "$stage\voice-studio\bin\64bit\"
    Copy-Item -Recurse (Join-Path $here "data") "$stage\voice-studio\data"
    # Описание для пользователей и лицензия — из корня репозитория.
    foreach ($f in "README.md", "README.ru.md", "LICENSE") {
        Copy-Item (Join-Path $here "..\$f") "$stage\"
    }
    foreach ($f in "install.cmd", "install.ps1", "uninstall.cmd", "uninstall.ps1") {
        Copy-Item (Join-Path $here $f) "$stage\"
    }
    $zip = Join-Path $here "dist\voice-studio-$version-windows-x64.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Remove-Item -Recurse -Force $stage
    Write-Ok "Архив: $zip"
}

if ($Install) {
    & (Join-Path $here "install.ps1") -Config $Config
}
