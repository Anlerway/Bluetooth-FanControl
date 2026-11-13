@echo off
rem 本地快速构建（MSVC 直接调 cl，不依赖 CMake）。产物: build\FanControl.UI.exe
rem 若当前环境已有 cl（CI 的 msvc-dev-cmd，或已进 Developer Prompt），直接用；
rem 否则依次尝试本机常见 vcvars64.bat 位置。
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
rc /nologo /fo build\resource.res resource.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /utf-8 /W4 /permissive- ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fo:build\ /Fe:build\FanControl.UI.exe ^
   src\main.cpp src\app.cpp src\ipc.cpp build\resource.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:app.manifest ^
   d2d1.lib dwrite.lib dwmapi.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib
exit /b %ERRORLEVEL%

:try
if defined VCVARS exit /b 0
if exist %1 set "VCVARS=%~1"
exit /b 0
