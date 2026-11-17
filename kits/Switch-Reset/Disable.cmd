@echo off
rem Run as administrator: the driver no longer restarts or powers off; Windows does (as before).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode Disable
pause
