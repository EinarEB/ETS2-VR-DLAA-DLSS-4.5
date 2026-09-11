@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 call :load_visual_studio
if errorlevel 1 exit /b 1
if not "%VSCMD_ARG_TGT_ARCH%"=="x64" exit /b 1
if not exist "build\dlaa" mkdir "build\dlaa"
> build\toolchain.txt echo MSVC=%VCToolsVersion%
>> build\toolchain.txt echo WindowsSDK=%WindowsSDKVersion%
>> build\toolchain.txt echo Target=%VSCMD_ARG_TGT_ARCH%
rc /nologo /fo build\dlaa\version.res src\dlaa\version.rc || exit /b 1
rc /nologo /I . /fo build\dlaa\embedded_assets.res src\dlaa\embedded_assets.rc || exit /b 1
cl /nologo /LD /EHsc /O2 /MT /W4 /WX /utf-8 /std:c++20 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DETS2_DEPTH_EMBEDDED /DDEPTH_MATCH_MAX_CANDIDATES=4 /Iexternal\reshade\include /Iexternal\imgui /Fobuild\dlaa\ /Fdbuild\dlaa\ src\dlaa\addon.cpp src\dlaa\embedded_assets.cpp src\dlaa\control_sync.cpp src\dlaa\settings_overlay.cpp src\standalone\native_sr.cpp src\depth\depth_match_addon.cpp /link /OUT:build\dlaa\ets2-dlaa.addon64 /IMPLIB:build\dlaa\ets2-dlaa.lib build\dlaa\version.res build\dlaa\embedded_assets.res d3d11.lib d3d12.lib d3dcompiler.lib dxgi.lib advapi32.lib bcrypt.lib version.lib || exit /b 1
exit /b %errorlevel%
:load_visual_studio
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b %errorlevel%
