@echo off
rem Run as administrator: default methods (restart: R2P from Windows if the payload loaded, else PSCI; power-off: PMIC).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode Method -Restart Auto -PowerOff Pmic
pause
