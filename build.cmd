@echo off
rem Builds build\engine_fixes.dll (the BLSE plugin) and build\engine_fixes_intro.vtm (the startup
rem companion for the UP loader).
setlocal EnableExtensions
cd /d "%~dp0"
call :ensure_x86_cl || exit /b 1
if not exist build mkdir build
cl /nologo /std:c++17 /W4 /EHsc /MT /O2 /Iinclude /LD src\engine_fixes.cpp /Fobuild\engine_fixes.obj /Febuild\engine_fixes.dll /link /INCREMENTAL:NO /IMPLIB:build\engine_fixes.lib || exit /b 1
cl /nologo /std:c++17 /W4 /EHsc /MT /O2 /Iinclude /c src\engine_fixes_intro.cpp /Fobuild\engine_fixes_intro.obj || exit /b 1
cl /nologo /std:c++17 /W4 /EHsc /MT /O2 /Iinclude /c src\intro_worker.cpp /Fobuild\intro_worker.obj || exit /b 1
link /nologo /DLL /INCREMENTAL:NO /OUT:build\engine_fixes_intro.vtm /IMPLIB:build\engine_fixes_intro.lib build\engine_fixes_intro.obj build\intro_worker.obj bcrypt.lib user32.lib gdi32.lib shell32.lib winmm.lib || exit /b 1
exit /b 0

:ensure_x86_cl
where cl >nul 2>&1 && (cl 2>&1 | findstr /c:"for x86" >nul) && exit /b 0
set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL for %%v in (18 2022) do for %%e in (Community Professional Enterprise BuildTools) do if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\%%v\%%e"
if not defined VSINSTALL (echo error: Visual Studio with C++ x86 tools not found & exit /b 1)
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul 2>&1
cd /d "%~dp0"
where cl >nul 2>&1 || (echo error: VsDevCmd did not provide cl.exe & exit /b 1)
exit /b 0
