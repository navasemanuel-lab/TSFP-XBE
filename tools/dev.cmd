@echo off
rem Developer build: the toolchain environment, then an incremental cmake build of the release preset.
rem   tools\dev.cmd            build
rem   tools\dev.cmd run [s]    build, then run the game for s seconds (default 40) and print the log's tail
setlocal
cd /d "%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%ProgramFiles%\Git\cmd;%PATH%"
cmake --build out\build\release || exit /b 1
if /i not "%1"=="run" exit /b 0
set "SECS=%2"
if not defined SECS set SECS=40
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run.ps1 -Seconds %SECS%
