@echo off
rem Builds the Release preset, runs the tests and writes build\dist\installer\Arpegio-VERSION-x64.exe.
rem Needs Inno Setup 6: winget install --id JRSoftware.InnoSetup --scope user
setlocal
call "%~dp0build.bat" dist test
if errorlevel 1 exit /b 1
cd /d "%~dp0.."

set ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" (
    echo Inno Setup 6 is not installed.
    exit /b 1
)

set VERSION=
for /f "tokens=2 delims==" %%v in ('findstr /b "CMAKE_PROJECT_VERSION:" build\dist\CMakeCache.txt') do set VERSION=%%v
if "%VERSION%"=="" (
    echo The version is not in the CMake cache.
    exit /b 1
)

if exist build\dist\package rmdir /s /q build\dist\package
cmake --install build\dist --prefix build\dist\package
if errorlevel 1 exit /b 1
"%ISCC%" /Q /DVersion=%VERSION% "/DSource=%CD%\build\dist\package" installer\arpegio.iss
