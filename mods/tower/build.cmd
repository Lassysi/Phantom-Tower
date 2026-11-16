@echo off
rem Build the tower module: build\core\modules\PBZTower.dll (same toolchain as the runtime, see src\core\sdk\README.md)
python -I "%~dp0tools\gen_pool.py" --check || exit /b 1
python -I "%~dp0tools\gen_growth.py" --check || exit /b 1
python -X utf8 -I "%~dp0tools\gen_terminal_flows.py" --check || exit /b 1
python -X utf8 -I "%~dp0tools\gen_encounter_dependencies.py" --check || exit /b 1
python -I "%~dp0tools\gen_travel.py" --check || exit /b 1
python -I "%~dp0tools\gen_native.py" --check || exit /b 1
python -I "%~dp0tools\gen_progression_catalog.py" --check || exit /b 1
python -X utf8 -I "%~dp0tools\gen_venues.py" --check || exit /b 1
python -X utf8 -I "%~dp0tools\gen_boss_drops.py" --check || exit /b 1
python -X utf8 -I "%~dp0tools\floors_plan.py" --header --check >/dev/null || exit /b 1
call "%~dp0..\..\src\core\sdk\build_module.bat" PBZTower "%~dp0src"
