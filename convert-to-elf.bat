@echo off
setlocal EnableExtensions DisableDelayedExpansion
title AnyPS5 - convert fake-signed files to ELF

rem ==========================================================================
rem  Only the conversion step of port-game.bat, without AnyPS5:
rem  turns fake-signed SELF files (eboot.bin, .prx, .sprx ...) into plain
rem  ELF files. Drag onto this file:
rem    - a dump folder: it is copied to .\converted\(folder name) and every
rem      fake-signed file in the copy is converted (except fakelib, the
rem      console's own system files)
rem    - single files: each is written to .\converted\(file name) as ELF
rem  The dump itself is never changed. Encrypted (retail) files cannot be
rem  converted on a PC; they are listed.
rem ==========================================================================

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "HELPER=%ROOT%\bin\port-helper.exe"
set "OUT=%ROOT%\converted"
set "SYS=%SystemRoot%\System32"
set "PATH=%SYS%;%SystemRoot%;%SYS%\Wbem"
set "TEMP=%ROOT%\tmp"
set "TMP=%ROOT%\tmp"
if not exist "%TEMP%" mkdir "%TEMP%"

if not exist "%HELPER%" call :build_helper
if not exist "%HELPER%" goto :nohelper
if "%~1"=="" goto :usage
if not exist "%OUT%" mkdir "%OUT%"

set "FAILED="
:next
if "%~1"=="" goto :done
set "ATTR=%~a1"
if /i "%ATTR:~0,1%"=="d" (call :folder "%~1") else call :file "%~1"
shift
goto :next

:done
echo.
if defined FAILED echo Some files could not be converted - see the messages above.
if not defined FAILED echo Done. The results are in "%OUT%"
echo.
pause
if defined FAILED exit /b 1
exit /b 0


:folder
set "NAME=%~nx1"
if not defined NAME set "NAME=drive-%~d1"
set "NAME=%NAME::=%"
set "DST=%OUT%\%NAME%"
echo.
echo === "%~1" ===
echo Copying to "%DST%" ...
"%SYS%\robocopy.exe" "%~f1." "%DST%" /E /XJ /R:2 /W:2 /MT:8 /NFL /NDL /NP /NJH /NJS /A-:R /XD "%ROOT%" >nul
if errorlevel 8 goto :copy_failed
echo Converting fake-signed files ...
"%HELPER%" fixtree "%DST%"
if errorlevel 1 set "FAILED=1"
exit /b 0

:copy_failed
echo Copying failed - check the free space and that the folder can be read.
set "FAILED=1"
exit /b 0

:file
echo.
"%HELPER%" convert "%~f1" "%OUT%\%~nx1"
if errorlevel 1 set "FAILED=1"
exit /b 0

:build_helper
echo Preparing bin\port-helper.exe from port-helper.c ...
set "MINGW="
if exist "%ROOT%\tools\mingw64\bin\gcc.exe" set "MINGW=%ROOT%\tools\mingw64"
if not defined MINGW for /f "delims=" %%D in ('dir /b /ad "%ROOT%\tools" 2^>nul') do if exist "%ROOT%\tools\%%D\mingw64\bin\gcc.exe" set "MINGW=%ROOT%\tools\%%D\mingw64"
if not defined MINGW exit /b 1
if not exist "%ROOT%\port-helper.c" exit /b 1
if not exist "%ROOT%\bin" mkdir "%ROOT%\bin"
"%MINGW%\bin\gcc.exe" -O2 -s -static -municode -o "%HELPER%" "%ROOT%\port-helper.c"
exit /b 0

:nohelper
echo bin\port-helper.exe is missing - run build-anyps5.bat first.
echo.
pause
exit /b 1

:usage
echo Drag a game dump folder, or eboot.bin / .prx files, onto convert-to-elf.bat.
echo.
pause
exit /b 1
