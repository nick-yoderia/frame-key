@echo off
setlocal
rem Frame Key: installs the headset side on a Steam Frame over SSH. Double-click to run.
rem Run again to update. Also removes the PC side that releases before 0.7 installed.
if not exist "%~dp0pc\install-headset.ps1" goto notextracted
if not exist "%~dp0headset\install-headset.sh" goto notextracted
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc\remove-pc-side.ps1" || goto fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc\install-headset.ps1" || goto fail
echo.
echo Done. In the headset: SteamVR dashboard ^> Frame Key.
pause
exit /b 0
:notextracted
echo Extract the whole zip to a folder first, then run Install.cmd from that folder.
pause
exit /b 1
:fail
echo.
echo Stopped: see the message above. Fix it and run Install.cmd again.
pause
exit /b 1
