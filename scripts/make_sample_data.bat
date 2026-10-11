@echo off
rem ---------------------------------------------------------------------------
rem MiniGIS - generar los shapefiles de ejemplo a partir de los GeoJSON de
rem data\sample usando ogr2ogr (viene con OSGeo4W o con cualquier GDAL).
rem
rem Uso:  scripts\make_sample_data.bat
rem ---------------------------------------------------------------------------

setlocal

set "OGR2OGR=E:\OSGeo4W\bin\ogr2ogr.exe"
if not exist "%OGR2OGR%" (
  for /f "delims=" %%i in ('where ogr2ogr 2^>nul') do set "OGR2OGR=%%i"
)
if not exist "%OGR2OGR%" (
  echo [error] No se encontro ogr2ogr.
  echo         Instala GDAL/OSGeo4W o anade ogr2ogr al PATH.
  exit /b 1
)

set "ROOT=%~dp0.."
set "SRC=%ROOT%\data\sample"
set "DST=%SRC%\shapefiles"
if not exist "%DST%" mkdir "%DST%"

echo [minigis] Generando shapefiles en "%DST%"...

"%OGR2OGR%" -f "ESRI Shapefile" "%DST%\cities.shp" "%SRC%\cities.geojson" ^
  -a_srs EPSG:4326 -lco ENCODING=UTF-8 -nln cities -overwrite
if errorlevel 1 goto :error

"%OGR2OGR%" -f "ESRI Shapefile" "%DST%\parcels.shp" "%SRC%\parcels.geojson" ^
  -a_srs EPSG:4326 -lco ENCODING=UTF-8 -nln parcels -overwrite
if errorlevel 1 goto :error

rem Copia reproyectada a UTM 30N: sirve para comprobar la reproyeccion al vuelo.
rem (ogr2ogr no admite -a_srs junto a -t_srs: el origen se declara con -s_srs).
"%OGR2OGR%" -f "ESRI Shapefile" "%DST%\parcels_utm30.shp" "%SRC%\parcels.geojson" ^
  -s_srs EPSG:4326 -t_srs EPSG:25830 -lco ENCODING=UTF-8 -nln parcels_utm30 -overwrite
if errorlevel 1 goto :error

echo.
echo [ok] Datos de ejemplo listos:
dir /b "%DST%"
exit /b 0

:error
echo.
echo [error] ogr2ogr fallo al generar los datos de ejemplo.
exit /b 1
