@echo off
rem Run as administrator: after a Windows restart, show the hekate menu (default).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode RebootTo
pause
