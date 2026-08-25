@echo off
cd /d "%~dp0"
title FFmpeg MJPEG GUI v4
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0launcher.ps1"
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" (
  echo.
  echo Exit code: %ERR%
  pause
)
