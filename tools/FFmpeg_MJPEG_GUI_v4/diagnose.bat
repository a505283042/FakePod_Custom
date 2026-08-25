@echo off
cd /d "%~dp0"
title FFmpeg MJPEG GUI v4 Diagnose
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0diagnose.ps1"
pause
