@echo off
:: RUN_ME.cmd - double-click to install the Fonix Jessica SAPI5 voice
:: Self-elevates via UAC, bypasses ExecutionPolicy, keeps window open on error.

net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

pushd "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
set RC=%errorlevel%
popd

echo.
echo Installer exited with code %RC%.
pause
