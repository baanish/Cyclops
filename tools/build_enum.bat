@echo off
pushd "%~dp0"
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VS=%%i
if not defined VS (echo vswhere found no VS install & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /EHsc /std:c++20 /nologo dshow_enum.cpp strmiids.lib ole32.lib oleaut32.lib
popd
