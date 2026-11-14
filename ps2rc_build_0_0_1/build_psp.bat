@echo off
rem build_psp.bat <ISO> <carpeta de salida> [--fast] [opciones extra del conversor]
rem Construye la version PSP de DBZ Budokai Tenkaichi 3 (SLUS-216.78) desde tu propia ISO.
rem --fast: traduccion en modo ABI (games\SLUS_216.78_v1.00\fast.toml), su propia carpeta work_fast y el titulo
rem         "BT3 PSP fast" en el XMB, para probar las dos versiones lado a lado.
rem Based on PS2RC by SandstoneDev - https://github.com/SandstoneDev/PS2RC
setlocal
set "KIT=%~dp0"
set "PACK=%KIT%PS2RC\games\SLUS_216.78_v1.00"
if "%~2"=="" (
  echo Uso: build_psp.bat "ruta\a\la.ISO" "carpeta\de\salida" [--fast] [--toolchain carpeta] [--dry-run]
  exit /b 1
)
set "ISO=%~1"
set "OUT=%~2"
shift
shift
set "WORK=%KIT%work"
set "TITLE="
if exist "%PACK%\pack.private.toml" del "%PACK%\pack.private.toml"
if /i "%~1"=="--fast" (
  copy /y "%PACK%\fast.toml" "%PACK%\pack.private.toml" >nul
  set "WORK=%KIT%work_fast"
  set TITLE=--title "BT3 PSP fast"
  echo --fast: traduccion en modo ABI ^(fast.toml^), carpeta de trabajo work_fast
  shift
)
set "EXTRA="
:args
if "%~1"=="" goto run
set "EXTRA=%EXTRA% %1"
shift
goto args
:run
pushd "%KIT%PS2RC" || exit /b 1
python -m pip show pyelftools >nul 2>&1 || python -m pip install pyelftools pillow
python -m ps2rc.convert "%ISO%" --out "%OUT%" --work "%WORK%" --no-splash %TITLE% %EXTRA%
set "RC=%ERRORLEVEL%"
popd
if exist "%PACK%\pack.private.toml" del "%PACK%\pack.private.toml"
if not "%RC%"=="0" (
  echo La conversion fallo ^(codigo %RC%^).
  exit /b %RC%
)
echo Listo: copia "%OUT%" a PSP\GAME\ de la Memory Stick.
endlocal
