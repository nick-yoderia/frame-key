@echo off
setlocal
rem Frame Key: removes the headset side, then the PC side. Double-click to run.
if not exist "%~dp0pc\install-pc.ps1" goto notextracted
echo == Headset side
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc\install-headset.ps1" -Uninstall
if errorlevel 1 echo The headset side wasn't removed (see above); the PC side is removed anyway.
echo.
echo == PC side
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc\install-pc.ps1" -Uninstall
echo.
pause
exit /b 0
:notextracted
echo Extract the whole zip to a folder first, then run Uninstall.cmd from that folder.
pause
exit /b 1
