@echo off
rem Builds build\tap\asio_tap.dll. tools\measure\asio_tap.ps1 puts it in front of a driver.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\.."
if not exist build\tap mkdir build\tap
cl /nologo /std:c++20 /O2 /EHsc /MT /utf-8 /LD /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I third_party\asiosdk\common tools\measure\asio_tap.cpp /Fo:build\tap\ /Fe:build\tap\asio_tap.dll /link ole32.lib advapi32.lib
