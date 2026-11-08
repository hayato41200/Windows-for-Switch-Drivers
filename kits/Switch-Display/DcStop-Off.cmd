@echo off
rem Stage 2b off: the DC keeps scanning while the screen is off (panel sleep stays). Restart Windows after.
reg add HKLM\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters /v DcStop /t REG_DWORD /d 0 /f
pause
