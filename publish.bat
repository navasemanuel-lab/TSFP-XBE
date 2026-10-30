@echo off
rem publish.bat [notes]  the TSFP.zip release.bat wrote, as the latest release of the repository in
rem .local\update_repo.txt (owner/name), its tag the version it was packed with: every launcher's UPDATE NOW
rem then installs it. Needs GitHub CLI (gh), signed in once with: gh auth login
setlocal
cd /d "%~dp0"
set "OUT=out\build\release"
where gh >nul 2>nul || (echo GitHub CLI not found: install it from https://cli.github.com and run gh auth login & exit /b 1)
if not exist ".local\update_repo.txt" (echo Write the repository, owner/name, into .local\update_repo.txt & exit /b 1)
if not exist "TSFP.zip" (echo No TSFP.zip: run release.bat first & exit /b 1)
if not exist "%OUT%\version.txt" (echo No version: run release.bat first & exit /b 1)
set /p REPO=<".local\update_repo.txt"
set /p VERSION=<"%OUT%\version.txt"
set "NOTES=%~1"
if not defined NOTES set "NOTES=Build %VERSION%"
gh release create "%VERSION%" "TSFP.zip" --repo "%REPO%" --title "%VERSION%" --notes "%NOTES%" --latest || exit /b 1
echo Published %VERSION% to %REPO%.
rem the next one: the patch counted up
powershell -NoProfile -Command "$v = [version]'%VERSION%'; '{0}.{1}.{2}' -f $v.Major, $v.Minor, ($v.Build + 1) | Set-Content -Encoding ascii '.local\next_version.txt'"
exit /b 0
