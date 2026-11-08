@echo off
net session >nul 2>&1 || (echo Run this as Administrator. & pause & exit /b 1)
echo Stopping (this can take a minute)...
netsh trace stop
echo.
dir C:\SwitchWOA-wlan.etl
echo Copy C:\SwitchWOA-wlan.etl to the USB drive and bring it to the PC.
pause
