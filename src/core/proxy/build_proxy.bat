@echo off
rem 构建发布版代理 avrt.dll → build\release-mod\avrt.dll（需 E:\VSBuildTools）
call "E:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (echo vcvars64 not found & exit /b 1)
set OUT=%~dp0..\..\..\build\release-mod
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cd /d "%~dp0"
rc /nologo /d PBZ_KIND_AVRT /fo "%OUT%\obj\avrt.res" ..\host\version.rc || exit /b 1
ml64 /nologo /c /Fo"%OUT%\obj\avrt_stubs.obj" avrt_stubs.asm || exit /b 1
cl /nologo /std:c++20 /utf-8 /O2 /MT /W4 /LD /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN avrt_proxy.cpp "%OUT%\obj\avrt_stubs.obj" "%OUT%\obj\avrt.res" ^
  /Fo"%OUT%\obj\\" /Fe"%OUT%\avrt.dll" /link /NOLOGO /DEF:avrt.def /OPT:REF /OPT:ICF || exit /b 1
del "%OUT%\avrt.exp" "%OUT%\avrt.lib" 2>nul
echo PROXY_OK %OUT%\avrt.dll
