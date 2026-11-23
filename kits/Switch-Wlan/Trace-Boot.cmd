@echo off
rem Records Windows Wi-Fi and network traces across a restart (persistent), until Trace-Stop.cmd is run.
net session >nul 2>&1 || (echo Run this as Administrator. & pause & exit /b 1)
netsh trace stop >nul 2>&1
netsh trace start scenario=NetConnection capture=yes persistent=yes report=disabled overwrite=yes maxsize=300 tracefile=C:\SwitchWOA-wlan.etl
echo.
echo Recording across restart. Now RESTART the Switch, let Wi-Fi fail to connect (wait 1 minute),
echo then run Trace-Stop.cmd (it can take a few minutes).
pause
