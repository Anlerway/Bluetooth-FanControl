@echo off
rem FanControl native installer build: build\setup.exe + build\uninstall.exe (same sources, dispatch by exe name)
rem usage: build.bat [version]   e.g. build.bat 2.0.0
setlocal EnableExtensions
cd /d "%~dp0"

set "VERSION=%~1"
if "%VERSION%"=="" set "VERSION=2.0.0"
for /f "tokens=1,2,3 delims=." %%a in ("%VERSION%") do (
  set "VMAJOR=%%a"
  set "VMINOR=%%b"
  set "VPATCH=%%c"
)
if not defined VMAJOR set "VMAJOR=2"
if not defined VMINOR set "VMINOR=0"
if not defined VPATCH set "VPATCH=0"

where cl >nul 2>nul
if not errorlevel 1 goto compile

set "VCVARS="
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call :try "D:\APP\Visual Studio2026\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
  echo [installer] vcvars64.bat not found; run this script from a Developer Command Prompt.
  exit /b 2
)
call "%VCVARS%" >nul

:compile
if not exist build mkdir build
copy /y "..\..\src\FanControl.UI.Cpp\app.ico" "build\app.ico" >nul 2>nul
(
  echo #define FC_VER_MAJOR %VMAJOR%
  echo #define FC_VER_MINOR %VMINOR%
  echo #define FC_VER_PATCH %VPATCH%
  echo #define FC_VER_STRING "%VERSION%"
) > build\version.h

cl /nologo /c /std:c++20 /EHsc /utf-8 /W4 /permissive- /O2 /MT ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fo:build\ ^
   src\main.cpp src\app.cpp src\ui.cpp src\theme.cpp src\i18n.cpp ^
   src\payload.cpp src\lzma2.cpp src\sysutil.cpp src\install.cpp
if errorlevel 1 exit /b 1

set "OBJS=build\main.obj build\app.obj build\ui.obj build\theme.obj build\i18n.obj build\payload.obj build\lzma2.obj build\sysutil.obj build\install.obj"
set "LIBS=d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib shlwapi.lib shell32.lib ole32.lib uuid.lib user32.lib gdi32.lib advapi32.lib version.lib"

rem uninstaller first: setup.rc embeds build\uninstall.exe as RCDATA
rc /nologo /fo build\uninstall.res uninstall.rc
if errorlevel 1 exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /OUT:build\uninstall.exe %OBJS% build\uninstall.res %LIBS%
if errorlevel 1 exit /b 1

rc /nologo /fo build\setup.res setup.rc
if errorlevel 1 exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /OUT:build\setup.exe %OBJS% build\setup.res %LIBS%
if errorlevel 1 exit /b 1

echo [installer] build\setup.exe build\uninstall.exe
exit /b 0

:try
if defined VCVARS exit /b 0
if exist %1 set "VCVARS=%~1"
exit /b 0
