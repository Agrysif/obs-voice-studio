<#
.SYNOPSIS
    Выпуск новой версии «Голосовой студии» на GitHub с автообновлением.

.DESCRIPTION
    1. Собирает плагин и прогоняет все проверки (build.ps1 -Test -Package).
    2. Собирает пакет обновления *-update.bin (библиотека + переводы).
    3. Пишет update.txt (версия, хеш пакета, что нового) и подписывает его
       секретным ключом разработчика (ECDSA P-256) -> update.txt.sig.
    4. Проверяет подпись тем же кодом, что работает в плагине.
    5. Публикует выпуск v<версия> на GitHub (gh) и проверяет его «глазами» плагина.

    Что нового пишется в releases/<версия>/:
        notes.ru.txt, notes.en.txt — коротко, для окна «Доступна новая версия»;
        github.md                  — полное описание выпуска на GitHub (RU + EN).

    Секретный ключ лежит вне репозитория: %USERPROFILE%\.voice-studio\signing-key.pem.
    Без него установленные плагины не примут обновление — храните резервную копию.

.EXAMPLE
    pwsh .\release.ps1 -DryRun     # всё, кроме публикации на GitHub
    pwsh .\release.ps1             # выпуск
#>
param(
    [switch]$DryRun,
    [switch]$SkipBuild,
    [string]$KeyPath = (Join-Path $env:USERPROFILE ".voice-studio\signing-key.pem"),
    [string]$Repo = ""
)

$ErrorActionPreference = "Stop"
if ($PSVersionTable.PSVersion.Major -lt 7) {
    # Подпись в нужном формате (ECDSA r||s) и чтение PEM-ключа умеет .NET из PowerShell 7.
    throw "Запустите через PowerShell 7: pwsh .\release.ps1 (установка: winget install Microsoft.PowerShell)"
}
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
. (Join-Path $PSScriptRoot "..\_shared\scripts\obs-build-tools.ps1")

$here = $PSScriptRoot
$cmakeLists = Get-Content -Raw (Join-Path $here "CMakeLists.txt")
$version = [regex]::Match($cmakeLists, "project\(voice-studio VERSION ([0-9.]+)").Groups[1].Value
if (-not $Repo) { $Repo = [regex]::Match($cmakeLists, 'VOICE_STUDIO_UPDATE_REPO "([^"]+)"').Groups[1].Value }
if (-not $version -or -not $Repo) { throw "Не удалось прочитать версию или репозиторий из CMakeLists.txt" }
$tag = "v$version"
$notesDir = Join-Path $here "releases\$version"
$dist = Join-Path $here "dist"
$tool = Join-Path $here "build\Release\voice-studio-update-tool.exe"

Write-Step "Выпуск $tag в $Repo"
foreach ($f in "notes.ru.txt", "notes.en.txt", "github.md") {
    if (-not (Test-Path (Join-Path $notesDir $f))) { throw "Нет файла releases\$version\$f — опишите, что нового" }
}
if (-not (Test-Path $KeyPath)) { throw "Нет секретного ключа подписи: $KeyPath" }

function Find-Gh {
    $cmd = Get-Command gh -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $local = Join-Path $here "..\_sdk\tools\gh\bin\gh.exe"
    if (Test-Path $local) { return (Resolve-Path $local).Path }
    throw "Не найден GitHub CLI (gh). Установите: winget install GitHub.cli"
}
$gh = if ($DryRun) { $null } else { Find-Gh }
if ($gh) {
    & $gh release view $tag --repo $Repo *> $null
    if ($LASTEXITCODE -eq 0) { throw "Выпуск $tag уже есть на GitHub. Поднимите версию в CMakeLists.txt." }
}

# --- 1. Сборка и проверки
if (-not $SkipBuild) {
    & (Join-Path $here "build.ps1") -Test -Package # при ошибке остановится сам
}
$zip = Join-Path $dist "voice-studio-$version-windows-x64.zip"
if (-not (Test-Path $zip)) { throw "Нет архива $zip" }

# --- 2. Пакет обновления
Write-Step "Пакет обновления"
$stage = Join-Path $dist "update-stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage "bin\64bit") | Out-Null
Copy-Item (Join-Path $here "build\Release\voice-studio.dll") (Join-Path $stage "bin\64bit\")
Copy-Item -Recurse (Join-Path $here "data") (Join-Path $stage "data")
$paths = Get-ChildItem -Recurse -File $stage | ForEach-Object {
    [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/')
}
$bundleName = "voice-studio-$version-update.bin"
$bundle = Join-Path $dist $bundleName
$packOut = & $tool pack $bundle $stage @paths
if ($LASTEXITCODE -ne 0) { throw "Не удалось собрать пакет: $packOut" }
Remove-Item -Recurse -Force $stage
$bundleSize = (Get-Item $bundle).Length
$bundleSha = (Get-FileHash -Algorithm SHA256 $bundle).Hash.ToLowerInvariant()
Write-Ok "$bundleName — $([math]::Round($bundleSize / 1MB, 2)) МБ, файлов: $($paths.Count)"

# --- 3. Описание версии и подпись
Write-Step "Описание версии и подпись"
function Get-Notes($file) {
    $t = (Get-Content -Raw -Encoding UTF8 (Join-Path $notesDir $file)).Trim()
    return $t.Replace("`r`n", "`n").Replace("`n", "\n")
}
$manifest = @(
    "name=voice-studio"
    "version=$version"
    "obs_min=32.0.0"
    "obs_max_major=32"
    "bundle=$bundleName"
    "bundle_sha256=$bundleSha"
    "bundle_size=$bundleSize"
    "page=https://github.com/$Repo/releases/tag/$tag"
    "notes_ru=$(Get-Notes 'notes.ru.txt')"
    "notes_en=$(Get-Notes 'notes.en.txt')"
) -join "`n"
$manifest += "`n"
$manifestPath = Join-Path $dist "update.txt"
$manifestBytes = [Text.UTF8Encoding]::new($false).GetBytes($manifest)
[IO.File]::WriteAllBytes($manifestPath, $manifestBytes)

$ecdsa = [System.Security.Cryptography.ECDsa]::Create()
try {
    $ecdsa.ImportFromPem((Get-Content -Raw $KeyPath))
    # SignData в .NET отдаёт подпись r||s (64 байта) — тот формат, что ждёт плагин.
    $sig = $ecdsa.SignData($manifestBytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)
} finally {
    $ecdsa.Dispose()
}
$sigPath = Join-Path $dist "update.txt.sig"
[IO.File]::WriteAllText($sigPath, [Convert]::ToBase64String($sig))

& $tool verify $manifestPath $sigPath
if ($LASTEXITCODE -ne 0) { throw "Подпись не принимается ключом, вшитым в плагин! Ключ не тот?" }
Write-Ok "Подпись верна"

$assets = @($zip, $bundle, $manifestPath, $sigPath)
if ($DryRun) {
    Write-Ok "Проверочный прогон закончен, на GitHub ничего не отправлено. Файлы:"
    $assets | ForEach-Object { Write-Host "    $_" }
    return
}

# --- 4. Публикация
Write-Step "Публикация на GitHub"
& $gh release create $tag @assets --repo $Repo --title "Voice Studio $version" `
    --notes-file (Join-Path $notesDir "github.md") --latest
if ($LASTEXITCODE -ne 0) { throw "Не удалось создать выпуск на GitHub" }

Write-Step "Проверка выпуска так, как его видит плагин"
& $tool online $Repo
if ($LASTEXITCODE -ne 0) { throw "Плагин не сможет обновиться с этого выпуска — проверьте файлы на GitHub" }
Write-Ok "Готово: https://github.com/$Repo/releases/tag/$tag"
