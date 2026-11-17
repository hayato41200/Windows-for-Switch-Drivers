@echo off
rem Run as administrator: after a Windows restart, hekate starts the hekate_ipl.ini entry
rem that has the line id=SWOA (add it to your Windows entry). Hold VOL- in hekate for its menu.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset.ps1" -Mode RebootTo -Id SWOA
pause
