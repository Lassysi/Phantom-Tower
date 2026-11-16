@echo off
chcp 65001 >nul
rem Build PBZCore.dll -> build\core\PBZCore.dll (needs E:\VSBuildTools)
rem Every .cpp under features\ is compiled automatically; new files in res\ world\ cam\ need no script change
setlocal enabledelayedexpansion
call "E:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (echo vcvars64 not found & exit /b 1)
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\core
set DEFS=
rem build.bat release: mod-only release core (PBZ_MOD_ONLY, no trainer switches, autoloads PBZTower.dll beside it)
if /i "%~1"=="release" (set "OUT=%ROOT%\build\release-mod\core" & set "DEFS=/DPBZ_MOD_ONLY")
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%~dp0"
rc /nologo /fo "%OUT%\PBZCore.res" host\version.rc || exit /b 1
set FEATURES=
for /r features %%f in (*.cpp) do set FEATURES=!FEATURES! "%%f"
cl /nologo /std:c++20 /utf-8 /EHa /O2 /Gy /Gw /MT /W3 /LD /Zi /Fd"%OUT%\PBZCore.compile.pdb" %DEFS% /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  host\dllmain.cpp host\modules.cpp services\ue.cpp services\gamethread.cpp services\attributes.cpp ^
  ipc\pipe.cpp services\hooks.cpp services\abshook.cpp services\process_event.cpp services\inventory.cpp services\diag.cpp services\native_exec.cpp !FEATURES! ^
  third_party\minhook\src\hook.c third_party\minhook\src\buffer.c third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde64.c "%OUT%\PBZCore.res" ^
  /Fo"%OUT%\\" /Fe"%OUT%\PBZCore.dll" /link /NOLOGO /DEBUG:FULL /OPT:REF /OPT:ICF /PDB:"%OUT%\PBZCore.pdb" user32.lib bcrypt.lib || exit /b 1
echo BUILD_OK %OUT%\PBZCore.dll
