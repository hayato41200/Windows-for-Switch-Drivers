@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0Wifi.ps1" -Mode Status
pause
