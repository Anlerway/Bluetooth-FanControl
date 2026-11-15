@echo off
rem Quick local build (MSVC cl, no CMake). Output: build\FanControl.UI.dll
rem The UI is a DLL loaded in-process by FanControl.exe (single process) via FcUiRun().
rem If cl is already on PATH (CI / Developer Prompt) use it, otherwise probe common vcvars64.bat paths.
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
rem /MT: static C runtime so the DLL loads on machines without the VC++ redistributable
cl /nologo /std:c++20 /EHsc /utf-8 /W4 /permissive- /LD /MT ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fo:build\ /Fe:build\FanControl.UI.dll ^
   src\main.cpp src\app.cpp src\ipc.cpp src\i18n.cpp src\serialports.cpp build\resource.res ^
   /link d2d1.lib dwrite.lib dwmapi.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib
exit /b %ERRORLEVEL%

:try
if defined VCVARS exit /b 0
if exist %1 set "VCVARS=%~1"
exit /b 0
