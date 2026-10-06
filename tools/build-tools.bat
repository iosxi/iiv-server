@echo off
rem Build the test programs (tools\mfprobe.c, mfenc.c, mfdec.c) into build\mf.
rem ASCII only and CRLF on purpose (see build.bat).
setlocal
if defined VCINSTALLDIR goto :build
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VSPATH="
pushd "%VSINSTALLER%"
for /f "tokens=*" %%i in ('.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSPATH=%%i"
popd
if not defined VSPATH goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
:build
cd /d "%~dp0.."
if not exist build\mf mkdir build\mf
set "CFLAGS=/nologo /utf-8 /W3 /O2 /MT /D_CRT_SECURE_NO_WARNINGS"
cl %CFLAGS% /Fobuild\mf\ /Febuild\mf\mfprobe.exe tools\mfprobe.c || exit /b 1
cl %CFLAGS% /Fobuild\mf\ /Febuild\mf\mfenc.exe tools\mfenc.c || exit /b 1
cl %CFLAGS% /Fobuild\mf\ /Febuild\mf\mfdec.exe tools\mfdec.c || exit /b 1
echo OK
exit /b 0
:novs
echo [error] Visual Studio Build Tools not found.
exit /b 1
