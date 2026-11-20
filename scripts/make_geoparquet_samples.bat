@echo off
rem ---------------------------------------------------------------------------
rem MiniGIS - genera los GeoParquet de ejemplo de data\sample\geoparquet
rem
rem Uso:  scripts\make_geoparquet_samples.bat
rem
rem Necesita un ogr2ogr con driver Parquet de ESCRITURA (OSGeo4W 3.12.x lo
rem trae) y, para los casos que GDAL no escribe (sin CRS, tabla, vacio),
rem Python con pyarrow y geopandas (el Python de OSGeo4W los trae).
rem
rem Los .parquet resultantes se versionan. Este script no se ejecuta en ctest:
rem solo sirve para regenerarlos de forma reproducible.
rem ---------------------------------------------------------------------------

setlocal

set "SCRIPT_DIR=%~dp0"
set "ROOT=%SCRIPT_DIR%.."
set "SRC=%ROOT%\data\sample"
set "DST=%SRC%\geoparquet"

set "OGR2OGR=D:\OSGeo4w\bin\ogr2ogr.exe"
if not exist "%OGR2OGR%" (
  for /f "delims=" %%i in ('where ogr2ogr 2^>nul') do set "OGR2OGR=%%i"
)
if not exist "%OGR2OGR%" (
  echo [error] No se encontro ogr2ogr con driver Parquet.
  echo         Instala GDAL/OSGeo4W 3.12 o superior y anade ogr2ogr al PATH.
  exit /b 1
)

set "PYTHON=D:\OSGeo4w\apps\Python312\python.exe"
if exist "%PYTHON%" (
  set "PROJ_DATA=D:\OSGeo4w\share\proj"
  set "GDAL_DATA=D:\OSGeo4w\share\gdal"
) else (
  set "PYTHON="
  for /f "delims=" %%i in ('where python 2^>nul') do (
    if not defined PYTHON (
      "%%i" -c "import pyarrow, geopandas" >nul 2>nul
      if not errorlevel 1 set "PYTHON=%%i"
    )
  )
)
if not defined PYTHON (
  echo [error] No se encontro Python con pyarrow y geopandas.
  echo         Usa el Python de OSGeo4W o instala: pip install pyarrow geopandas
  exit /b 1
)

if not exist "%DST%" mkdir "%DST%"

echo [minigis] Generando GeoParquet en "%DST%"...

rem --- Poligonos en 4326: el fixture base de atributos y CRS ----------------
"%OGR2OGR%" -f Parquet "%DST%\parcels.parquet" "%SRC%\parcels.geojson" ^
  -a_srs EPSG:4326 -lco "FID="
if errorlevel 1 goto :error

rem --- Los mismos poligonos con FID nombrado --------------------------------
"%OGR2OGR%" -f Parquet "%DST%\parcels_fid.parquet" "%SRC%\parcels.geojson" ^
  -a_srs EPSG:4326 -lco "FID=parcel_id"
if errorlevel 1 goto :error

rem --- Poligonos en UTM 30N: comprueba la reproyeccion al vuelo -------------
"%OGR2OGR%" -f Parquet "%DST%\parcels_utm30.parquet" "%SRC%\shapefiles\parcels_utm30.shp" ^
  -lco "FID="
if errorlevel 1 goto :error

rem --- Punto + linea + poligono en el mismo archivo -------------------------
"%OGR2OGR%" -f Parquet "%DST%\mixto.parquet" "%SRC%\kml\mixto.kml"
if errorlevel 1 goto :error

rem --- Una fila con geometria nula junto a otra con punto -------------------
"%OGR2OGR%" -f Parquet "%DST%\sin_geometria.parquet" "%DST%\sin_geometria.geojson" ^
  -a_srs EPSG:4326
if errorlevel 1 goto :error

rem --- Sin CRS, tabla sin geometria y capa vacia: los escribe Python --------
"%PYTHON%" "%SCRIPT_DIR%make_geoparquet_samples.py" "%SRC%" "%DST%"
if errorlevel 1 goto :error

echo.
echo [ok] GeoParquet generados en "%DST%"
exit /b 0

:error
echo.
echo [error] Fallo la generacion de GeoParquet. Revisa el mensaje de arriba.
exit /b 1
