@echo off
setlocal EnableExtensions

title Uninstall Virtual Recorder for OBS

set "PLUGIN_NAME=virtuedit-obs"
set "DEST_DIR=%ProgramData%\obs-studio\plugins\%PLUGIN_NAME%"

net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Requesting administrator permission...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"
    exit /b
)

echo.
echo Virtual Recorder for OBS - Windows Uninstaller
echo =============================================
echo.

tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if "%errorlevel%"=="0" (
    echo ERROR: OBS Studio is currently running.
    echo.
    echo Close OBS completely, then run this uninstaller again.
    echo.
    pause
    exit /b 1
)

tasklist /FI "IMAGENAME eq obs.exe" 2>nul | find /I "obs.exe" >nul
if "%errorlevel%"=="0" (
    echo ERROR: OBS Studio is currently running.
    echo.
    echo Close OBS completely, then run this uninstaller again.
    echo.
    pause
    exit /b 1
)

if not exist "%DEST_DIR%" (
    echo Virtual Recorder is not installed at:
    echo   %DEST_DIR%
    echo.
    pause
    exit /b 0
)

echo Removing:
echo   %DEST_DIR%
echo.

rmdir /S /Q "%DEST_DIR%" >nul 2>&1
if exist "%DEST_DIR%" (
    echo ERROR: Could not remove the plugin folder.
    echo Check folder permissions, then try again.
    echo.
    pause
    exit /b 1
)

echo Uninstall complete. Your recordings were not removed.
echo.
echo Press any key to close this window.
pause >nul
exit /b 0
