@echo off
rem Builds build\release\dst_compare.exe. Needs a prior release build, and LIB_DST pointing to src\libdstdec of foo_input_sacd.
setlocal
if not defined LIB_DST set "LIB_DST=..\foo_input_sacd\src\libdstdec"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\.."
cl /nologo /std:c++20 /O2 /EHsc /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I src /I "%LIB_DST%" /I "%LIB_DST%\decoder" tools\measure\dst_compare.cpp "%LIB_DST%\decoder\decoder.cpp" build\release\arpegio_core.lib /Fo:build\release\ /Fe:build\release\dst_compare.exe
