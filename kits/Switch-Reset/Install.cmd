@echo off
rem Run as administrator: installs switchpwrbtn v2 (restart to hekate, power-off) and points it
rem at bootloader\update.bin on partition 1 of the boot SD (read only).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode Install
pause
