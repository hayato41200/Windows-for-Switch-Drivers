@echo off
rem Run as administrator: the driver restarts to hekate and powers off again (default).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode Enable
pause
