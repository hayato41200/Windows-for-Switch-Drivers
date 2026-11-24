@echo off
rem Double-click: if not administrator, ask UAC and run this file again elevated.
rem Run as administrator: installs switchpwrbtn v2 (restart to hekate, power-off) and points it
rem at bootloader\update.bin on partition 1 of the boot SD (read only).
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  if errorlevel 1 (
    echo Could not start as administrator.
    pause
  )
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode Install
pause
