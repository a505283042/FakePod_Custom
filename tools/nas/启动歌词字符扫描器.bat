@echo off
chcp 65001 >nul
cd /d "%~dp0"
py -3 lyrics_font_analyzer.py %*
if errorlevel 1 pause
