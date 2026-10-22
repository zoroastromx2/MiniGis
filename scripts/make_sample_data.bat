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

rem ---------------------------------------------------------------------------
rem GeoPackage con tres capas: es el archivo que comprueba que al abrir un
rem contenedor se cargan todas sus capas y el panel las agrupa bajo su nombre.
rem ---------------------------------------------------------------------------
set "GPKG_DST=%SRC%\geopackage"
if not exist "%GPKG_DST%" mkdir "%GPKG_DST%"
if exist "%GPKG_DST%\ejemplo.gpkg" del "%GPKG_DST%\ejemplo.gpkg"

"%OGR2OGR%" -f GPKG "%GPKG_DST%\ejemplo.gpkg" "%DST%\cities.shp" -nln cities
if errorlevel 1 goto :error

"%OGR2OGR%" -f GPKG -update -append "%GPKG_DST%\ejemplo.gpkg" "%DST%\parcels.shp" -nln parcels
if errorlevel 1 goto :error

"%OGR2OGR%" -f GPKG -update -append "%GPKG_DST%\ejemplo.gpkg" "%DST%\parcels_utm30.shp" -nln parcels_utm30
if errorlevel 1 goto :error

rem ---------------------------------------------------------------------------
rem GeoTIFF de ejemplo: los genera un script Python con GDAL. Si no hay Python
rem con GDAL se avisa y se continua: los .tif ya estan versionados.
rem ---------------------------------------------------------------------------
set "PYTHON="
if exist "D:\OSGeo4w\apps\Python312\python.exe" (
  set "PYTHON=D:\OSGeo4w\apps\Python312\python.exe"
  set "PYTHONHOME=D:\OSGeo4w\apps\Python312"
) else (
  for /f "delims=" %%i in ('where python 2^>nul') do (
    if not defined PYTHON (
      "%%i" -c "import osgeo" >nul 2>nul
      if not errorlevel 1 set "PYTHON=%%i"
    )
  )
)

if not defined PYTHON (
  echo [aviso] No se encontro Python con GDAL; los GeoTIFF de ejemplo ya estan versionados.
  goto :rasters_ok
)

echo [minigis] Generando GeoTIFF de ejemplo en "%SRC%\rasters"...
"%PYTHON%" "%ROOT%\scripts\make_sample_rasters.py"
if errorlevel 1 goto :raster_error

:rasters_ok
echo.
echo [ok] Datos de ejemplo listos:
dir /b "%DST%"
if exist "%SRC%\geopackage" dir /b "%SRC%\geopackage"
if exist "%SRC%\rasters" dir /b "%SRC%\rasters"
exit /b 0

:raster_error
echo.
echo [error] make_sample_rasters.py fallo al generar los GeoTIFF de ejemplo.
exit /b 1

:error
echo.
echo [error] ogr2ogr fallo al generar los datos de ejemplo.
exit /b 1
