@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0Gpu.ps1" -Mode PowerTest
pause
