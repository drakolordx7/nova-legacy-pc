@echo off
rem Runs the given command line inside the MSVC x64 developer environment (cl, link on PATH).
rem   tools\vsenv.cmd cmake --preset win-release
rem Below normal priority on a shared PC:
rem   start "" /belownormal /wait /b tools\vsenv.cmd cmake --build --preset win-release
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
%*
