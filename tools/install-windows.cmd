@echo off
rem Retoque for Windows: install OptiScaler with DLSS Neural Rendering into one game folder, or take it out.
rem
rem All this file does is start install-windows.ps1, which sits beside it, in Windows PowerShell.
rem -ExecutionPolicy Bypass applies to that one powershell.exe process only: no execution policy is changed
rem for your user or for the machine. install-windows.ps1 says what it does and refuses to do.
rem
rem   install-windows.cmd                        asks for the game folder and what to do
rem   install-windows.cmd -GameDir "D:\Games\Some Game\bin64" [-ProxyName winmm.dll] [-KeepReShade] [-DryRun]
rem   install-windows.cmd -GameDir "D:\Games\Some Game\bin64" -Uninstall
rem
rem This file keeps CRLF line ends (tools/.gitattributes): cmd.exe can miss a label in a file with LF only.
setlocal EnableExtensions DisableDelayedExpansion

set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
rem A 32-bit cmd.exe sees SysWOW64 as System32, and a 32-bit PowerShell cannot read a 64-bit game's modules.
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
set "SCRIPT=%~dp0install-windows.ps1"
if not exist "%SCRIPT%" goto :noscript

rem With arguments: pass them through unchanged, no questions, no pause.
if "%~1"=="" goto :ask
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
exit /b %ERRORLEVEL%

:ask
echo Retoque: OptiScaler with DLSS Neural Rendering, for one game folder.
echo.
echo Give the folder that holds the game's .exe (Unreal Engine games: ^<Game^>\Binaries\Win64).
echo You can drag the folder into this window. Close the game first.
echo Nothing is written before you choose an action below.
echo.
set "GAMEDIR="
set /p "GAMEDIR=Game folder: "
if not defined GAMEDIR goto :usage
set "GAMEDIR=%GAMEDIR:"=%"
if not defined GAMEDIR goto :usage
rem "C:\Games\" would end in \" and swallow its closing quote on the way to PowerShell.
if "%GAMEDIR:~-1%"=="\" set "GAMEDIR=%GAMEDIR:~0,-1%"
echo.
choice /c IUDQ /n /m "[I]nstall, [U]ninstall, [D]ry run (show what an install would do), or [Q]uit? "
set "ACTION=%ERRORLEVEL%"
if "%ACTION%"=="4" exit /b 0
if not "%ACTION%"=="2" goto :proxy
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -GameDir "%GAMEDIR%" -Uninstall
goto :finish

:proxy
set "PROXY="
set /p "PROXY=Proxy DLL name, Enter for dxgi.dll (or winmm.dll, version.dll, dbghelp.dll, d3d12.dll, winhttp.dll, wininet.dll): "
if not defined PROXY set "PROXY=dxgi.dll"
set "DRY="
if "%ACTION%"=="3" set "DRY=-DryRun"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -GameDir "%GAMEDIR%" -ProxyName "%PROXY%" %DRY%

:finish
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="4" echo To go ahead anyway, run it from a command prompt with the flag named above, for example:
if "%RC%"=="4" echo   install-windows.cmd -GameDir "%GAMEDIR%" -KeepReShade
if "%RC%"=="5" echo To go ahead anyway: install-windows.cmd -GameDir "%GAMEDIR%" -AllowSecondProxy
if not "%RC%"=="0" echo Exit code %RC%. The lines above say what happened.
pause
exit /b %RC%

:usage
echo No folder given. Usage:
echo   install-windows.cmd                        asks for the game folder and what to do
echo   install-windows.cmd -GameDir "<folder>" [-ProxyName winmm.dll] [-KeepReShade] [-DryRun]
echo   install-windows.cmd -GameDir "<folder>" -Uninstall
pause
exit /b 1

:noscript
echo install-windows.ps1 is not beside this file. Extract the whole release zip, then run this again.
pause
exit /b 1
