@echo off
rem Stage 2b on (default): stop the DC while the screen is off. Restart Windows after.
reg delete HKLM\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters /v DcStop /f
pause
