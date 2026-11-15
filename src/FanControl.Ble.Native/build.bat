@echo off
REM Build FanControl.Ble.dll (x64, /MT, only depends on the OS WinRT)
setlocal
set HERE=%~dp0
if not defined VCINSTALLDIR (
  call "D:\APP\Visual Studio2026\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
if not exist "%HERE%build" mkdir "%HERE%build"
pushd "%HERE%src"
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /W4 /LD bleapi.cpp ^
   /link /OUT:"%HERE%build\FanControl.Ble.dll" WindowsApp.lib || (popd & exit /b 1)
popd
echo built %HERE%build\FanControl.Ble.dll
