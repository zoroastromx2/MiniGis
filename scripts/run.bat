@echo off
rem ---------------------------------------------------------------------------
rem MiniGIS - lanzar la aplicacion desde la linea de comandos
rem
rem Uso:  scripts\run.bat [preset] [argumentos para minigis]
rem       scripts\run.bat                                              (GUI vacia)
rem       scripts\run.bat windows-msvc-debug data\sample\shapefiles\cities.shp
rem       scripts\run.bat windows-msvc-debug --snapshot mapa.png -l data\sample\shapefiles\cities.shp
rem ---------------------------------------------------------------------------

setlocal
set "SCRIPT_DIR=%~dp0"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-msvc-debug"
shift

rem Se recogen los argumentos restantes (el preset ya se ha consumido).
set "ARGS="
:collect
if "%~1"=="" goto :ready
set "ARGS=%ARGS% %~1"
shift
goto :collect
:ready

call "%SCRIPT_DIR%env.bat"
if errorlevel 1 exit /b 1

set "EXE=%SCRIPT_DIR%..\build\%PRESET%\bin\minigis.exe"
if not exist "%EXE%" (
  echo [error] No existe "%EXE%".
  echo         Compila primero:  scripts\build.bat %PRESET%
  exit /b 1
)

"%EXE%" %ARGS%
exit /b %errorlevel%
