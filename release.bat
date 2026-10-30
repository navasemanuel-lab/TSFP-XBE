@echo off
rem release.bat         TSFP.zip: the game to play (TSFP.exe, its launcher, the Visual C++ runtime, an empty assets folder)
rem release.bat source  TSFP-source.zip: the project to build, without the game's files or the code made from them
setlocal
set "GAME_EXE=TSFP"
cd /d "%~dp0"
set "OUT=out\build\release"
set "STAGE=%OUT%\package"
if /i "%~1"=="source" goto :source

rem the Visual C++ runtime beside the executable, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo Visual C++ runtime not found: install Visual Studio with the C++ workload & exit /b 1)
if not exist "%OUT%\%GAME_EXE% Launcher.exe" call build.bat || exit /b 1

rd /s /q "%STAGE%" 2>nul
mkdir "%STAGE%\assets"
copy /y "%OUT%\%GAME_EXE%.exe" "%STAGE%\" >nul || exit /b 1
copy /y "%OUT%\%GAME_EXE% Launcher.exe" "%STAGE%\" >nul || exit /b 1
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do copy /y "%VCRUNTIME%\%%f.dll" "%STAGE%\" >nul
rem the launcher's cover and logo (.local/art), and the early HUD's TimeSplitters 2 textures and font made by
rem tools/ts2_pack.py into out/build/release/ts2, and the early skyline's preview files by tools/preview_pack.py into
rem out/build/release/preview: the packer's own, never part of the source
if exist ".local\art\cover.jpg" xcopy /e /i /q /y ".local\art" "%STAGE%\art" >nul
if exist "%OUT%\ts2\data\hud.pak" xcopy /e /i /q /y "%OUT%\ts2" "%STAGE%\ts2" >nul
if exist "%OUT%\preview\PAK\STORY\L_6_ST.PAK" xcopy /e /i /q /y "%OUT%\preview" "%STAGE%\preview" >nul
rem the shaders and pipeline keys of the places played here, made from the game's own shaders: a new player's run
rem makes them ahead (StartPrebuild) instead of stopping for them in play; packed as seeds, so unpacking over an
rem install keeps the player's own files
if exist "%OUT%\shaders.bin" copy /y "%OUT%\shaders.bin" "%STAGE%\shaders_seed.bin" >nul
if exist "%OUT%\pipekeys.bin" copy /y "%OUT%\pipekeys.bin" "%STAGE%\pipekeys_seed.bin" >nul
rem the model converter and its Blender add-on (tools/tsfp_model.py packed beside the add-on's own files)
mkdir "%STAGE%\tools"
copy /y tools\tsfp_model.py "%STAGE%\tools\" >nul || exit /b 1
rd /s /q "%OUT%\blender_addon" 2>nul
xcopy /e /i /q /y tools\blender\tsfp_model "%OUT%\blender_addon" >nul || exit /b 1
copy /y tools\tsfp_model.py "%OUT%\blender_addon\" >nul || exit /b 1
rd /s /q "%OUT%\blender_addon\__pycache__" 2>nul
powershell -NoProfile -Command "Compress-Archive -Force -Path '%OUT%\blender_addon\*' -DestinationPath '%STAGE%\tools\tsfp_model_blender.zip'" || exit /b 1
rem the version (.local\next_version.txt, major.minor.patch: publish.bat makes it the release's tag and counts the
rem patch up after) and the repository the launcher updates from (.local\update_repo.txt, owner/name: the packer's own)
set "VERSION=0.0.0"
if exist ".local\next_version.txt" set /p VERSION=<".local\next_version.txt"
>"%STAGE%\version.txt" echo %VERSION%
>"%OUT%\version.txt" echo %VERSION%
if exist ".local\update_repo.txt" copy /y ".local\update_repo.txt" "%STAGE%\update.txt" >nul
xcopy /e /i /q /y prompts "%STAGE%\prompts" >nul || exit /b 1
copy /y README.md "%STAGE%\" >nul
copy /y sdk\THIRD_PARTY.txt "%STAGE%\" >nul
echo Put your US Xbox game here: default.xbe with the game's folders beside it, or the disc image (.iso).> "%STAGE%\assets\PUT THE GAME HERE.txt"
mkdir "%STAGE%\mods"
rem the mods made from the game's files into out/build/release/mods_play (the early Cortez by tools/tsfp_model.py,
rem the early HUD's Temporal Uplink and scope textures): the packer's own, never part of the source
if exist "%OUT%\mods_play" xcopy /e /i /q /y "%OUT%\mods_play" "%STAGE%\mods" >nul
echo Each mod is a folder here: mods\^<mod^>\files\ holds files by their path in the game's paks. See README.md, Mods.> "%STAGE%\mods\PUT MODS HERE.txt"
del /q "%GAME_EXE%.zip" 2>nul
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%GAME_EXE%.zip'" || exit /b 1
echo %GAME_EXE%.zip written.
exit /b 0

:source
rem the project as it builds: no game files (assets), no recompiled game code (generated), no build or local files
rd /s /q "%STAGE%-source" 2>nul
robocopy . "%STAGE%-source\%GAME_EXE%" /e /nfl /ndl /njh /njs /xd out generated assets .local .vs logs hdd CrashDumps /xf *.zip *.exe *.pdb settings.toml build_log.txt >nul
if errorlevel 8 exit /b 1
del /q "%GAME_EXE%-source.zip" 2>nul
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%-source\%GAME_EXE%' -DestinationPath '%GAME_EXE%-source.zip'" || exit /b 1
echo %GAME_EXE%-source.zip written.
exit /b 0
