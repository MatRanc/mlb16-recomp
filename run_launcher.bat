@echo off
cd /d "%~dp0"
python tools\launcher\mlb16_launcher.py %*
if %ERRORLEVEL% NEQ 0 pause
