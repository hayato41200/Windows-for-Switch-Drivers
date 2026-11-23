@echo off
rem Double-click: if not administrator, ask UAC and run this file again elevated.
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  if errorlevel 1 (
    echo Could not start as administrator.
    pause
  )
  exit /b
)
powershell -ExecutionPolicy Bypass -File "%~dp0Fan.ps1" -Mode Status
pause
