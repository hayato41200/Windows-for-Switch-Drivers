@echo off
rem Run as administrator: driver version, payload, methods (read only).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode Status
pause
