@echo off
rem B2recomp builder package: "Build B2recomp.bat" at the top of the package (scripts/make_package.py copies this file there).
rem Builds the game on this PC from YOUR extracted dump with the tools in deps\ (nothing else to install), then starts
rem the launcher. Drag your dump folder onto this file, or start it and type / paste the folder.
setlocal
cd /d "%~dp0"
title B2recomp builder

rem only the bundled tools: no Python or compiler settings of this PC may leak in
set PYTHONHOME=
set PYTHONPATH=
set PYTHONSTARTUP=
set PYTHONNOUSERSITE=1
set PYTHONUTF8=1
set LLVM_MINGW=
set LLVM_ROOT=
set SDL3_ROOT=
set VULKAN_HEADERS=
set GLSLANG_ROOT=
set GLSLANG_DLL=
set TOOLCHAIN=

set "PY=%~dp0deps\python\python.exe"
if not exist "%PY%" (
    echo This folder is not a complete B2recomp builder package: deps\python\python.exe is missing.
    goto :fail
)

echo B2recomp builder
echo ================
echo This builds the game on this PC from your own extracted copy of Bayonetta 2, version 1.2 (update v131072):
echo a folder with exefs\ and romfs\ inside (see BUILDING.md). Nothing is downloaded. It takes about 5 to 15 minutes.
echo.

set "DUMP=%~1"
if "%DUMP%"=="" if exist "build\setup_state.json" (
    echo Using the dump folder of the last build. To use another one, drag it onto this file.
    "%PY%" "%~dp0scripts\setup.py"
    goto :done
)
if "%DUMP%"=="" set /p "DUMP=Your dump folder (drag it into this window, then press Enter): "
set "DUMP=%DUMP:"=%"
if "%DUMP%"=="" (
    echo No folder given.
    goto :fail
)
"%PY%" "%~dp0scripts\setup.py" --dump "%DUMP%"

:done
if errorlevel 1 goto :fail
echo.
echo Built. Starting the launcher (next time, start build\launcher.exe directly).
start "" "%~dp0build\launcher.exe"
pause
exit /b 0

:fail
echo.
echo The build stopped. The message above says why; fix it and start this file again (finished steps are kept).
pause
exit /b 1
