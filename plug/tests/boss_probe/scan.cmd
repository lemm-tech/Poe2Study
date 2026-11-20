@echo off
chcp 65001 >nul
"%~dp0BossProbe.exe" %*
echo.
pause
