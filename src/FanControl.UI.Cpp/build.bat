@echo off
rem Local quick build (MSVC cl directly, no CMake). Output: build\FanControl.UI.exe + build\FanControl.exe
rem If cl is already on PATH (CI / Developer Prompt) use it, otherwise probe common vcvars64.bat locations.
setlocal
where cl >nul 2>nul
if not errorlevel 1 goto build

set "VCVARS="
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call :try "D:\APP\Visual Studio2026\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
  echo [build] vcvars64.bat not found; run this script from a Developer Command Prompt.
  exit /b 2
)
call "%VCVARS%" >nul

:build
if not exist build mkdir build
if not exist build\launcher mkdir build\launcher
rc /nologo /fo build\resource.res resource.rc
if errorlevel 1 exit /b 1
rc /nologo /fo build\launcher.res launcher.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /utf-8 /W4 /permissive- ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fo:build\ /Fe:build\FanControl.UI.exe ^
   src\main.cpp src\app.cpp src\ipc.cpp src\i18n.cpp build\resource.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:app.manifest ^
   d2d1.lib dwrite.lib dwmapi.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib
if errorlevel 1 exit /b 1
rem Elevation launcher: installer and the ONLOGON task point at FanControl.exe
cl /nologo /std:c++20 /EHsc /utf-8 /W4 /permissive- ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fo:build\launcher\ /Fe:build\FanControl.exe ^
   src\launcher.cpp build\launcher.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:launcher.manifest ^
   /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   user32.lib shell32.lib
exit /b %ERRORLEVEL%

:try
if defined VCVARS exit /b 0
if exist %1 set "VCVARS=%~1"
exit /b 0
