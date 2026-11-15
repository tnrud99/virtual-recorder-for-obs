@echo off
setlocal EnableExtensions

title Install Virtual Recorder for OBS

set "PLUGIN_NAME=virtuedit-obs"
set "SCRIPT_DIR=%~dp0"
set "SOURCE_DIR=%SCRIPT_DIR%%PLUGIN_NAME%"
set "DEST_ROOT=%ProgramData%\obs-studio\plugins"
set "DEST_DIR=%DEST_ROOT%\%PLUGIN_NAME%"

net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Requesting administrator permission...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"
    exit /b
)

echo.
echo Virtual Recorder for OBS - Windows Installer
echo ===========================================
echo.

if not exist "%SOURCE_DIR%\bin\64bit\virtuedit-obs.dll" (
    echo ERROR: Could not find "%SOURCE_DIR%\bin\64bit\virtuedit-obs.dll".
    echo.
    echo Extract the ZIP first, then run Install-Windows.bat from the extracted folder.
    echo.
    pause
    exit /b 1
)

tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if "%errorlevel%"=="0" (
    echo ERROR: OBS Studio is currently running.
    echo.
    echo Close OBS completely, then run this installer again.
    echo.
    pause
    exit /b 1
)

tasklist /FI "IMAGENAME eq obs.exe" 2>nul | find /I "obs.exe" >nul
if "%errorlevel%"=="0" (
    echo ERROR: OBS Studio is currently running.
    echo.
    echo Close OBS completely, then run this installer again.
    echo.
    pause
    exit /b 1
)

echo Installing to:
echo   %DEST_DIR%
echo.

if not exist "%DEST_ROOT%" (
    mkdir "%DEST_ROOT%" >nul 2>&1
    if not "%errorlevel%"=="0" (
        echo ERROR: Could not create "%DEST_ROOT%".
        echo.
        pause
        exit /b 1
    )
)

if exist "%DEST_DIR%" (
    echo Updating existing installation...
    rmdir /S /Q "%DEST_DIR%" >nul 2>&1
    if exist "%DEST_DIR%" (
        echo ERROR: Could not replace the existing plugin folder.
        echo Close OBS completely and check folder permissions, then try again.
        echo.
        pause
        exit /b 1
    )
)

robocopy "%SOURCE_DIR%" "%DEST_DIR%" /MIR /NFL /NDL /NJH /NJS /NP >nul
set "ROBOCOPY_EXIT=%errorlevel%"
if %ROBOCOPY_EXIT% GEQ 8 (
    echo ERROR: File copy failed. Robocopy exit code: %ROBOCOPY_EXIT%
    echo.
    pause
    exit /b 1
)

echo Installation complete.
echo.
echo Open OBS Studio. Virtual Recorder should appear in OBS.
echo If it is not visible, check the OBS Docks or Tools menu.
echo.
pause
exit /b 0
