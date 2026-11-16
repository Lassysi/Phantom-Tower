@echo off
setlocal
call "E:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
set "CORE=%~dp0..\..\.."
set "OUT=%~dp0..\..\..\..\..\build\core\res-loot-tests"
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"
cl /nologo /std:c++20 /utf-8 /EHa /Od /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX "%~dp0native_loot_test.cxx" "%CORE%\services\hooks.cpp" "%CORE%\services\abshook.cpp" "%CORE%\third_party\minhook\src\hook.c" "%CORE%\third_party\minhook\src\buffer.c" "%CORE%\third_party\minhook\src\trampoline.c" "%CORE%\third_party\minhook\src\hde\hde64.c" /Fe:"native_loot_test.exe" /link user32.lib
if errorlevel 1 (popd & exit /b 1)
"%OUT%\native_loot_test.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
