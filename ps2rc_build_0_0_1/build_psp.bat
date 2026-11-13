@echo off
rem build_psp.bat <ISO> <carpeta de salida> [opciones extra del conversor]
rem Construye la version PSP 0.0.1 de DBZ Budokai Tenkaichi 3 (SLUS-216.78) desde tu propia ISO.
rem Based on PS2RC by SandstoneDev - https://github.com/SandstoneDev/PS2RC
setlocal
set "KIT=%~dp0"
if "%~2"=="" (
  echo Uso: build_psp.bat "ruta\a\la.ISO" "carpeta\de\salida" [--toolchain carpeta] [--dry-run]
  exit /b 1
)
set "ISO=%~1"
set "OUT=%~2"
shift
shift
set "EXTRA="
:args
if "%~1"=="" goto run
set "EXTRA=%EXTRA% %1"
shift
goto args
:run
pushd "%KIT%PS2RC" || exit /b 1
python -m pip show pyelftools >nul 2>&1 || python -m pip install pyelftools pillow
python -m ps2rc.convert "%ISO%" --out "%OUT%" --work "%KIT%work" --no-splash %EXTRA%
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" (
  echo La conversion fallo ^(codigo %RC%^).
  exit /b %RC%
)
echo Listo: copia "%OUT%" a PSP\GAME\ de la Memory Stick.
endlocal
