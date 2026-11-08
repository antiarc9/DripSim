@echo off
rem Builds DripSim.dll at below-normal priority so a running game keeps the CPU.
rem Usage: scripts\build_plugin.cmd [jobs]   (default 8 parallel jobs)
setlocal
set "JOBS=%~1"
if "%JOBS%"=="" set "JOBS=8"
set "ROOT=%~dp0.."
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCPKG_ROOT set "VCPKG_ROOT=%VS%\VC\vcpkg"
set "VCPKG_MAX_CONCURRENCY=%JOBS%"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%ROOT%\plugin" || exit /b 1
start "" /belownormal /b /wait cmd /c "cmake --preset release && cmake --build build/release -j %JOBS%"
exit /b %ERRORLEVEL%
