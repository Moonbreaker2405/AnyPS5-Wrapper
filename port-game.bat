@echo off
setlocal EnableExtensions DisableDelayedExpansion
title AnyPS5 - port a game

rem ==========================================================================
rem  Converts a PS5 game dump into a Windows game folder in .\ported
rem  Drag the game's folder onto this file. It can be:
rem    - the dump folder itself (the one with eboot.bin), or a folder around it
rem    - the eboot.bin file
rem    - the game folder and its patch folder together (or a folder with both)
rem  Plain (decrypted) and fake-signed dumps both work. The dump itself is
rem  never changed. Running it again for the same game updates the copy.
rem
rem  The work is done by bin\port-helper.exe, which is compiled from
rem  port-helper.c with the compiler in tools the first time it is needed.
rem ==========================================================================

rem Windows puts quotes only around dragged paths that contain spaces, so a
rem path with an ampersand or caret character arrives cut off or changed in
rem the arguments. The helper therefore also gets cmd.exe's own command line.
setlocal EnableDelayedExpansion
set "ANYPS5_CMDLINE=!cmdcmdline!"
setlocal DisableDelayedExpansion
set "ANYPS5_BAT=%~f0"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BIN=%ROOT%\bin"
set "HELPER=%BIN%\port-helper.exe"
set "HELPER_VERSION=2"
set "SYS=%SystemRoot%\System32"
set "PATH=%SYS%;%SystemRoot%;%SYS%\Wbem"
set "TEMP=%ROOT%\tmp"
set "TMP=%ROOT%\tmp"
if not exist "%TEMP%" mkdir "%TEMP%"

if not exist "%BIN%\relinker.exe" goto :nobuild

set "NEED_HELPER=1"
if exist "%HELPER%" (
  "%HELPER%" version %HELPER_VERSION% >nul 2>&1
  if not errorlevel 1 set "NEED_HELPER="
)
if defined NEED_HELPER call :build_helper
if not exist "%HELPER%" goto :nohelper

"%HELPER%" port "%ROOT%" %*
set "RC=%errorlevel%"
echo.
pause
rem Started by dragging: end cmd.exe here, or it would run the part of a
rem dragged path after an ampersand character as a command.
"%HELPER%" dragged >nul 2>&1
if not errorlevel 1 exit %RC%
exit /b %RC%


:build_helper
echo Preparing bin\port-helper.exe from port-helper.c ...
set "MINGW="
if exist "%ROOT%\tools\mingw64\bin\gcc.exe" set "MINGW=%ROOT%\tools\mingw64"
if not defined MINGW for /f "delims=" %%D in ('dir /b /ad "%ROOT%\tools" 2^>nul') do if exist "%ROOT%\tools\%%D\mingw64\bin\gcc.exe" set "MINGW=%ROOT%\tools\%%D\mingw64"
if not defined MINGW (
  echo The compiler in the tools folder was not found - run build-anyps5.bat first.
  exit /b 1
)
if not exist "%ROOT%\port-helper.c" (
  echo port-helper.c is missing - it belongs next to port-game.bat.
  exit /b 1
)
if exist "%HELPER%" del "%HELPER%"
set "PATH=%MINGW%\bin;%PATH%"
"%MINGW%\bin\gcc.exe" -O2 -s -static -municode -o "%HELPER%" "%ROOT%\port-helper.c"
exit /b 0

:nobuild
echo bin\relinker.exe not found - run build-anyps5.bat first.
echo.
pause
exit /b 1

:nohelper
echo.
echo bin\port-helper.exe could not be created - see the messages above.
echo.
pause
exit /b 1
