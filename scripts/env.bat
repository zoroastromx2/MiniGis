@echo off
rem ---------------------------------------------------------------------------
rem MiniGIS - entorno de compilacion (se invoca desde el resto de scripts)
rem
rem Prepara en esta consola:
rem   * compilador MSVC 2022 x64 (vcvars64.bat)
rem   * PATH con las DLLs de Qt 6.10.3 y de las dependencias de vcpkg
rem   * VCPKG_ROOT y MINIGIS_QT_DIR para CMake
rem
rem Si tus rutas son distintas, cambia las variables de abajo.
rem ---------------------------------------------------------------------------

set "MINIGIS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%MINIGIS_VCVARS%" set "MINIGIS_VCVARS=E:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%MINIGIS_VCVARS%" (
  echo [error] No se encuentra vcvars64.bat de Visual Studio 2022.
  echo         Edita MINIGIS_VCVARS en scripts\env.bat con la ruta correcta.
  exit /b 1
)

call "%MINIGIS_VCVARS%" >nul
if errorlevel 1 (
  echo [error] vcvars64.bat fallo.
  exit /b 1
)

rem MSVC en espanol escribe las notas de /showIncludes como "Nota: ..." y CMake
rem no las reconoce: con VSLANG en ingles la salida de compilacion queda limpia.
set "VSLANG=1033"

set "QT_ROOT=D:\QtWin\6.10.3\msvc2022_64"
set "VCPKG_ROOT=E:\vcpkg"
set "MINIGIS_QT_DIR=%QT_ROOT%\lib\cmake\Qt6"
set "PATH=%QT_ROOT%\bin;%VCPKG_ROOT%\installed\x64-windows\bin;%PATH%"

if not exist "%QT_ROOT%\bin\Qt6Core.dll" (
  echo [error] No se encuentra Qt en "%QT_ROOT%".
  echo         Edita QT_ROOT en scripts\env.bat.
  exit /b 1
)
if not exist "%VCPKG_ROOT%\installed\x64-windows\share\gdal\GDALConfig.cmake" (
  echo [error] vcpkg no tiene GDAL instalado en "%VCPKG_ROOT%\installed\x64-windows".
  echo         Ejecuta:  "%VCPKG_ROOT%\vcpkg.exe" install gdal --triplet x64-windows
  exit /b 1
)

exit /b 0
