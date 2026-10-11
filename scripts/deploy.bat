@echo off
rem ---------------------------------------------------------------------------
rem MiniGIS - crear una carpeta "dist" autonoma (Qt, dependencias de vcpkg y
rem datos de PROJ/GDAL) para repartir el programa.
rem
rem Uso:  scripts\deploy.bat [preset]
rem ---------------------------------------------------------------------------

setlocal
set "SCRIPT_DIR=%~dp0"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-msvc-release"

call "%SCRIPT_DIR%env.bat"
if errorlevel 1 exit /b 1

set "ROOT=%SCRIPT_DIR%.."
set "BUILD=%ROOT%\build\%PRESET%"
set "DIST=%ROOT%\dist\%PRESET%"

if not exist "%BUILD%\bin\minigis.exe" (
  echo [error] No existe "%BUILD%\bin\minigis.exe".
  echo         Compila primero:  scripts\build.bat %PRESET%
  exit /b 1
)

echo [minigis] Preparando "%DIST%"...
if not exist "%DIST%" mkdir "%DIST%"

copy /Y "%BUILD%\bin\minigis.exe" "%DIST%" >nul
copy /Y "%BUILD%\bin\*.dll" "%DIST%" >nul

set "WINDEPLOYQT_ARGS=--no-translations --no-compiler-runtime"
echo %PRESET% | findstr /I "debug" >nul && set "WINDEPLOYQT_ARGS=%WINDEPLOYQT_ARGS% --debug"
"%QT_ROOT%\bin\windeployqt.exe" %WINDEPLOYQT_ARGS% "%DIST%\minigis.exe"
if errorlevel 1 (
  echo [error] windeployqt fallo.
  exit /b 1
)

rem windeployqt no copia el plugin de plataforma "offscreen", necesario para los
rem modos sin ventana (--info, --snapshot) y para ejecutar las pruebas.
if not exist "%DIST%\platforms" mkdir "%DIST%\platforms"
copy /Y "%QT_ROOT%\plugins\platforms\qoffscreen*.dll" "%DIST%\platforms" >nul 2>nul

rem Datos de PROJ y GDAL: se copian junto al ejecutable para que la aplicacion
rem los encuentre sola (ver src/core/minigisgdalinit.cpp).
xcopy /E /I /Y /Q "%VCPKG_ROOT%\installed\x64-windows\share\proj" "%DIST%\proj" >nul
xcopy /E /I /Y /Q "%VCPKG_ROOT%\installed\x64-windows\share\gdal" "%DIST%\gdal" >nul

rem Datos de ejemplo
if exist "%ROOT%\data\sample\shapefiles" (
  xcopy /E /I /Y /Q "%ROOT%\data\sample\shapefiles" "%DIST%\sample" >nul
)

echo.
echo [ok] Distribucion lista en "%DIST%"
echo      Prueba:  "%DIST%\minigis.exe" --version
exit /b 0
