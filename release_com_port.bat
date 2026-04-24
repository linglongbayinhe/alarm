@echo off
setlocal

rem Edit these two lines if your board uses another serial port or USB-serial chip name.
set "PORT=COM6"
set "BOARD_KEYWORD=CH340K"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0release_com_port.ps1" -Port "%PORT%" -BoardKeyword "%BOARD_KEYWORD%"

echo.
pause
