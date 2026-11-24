@echo off
rem Double-click: if not administrator, ask UAC and run this file again elevated.
rem Fast startup Off (test). Off is the normal setting: Shut down then powers the console off.
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  if errorlevel 1 (
    echo Could not start as administrator.
    pause
  )
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode FastStartup -Value Off
pause
