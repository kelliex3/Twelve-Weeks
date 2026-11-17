@echo off
rem Regenerates the Visual Studio solution for this folder's location.
rem Run after moving/copying the project (the old build dir bakes in absolute
rem paths, so it must be rebuilt from scratch).
rem
rem   regenerate-solution.bat          VS2022 -> build\tlou-ii-mp.sln
rem   regenerate-solution.bat 2026     VS2026 -> build-vs2026\tlou-ii-mp.slnx
rem
rem Needs on PATH: cmake, python, gui2.exe, ndarc.exe.
rem Needs installed: the "C++ Clang tools for Windows" VS component.
setlocal
cd /d "%~dp0"

rem VS2026 writes the new XML solution format (.slnx); VS2022 writes .sln.
set PRESET=vs2022
set OUTDIR=build
set SLN=tlou-ii-mp.sln
if /i "%~1"=="2026" (
    set PRESET=vs2026
    set OUTDIR=build-vs2026
    set SLN=tlou-ii-mp.slnx
)

if exist "%OUTDIR%" rmdir /s /q "%OUTDIR%"
cmake --preset %PRESET%
if errorlevel 1 (
    echo.
    echo Configure FAILED - read the error above.
    pause
    exit /b 1
)
echo.
echo Done - open %OUTDIR%\%SLN%
pause
