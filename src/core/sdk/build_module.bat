@echo off
chcp 65001 >nul
rem 编译功能模块：build_module.bat <模块名> <源码目录> [输出目录]
rem 源码目录下所有 .cpp + sdk\module_shim.cpp → <输出目录>\<模块名>.dll（默认 build\core\modules）
rem 额外宏用环境变量 PBZ_MODULE_DEFS（发布版：/DPBZ_RELEASE）
rem 编译选项必须和运行库（src\core\build.bat）一致，见 sdk\pbz_module.h 顶部的约束
setlocal enabledelayedexpansion
if "%~2"=="" (echo 用法：build_module.bat 模块名 源码目录 [输出目录] & exit /b 2)
call "E:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (echo vcvars64 not found & exit /b 1)
set NAME=%~1
set SRC=%~f2
set OUT=%~f3
if "%OUT%"=="" set OUT=%~dp0..\..\..\build\core\modules
if not exist "%OUT%\obj-%NAME%" mkdir "%OUT%\obj-%NAME%"
rc /nologo /d PBZ_KIND_MODULE /fo "%OUT%\obj-%NAME%\%NAME%.res" "%~dp0..\host\version.rc" || exit /b 1
set FILES=
for /r "%SRC%" %%f in (*.cpp) do set FILES=!FILES! "%%f"
cl /nologo /std:c++20 /utf-8 /EHa /O2 /Gy /Gw /MT /W3 /LD /Zi /DPBZ_MODULE %PBZ_MODULE_DEFS% /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  /Fd"%OUT%\obj-%NAME%\%NAME%.compile.pdb" "%~dp0module_shim.cpp" !FILES! "%OUT%\obj-%NAME%\%NAME%.res" ^
  /Fo"%OUT%\obj-%NAME%\\" /Fe"%OUT%\%NAME%.dll" /link /NOLOGO /DEBUG:FULL /OPT:REF /OPT:NOICF /INCREMENTAL:NO %PBZ_MODULE_LINK% /PDB:"%OUT%\%NAME%.pdb" || exit /b 1
echo MODULE_OK %OUT%\%NAME%.dll
