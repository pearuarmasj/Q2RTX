@echo off
setlocal

rem Configure MSVC + Windows SDK environment for command-line builds.
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 (
  echo Failed to initialize Visual Studio developer environment.
  exit /b 1
)

rem Build using the existing Ninja build directory.
ninja -C "c:\Users\pearu\Q2RTX\build" %*
exit /b %errorlevel%
