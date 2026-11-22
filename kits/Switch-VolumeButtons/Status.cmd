@echo off
rem Run as administrator: Buttons.ps1 -Mode Status (log C:\SwitchWOA-VolumeButtons.txt).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Buttons.ps1" -Mode Status
pause
