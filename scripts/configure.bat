@echo off
rem ---------------------------------------------------------------------------
rem MiniGIS - configurar el proyecto con CMake
rem
rem Uso:  scripts\configure.bat [preset] [opciones extra de CMake]
rem       scripts\configure.bat                       (Debug, recomendado en Qt Creator)
rem       scripts\configure.bat windows-msvc-release
rem ---------------------------------------------------------------------------

setlocal

rem OJO: en cmd, SHIFT tambien desplaza %0, asi que la carpeta del script se
rem guarda antes de tocar los argumentos.
set "SCRIPT_DIR=%~dp0"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-msvc-debug"
shift

call "%SCRIPT_DIR%env.bat"
if errorlevel 1 exit /b 1

echo [minigis] Configurando el preset "%PRESET%"...
cmake --preset "%PRESET%" %1 %2 %3 %4 %5 %6 %7 %8 %9
if errorlevel 1 (
  echo.
  echo [error] La configuracion fallo. Revisa el mensaje de CMake de arriba.
  exit /b 1
)

echo.
echo [ok] Configurado en build\%PRESET%
echo      Siguiente paso:  scripts\build.bat %PRESET%
exit /b 0
