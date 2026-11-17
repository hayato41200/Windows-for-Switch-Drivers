@echo off
rem Run as administrator: restart through PSCI SYSTEM_RESET (bl31 does R2P on the hekate chain)
rem instead of writing the payload from Windows. Undo with Restart-Auto.cmd.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode Method -Restart Psci
pause
