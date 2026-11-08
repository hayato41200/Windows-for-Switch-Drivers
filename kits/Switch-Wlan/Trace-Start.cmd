@echo off
rem Records Windows Wi-Fi and network traces until Trace-Stop.cmd is run.
net session >nul 2>&1 || (echo Run this as Administrator. & pause & exit /b 1)
netsh trace stop >nul 2>&1
netsh trace start scenario=NetConnection capture=yes report=disabled overwrite=yes maxsize=300 tracefile=C:\SwitchWOA-wlan.etl
echo.
echo Recording. Now connect to your Wi-Fi network, wait until it fails, then run Trace-Stop.cmd.
pause
