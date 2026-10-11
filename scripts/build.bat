@echo off
rem ---------------------------------------------------------------------------
rem MiniGIS - compilar
rem
rem Uso:  scripts\build.bat [preset]
rem ---------------------------------------------------------------------------

setlocal
set "SCRIPT_DIR=%~dp0"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-msvc-debug"

call "%SCRIPT_DIR%env.bat"
if errorlevel 1 exit /b 1

echo [minigis] Compilando el preset "%PRESET%"...
cmake --build --preset "%PRESET%" --parallel
if errorlevel 1 (
  echo.
  echo [error] La compilacion fallo.
  exit /b 1
)

echo.
echo [ok] Ejecutable: build\%PRESET%\bin\minigis.exe
echo      Pruebas:    ctest --preset %PRESET%
echo      Ejecutar:   scripts\run.bat %PRESET%
exit /b 0
