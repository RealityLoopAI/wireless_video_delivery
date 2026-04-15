@echo off
setlocal
set "ROOT=%~dp0"
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%one_click_windows.ps1"
