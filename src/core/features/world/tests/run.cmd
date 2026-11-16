@echo off
setlocal
call "E:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
set "OUT=%~dp0..\..\..\..\..\build\core\enemy-spawn-tests"
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"
cl /nologo /std:c++20 /utf-8 /EHa /W4 /Od /DNOMINMAX /DWIN32_LEAN_AND_MEAN "%~dp0spawn_test.cxx" /Fe:spawn_test.exe
if errorlevel 1 (popd & exit /b 1)
"%OUT%\spawn_test.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
