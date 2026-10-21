@echo off
rem Double-click: build the plugin, run the checks and install it into OBS.
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Test -Install
echo.
pause
