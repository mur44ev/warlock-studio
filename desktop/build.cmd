@echo off
setlocal
rem Set WARLOCK_QT_DIR to a Qt 5.15 MSVC x64 installation, or put qmake on PATH.
if defined WARLOCK_QT_DIR set "PATH=%WARLOCK_QT_DIR%\bin;%PATH%"
if not defined VSCMD_VER (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat"
)
where cl >nul 2>nul
if errorlevel 1 (echo Run in an x64 MSVC Developer Command Prompt. & exit /b 1)
where qmake >nul 2>nul
if errorlevel 1 (echo Set WARLOCK_QT_DIR to Qt 5.15 MSVC x64. & exit /b 1)
cd /d "%~dp0.."
if not exist third_party\hidapi\windows\hid.c (echo Run git submodule update --init --recursive first. & exit /b 1)
if not exist build\hidapi mkdir build\hidapi
if not exist dist\studio mkdir dist\studio
cl /nologo /O2 /MD /LD /Ithird_party\hidapi\hidapi third_party\hidapi\windows\hid.c /Fobuild\hidapi\hid.obj /link /OUT:dist\studio\hidapi.dll /IMPLIB:build\hidapi\hidapi.lib setupapi.lib cfgmgr32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MD /EHsc /std:c++17 tools\usb_rates.cpp /Fobuild\usb_rates.obj /Fe:dist\studio\usb_rates.exe /link setupapi.lib
if errorlevel 1 exit /b 1
cd desktop
qmake Studio.pro
if errorlevel 1 exit /b 1
nmake /nologo
if errorlevel 1 exit /b 1
windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw "..\dist\studio\WarlockStudio.exe"
exit /b %errorlevel%
