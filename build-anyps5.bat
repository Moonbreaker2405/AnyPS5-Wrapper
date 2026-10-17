@echo off
setlocal EnableExtensions DisableDelayedExpansion
title AnyPS5 - setup, update and build

rem ==========================================================================
rem  One double-click does everything:
rem   1. First run only: downloads the two portable tools into .\tools and
rem      checks them against their official SHA-256 checksums.
rem   2. Downloads (or updates to) the newest AnyPS5 source into .\src
rem   3. Builds it in .\build and copies the result into .\bin, together
rem      with port-helper.exe (compiled from port-helper.c)
rem  Nothing is installed. No PATH, registry or user-profile changes.
rem  To remove everything, delete this folder.
rem ==========================================================================

cd /d "%~dp0"
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "T=%ROOT%\tools"
set "DL=%T%\downloads"
set "SRC=%ROOT%\src"
set "BUILD=%ROOT%\build"
set "BIN=%ROOT%\bin"
set "SYS=%SystemRoot%\System32"

rem Exact files used by AnyPS5's own CI, pinned by checksum.
set "WINLIBS_URL=https://github.com/brechtsanders/winlibs_mingw/releases/download/15.2.0posix-14.0.0-ucrt-r7/winlibs-x86_64-posix-seh-gcc-15.2.0-mingw-w64ucrt-14.0.0-r7.zip"
set "WINLIBS_FILE=winlibs-x86_64-posix-seh-gcc-15.2.0-mingw-w64ucrt-14.0.0-r7.zip"
set "WINLIBS_SHA256=cb2fbad6162540cdf5e1facdce08d4dac359e8cf64f7f696a99274291763b815"
set "PGIT_URL=https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.2/PortableGit-2.56.0.2-64-bit.7z.exe"
set "PGIT_FILE=PortableGit-2.56.0.2-64-bit.7z.exe"
set "PGIT_SHA256=075e158ef8e1f0ab80b347e245405d3eca735c2dc88fd8e032e137d0ca61f61b"

rem ---- Folder path checks ---------------------------------------------------
set "BADPATH="
if not "%ROOT%"=="%ROOT:&=%" set "BADPATH=1"
if not "%ROOT%"=="%ROOT:(=%" set "BADPATH=1"
if not "%ROOT%"=="%ROOT:)=%" set "BADPATH=1"
if not "%ROOT%"=="%ROOT:!=%" set "BADPATH=1"
if not "%ROOT%"=="%ROOT:^=%" set "BADPATH=1"
if defined BADPATH goto :badpath
if not "%ROOT%"=="%ROOT: =%" call :warn "The folder path contains spaces." "Recommended: a short path without spaces, for example  D:\AnyPS5"

rem Keep temporary files and Git's settings inside this folder. Git never
rem asks for a login (no pop-up windows) and trusts this folder on any drive.
set "TEMP=%ROOT%\tmp"
set "TMP=%ROOT%\tmp"
set "HOME=%T%\home"
set "GIT_TERMINAL_PROMPT=0"
set "GCM_INTERACTIVE=never"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%HOME%" mkdir "%HOME%"

rem ---- Tools ----------------------------------------------------------------
echo.
echo === Checking tools ===
call :find_tools
if defined MINGW goto :have_mingw
call :install_winlibs
if not "%errorlevel%"=="0" goto :fail
:have_mingw
if defined GITDIR goto :have_git
call :install_git
if not "%errorlevel%"=="0" goto :fail
:have_git
call :find_tools
if not defined MINGW (
  echo WinLibs GCC was not found in the tools folder.
  goto :fail
)
if not defined GITDIR (
  echo Portable Git was not found in the tools folder.
  goto :fail
)
echo Compiler: %MINGW%
echo Git:      %GITDIR%

rem Only the portable tools and Windows itself are visible from here on.
set "PATH=%MINGW%\bin;%GITDIR%\cmd;%SYS%;%SystemRoot%;%SYS%\Wbem"
git config --global --replace-all credential.helper ""
if not "%errorlevel%"=="0" goto :fail
git config --global --replace-all safe.directory "*"
if not "%errorlevel%"=="0" goto :fail

set "GCCVER="
for /f %%v in ('g++ -dumpfullversion') do set "GCCVER=%%v"
if not "%GCCVER%"=="15.2.0" call :warn "AnyPS5 needs GCC 15.2.0, but tools has GCC %GCCVER%." "To fix: delete that compiler folder in tools and run this script again."

rem WinLibs' CMake needs a certificate bundle to download FFmpeg during the
rem build (see AnyPS5 docs/dev/BUILD.md). Use the one that comes with Git.
set "SSL_CERT_FILE="
for %%C in ("%GITDIR%\ucrt64\etc\ssl\certs\ca-bundle.crt" "%GITDIR%\mingw64\etc\ssl\certs\ca-bundle.crt" "%GITDIR%\usr\ssl\certs\ca-bundle.crt") do if not defined SSL_CERT_FILE if exist "%%~C" set "SSL_CERT_FILE=%%~C"
if not defined SSL_CERT_FILE (
  echo Could not find the certificate bundle inside Portable Git.
  goto :fail
)

rem ---- Source ---------------------------------------------------------------
echo.
echo === Getting the newest AnyPS5 source ===
if exist "%SRC%" if not exist "%SRC%\.git" rmdir /s /q "%SRC%"
if exist "%SRC%\.git" goto :update_source
git clone --depth 1 https://github.com/boykopovar/AnyPS5.git "%SRC%"
if not "%errorlevel%"=="0" goto :fail
goto :source_ready
:update_source
git -C "%SRC%" fetch --depth 1 origin main
if not "%errorlevel%"=="0" goto :source_failed
git -C "%SRC%" reset --hard origin/main
if not "%errorlevel%"=="0" goto :source_failed
:source_ready
git -C "%SRC%" submodule sync --recursive
if not "%errorlevel%"=="0" goto :fail
git -C "%SRC%" submodule update --init --recursive --force --depth 1
if "%errorlevel%"=="0" goto :submodules_ready
echo Retrying the submodule download with full history...
git -C "%SRC%" submodule update --init --recursive --force
if not "%errorlevel%"=="0" goto :fail
:submodules_ready

rem ---- Build ----------------------------------------------------------------
echo.
echo === Configuring ===
set "OLDROOT="
if exist "%BUILD%\anyps5-location.txt" set /p "OLDROOT=" < "%BUILD%\anyps5-location.txt"
if exist "%BUILD%\CMakeCache.txt" if /i not "%OLDROOT%"=="%ROOT%" (
  echo This folder was moved or renamed - starting a clean build.
  rmdir /s /q "%BUILD%"
)
if not exist "%BUILD%" mkdir "%BUILD%"
>"%BUILD%\anyps5-location.txt" echo %ROOT%

rem port-helper.exe (port-game.bat's helper) is built first, because the
rem build also uses it in place of PowerShell: libusb, a part of AnyPS5, runs
rem a PowerShell script that only writes its version into a header file.
rem PowerShell itself would write cache files into AppData.
if not exist "%ROOT%\port-helper.c" (
  echo port-helper.c is missing - it belongs next to build-anyps5.bat.
  goto :fail
)
gcc -O2 -s -static -municode -o "%BUILD%\port-helper.exe" "%ROOT%\port-helper.c"
if not "%errorlevel%"=="0" goto :fail
set "PS_STANDIN=%BUILD:\=/%/port-helper.exe"

cmake -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ "-DLIBUSB_POWERSHELL_EXECUTABLE:FILEPATH=%PS_STANDIN%"
if not "%errorlevel%"=="0" goto :fail

echo.
echo === Building AnyPS5 - the first build takes a long time ===
cmake --build "%BUILD%" --parallel
if not "%errorlevel%"=="0" goto :fail

echo.
echo === Building the system libraries ===
cmake --build "%BUILD%" --target libs --parallel
if not "%errorlevel%"=="0" goto :fail

rem ---- Result: the same files as an official AnyPS5 Windows release ----------
echo.
echo === Collecting the result in bin ===
if exist "%BIN%" rmdir /s /q "%BIN%"
mkdir "%BIN%\libs"
copy /y "%BUILD%\core\relinker\relinker.exe" "%BIN%\" >nul
copy /y "%BUILD%\port-helper.exe" "%BIN%\" >nul
copy /y "%BUILD%\core\libs\libs\*.prx" "%BIN%\libs\" >nul
for %%d in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do copy /y "%MINGW%\bin\%%d" "%BIN%\libs\" >nul
set "MISSING="
if not exist "%BIN%\relinker.exe" call :missing relinker.exe
if not exist "%BIN%\port-helper.exe" call :missing port-helper.exe
for %%d in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do if not exist "%BIN%\libs\%%d" call :missing %%d
for /f "delims=" %%P in ('dir /b /ad "%SRC%\core\libs\prx" 2^>nul') do if not exist "%BIN%\libs\%%P.prx" call :missing %%P.prx
if defined MISSING goto :fail
git -C "%SRC%" log -1 --format="AnyPS5 commit %%h from %%cd" > "%BIN%\VERSION.txt"


echo.
type "%BIN%\VERSION.txt"
echo Done. Drag a game folder onto port-game.bat to convert it.
echo.
pause
exit /b 0


rem ==========================================================================
rem  Subroutines
rem ==========================================================================

:find_tools
set "MINGW="
set "GITDIR="
if exist "%T%\mingw64.extracting" goto :find_git
if exist "%T%\mingw64\bin\g++.exe" set "MINGW=%T%\mingw64"
if not defined MINGW for /f "delims=" %%D in ('dir /b /ad "%T%" 2^>nul') do if exist "%T%\%%D\mingw64\bin\g++.exe" set "MINGW=%T%\%%D\mingw64"
:find_git
if exist "%T%\PortableGit.extracting" exit /b 0
if exist "%T%\PortableGit\cmd\git.exe" if not exist "%T%\PortableGit\post-install.bat" set "GITDIR=%T%\PortableGit"
if not defined GITDIR if exist "%T%\git\cmd\git.exe" if not exist "%T%\git\post-install.bat" set "GITDIR=%T%\git"
exit /b 0

:install_winlibs
echo WinLibs GCC 15.2.0 is not set up yet.
call :fetch "%WINLIBS_URL%" "%DL%\%WINLIBS_FILE%" "%WINLIBS_SHA256%"
if not "%errorlevel%"=="0" exit /b 1
if not exist "%SYS%\tar.exe" goto :old_windows
echo Extracting WinLibs - about 1 GB, this takes a few minutes...
if exist "%T%\mingw64" rmdir /s /q "%T%\mingw64"
>"%T%\mingw64.extracting" echo extracting
"%SYS%\tar.exe" -xf "%DL%\%WINLIBS_FILE%" -C "%T%"
if not "%errorlevel%"=="0" (
  echo Extracting WinLibs failed.
  exit /b 1
)
if not exist "%T%\mingw64\bin\g++.exe" (
  echo The WinLibs archive did not contain mingw64\bin\g++.exe
  exit /b 1
)
del "%T%\mingw64.extracting"
del "%DL%\%WINLIBS_FILE%"
rmdir "%DL%" 2>nul
exit /b 0

:install_git
if exist "%T%\PortableGit.extracting" goto :git_extract
if exist "%T%\PortableGit\cmd\git.exe" goto :git_setup
:git_extract
echo Portable Git 2.56.0.2 is not set up yet.
call :fetch "%PGIT_URL%" "%DL%\%PGIT_FILE%" "%PGIT_SHA256%"
if not "%errorlevel%"=="0" exit /b 1
echo Extracting Portable Git...
if exist "%T%\PortableGit" rmdir /s /q "%T%\PortableGit"
>"%T%\PortableGit.extracting" echo extracting
"%DL%\%PGIT_FILE%" -o"%T%\PortableGit" -y
if not exist "%T%\PortableGit\cmd\git.exe" (
  echo Extracting Portable Git failed.
  exit /b 1
)
del "%T%\PortableGit.extracting"
:git_setup
rem After extracting, Portable Git runs its one-time setup script
rem post-install.bat, which deletes itself when done. Wait for it, or run it.
call :wait_postinstall 30
if "%errorlevel%"=="0" goto :git_done
echo Running Portable Git's one-time setup...
start "" /d "%T%\PortableGit" /wait "%T%\PortableGit\git-bash.exe" --no-needs-console --hide --no-cd --command=post-install.bat
call :wait_postinstall 120
if "%errorlevel%"=="0" goto :git_done
echo Portable Git's one-time setup did not finish.
exit /b 1
:git_done
if exist "%DL%\%PGIT_FILE%" del "%DL%\%PGIT_FILE%"
rmdir "%DL%" 2>nul
exit /b 0

:wait_postinstall
set /a WAITED=0
:wait_postinstall_loop
if not exist "%T%\PortableGit\post-install.bat" exit /b 0
if %WAITED% GEQ %~1 exit /b 1
"%SYS%\ping.exe" -n 2 127.0.0.1 >nul
set /a WAITED+=1
goto :wait_postinstall_loop

:fetch
rem  %1 = URL   %2 = file   %3 = expected SHA-256
if not exist "%DL%" mkdir "%DL%"
if not exist "%~2" goto :fetch_download
call :sha256_ok "%~2" "%~3"
if "%errorlevel%"=="0" exit /b 0
echo The existing %~nx2 is incomplete or damaged - downloading it again.
del "%~2"
:fetch_download
if not exist "%SYS%\curl.exe" goto :fetch_manual
echo Downloading %~nx2
"%SYS%\curl.exe" -q -L --fail --retry 3 --retry-delay 3 -o "%~2.part" "%~1"
if not "%errorlevel%"=="0" goto :fetch_failed
move /y "%~2.part" "%~2" >nul
call :sha256_ok "%~2" "%~3"
if "%errorlevel%"=="0" exit /b 0
del "%~2"
echo The download did not match its official SHA-256 checksum and was deleted.
echo Please run this script again later.
exit /b 1
:fetch_failed
if exist "%~2.part" del "%~2.part"
:fetch_manual
echo.
echo Could not download %~nx2
echo Download it in your browser from:
echo   %~1
echo and put the file into:
echo   %DL%
echo Then run this script again.
exit /b 1

:sha256_ok
rem  %1 = file   %2 = expected SHA-256.  Exit code 0 = matches.
set "HASH="
for /f "skip=1 delims=" %%h in ('%SYS%\certutil.exe -hashfile "%~1" SHA256') do if not defined HASH set "HASH=%%h"
if not defined HASH exit /b 1
set "HASH=%HASH: =%"
if /i "%HASH%"=="%~2" exit /b 0
exit /b 1

:missing
echo Missing in the build result: %~1
set "MISSING=1"
exit /b 0

:warn
echo.
echo WARNING: %~1
if not "%~2"=="" echo %~2
echo Press a key to continue anyway, or close this window to stop.
pause >nul
exit /b 0

:old_windows
echo This needs the curl and tar tools that come with Windows 10 version 1803
echo or newer. Please update Windows, then run this script again.
exit /b 1

:badpath
echo.
echo The path of this folder contains one of these characters:  ^& ^( ^) ^! ^^
echo Please move the folder to a simple path, for example  D:\AnyPS5
echo.
pause
exit /b 1

:source_failed
echo.
echo Updating the source failed. If this keeps happening, delete the src
echo folder and run this script again.
goto :fail

:fail
echo.
echo *** Stopped - see the messages above. ***
echo Run this script again - it continues where it stopped.
echo If the build itself fails, the newest AnyPS5 may be broken upstream right
echo now. That is usually fixed within hours - try again later.
echo.
pause
exit /b 1
