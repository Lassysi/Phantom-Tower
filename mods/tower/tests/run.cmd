@echo off
setlocal
call "E:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 1
set "OUT=%~dp0..\..\..\build\tower-tests"
if not exist "%OUT%" mkdir "%OUT%"
rem Tests use their own LOCALAPPDATA (language tests write files); never touch the player's PBZTower folder
set "LOCALAPPDATA=%OUT%\fixture-localappdata"
if not exist "%LOCALAPPDATA%" mkdir "%LOCALAPPDATA%"
pushd "%OUT%"
set "TESTS=logic_test special_test playstyle_test endless_share_test growth_test trade_test growth_effect_test leech_test burn_test bar_math_test arcane_charge_test arcane_hit_sig_test hex_test hex_qa_test hex_persist_test hex_qa_persist_test attack_speed_test weapon_cap_sig_test weapon_power_sig_test seed_test seed_repro_test travel_test sandbox_test save_test flow_test wave_stress_test summon_permit_test points_test rewards_test plan_test modal_test replay_entry_test encounter_dependencies_test storygate_test preload_runtime_test replay_group_test vm_bridge_test vm_dispatch_test spawn_receipt_test combatlog_test reach_runtime_test venue_runtime_test save_compat_test mods_test hotkey_test sony_pad_test language_test language_run_test save_upgrade_test"
if not "%~1"=="" set "TESTS=%~1"
for %%t in (%TESTS%) do (
  call :test %%t
  if errorlevel 1 goto failed
)
if "%~1"=="" (
  python -I "%~dp0boss_baseline_test.py"
  if errorlevel 1 goto failed
  python -I "%~dp0..\tools\test_cmd_whitelist.py"
  if errorlevel 1 goto failed
)
popd
exit /b 0

:test
set "SRC=%~dp0%1.cxx"
if not exist "%SRC%" set "SRC=%~dp0%1.cpp"
cl /nologo /std:c++20 /utf-8 /EHa /W4 /Od /DNOMINMAX /DWIN32_LEAN_AND_MEAN "%SRC%" /Fe:%1.exe >"%OUT%\compile-%1.log" 2>&1
if errorlevel 1 (
  type "%OUT%\compile-%1.log"
  exit /b 1
)
"%OUT%\%1.exe"
if not "%errorlevel%"=="0" exit /b 1
exit /b 0

:failed
popd
exit /b 1
