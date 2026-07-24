@echo off
setlocal EnableExtensions
cd /d "%~dp0"

where cl.exe >nul 2>&1
if errorlevel 1 call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

if not exist bin mkdir bin

rem DXGI proxy
cl /nologo /O2 /LD /Fo:"bin\nvcloak_dxgi.obj" /Fe:"bin\dxgi.dll" "src\nvcloak.c" /link /DEF:"src\nvcloak_dxgi.def" kernel32.lib user32.lib ole32.lib uuid.lib dxgi.lib dxguid.lib
if errorlevel 1 exit /b 1

rem Version API proxy
ml64 /nologo /c /Fo"bin\version_stubs.obj" "src\version_stubs.asm"
if errorlevel 1 exit /b 1
cl /nologo /O2 /LD /DPROXY_VERSION /Fo:"bin\nvcloak_version.obj" /Fe:"bin\version.dll" "src\nvcloak.c" "bin\version_stubs.obj" /link /DEF:"src\version_proxy.def" kernel32.lib user32.lib ole32.lib uuid.lib dxgi.lib dxguid.lib
if errorlevel 1 exit /b 1

rem Generate and build additional forwarding proxies in temporary bin\obj paths.
if exist "bin\obj" rmdir /s /q "bin\obj"
for %%D in (winmm dbghelp d3d12 wininet winhttp) do (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "src\build_forwarding_proxy.ps1" -Name %%D
    if errorlevel 1 exit /b 1
)

copy /y "bin\dxgi.dll" "bin\nvcloak.asi" >nul
del /q bin\*.obj bin\*.exp bin\*.lib 2>nul
rmdir /s /q "bin\obj" 2>nul

echo.
echo Built injection files in: %~dp0bin
for %%F in (bin\*.dll bin\*.asi) do echo   %%F
