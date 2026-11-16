@echo off
setlocal
rem Frame Key: removes it from the headset, then what it left on this PC. Double-click to run.
if not exist "%~dp0pc\install-headset.ps1" goto notextracted
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc\install-headset.ps1" -Uninstall
if errorlevel 1 echo The headset side wasn't removed (see above).
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc\remove-pc-side.ps1" -All
echo.
pause
exit /b 0
:notextracted
echo Extract the whole zip to a folder first, then run Uninstall.cmd from that folder.
pause
exit /b 1
