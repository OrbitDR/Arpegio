@echo off
rem Usage: scripts\build.bat [release|debug] [test]
setlocal
set PRESET=%~1
if "%PRESET%"=="" set PRESET=release
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
cmake --preset %PRESET% >nul
if errorlevel 1 exit /b 1
cmake --build --preset %PRESET%
if errorlevel 1 exit /b 1
if /i "%~2"=="test" ctest --preset %PRESET%
