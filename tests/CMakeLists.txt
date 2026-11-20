############################################################
# Pruebas de humo de MiniGIS (CTest)
#
# Verifican el contrato del ejecutable: ayuda, version, lectura real de un
# shapefile con GDAL, render a PNG sin pantalla y codigos de salida.

set(MINIGIS_SAMPLE_SHP "${MINIGIS_ROOT_DIR}/data/sample/shapefiles/cities.shp")

add_test(NAME minigis.help COMMAND minigis --help)
add_test(NAME minigis.version COMMAND minigis --version)

add_test(NAME minigis.info
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_SHP}"
          -DMODE=info
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.snapshot
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_SHP}"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/cities.png"
          -DMODE=snapshot
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.salida-por-error-de-archivo
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${CMAKE_BINARY_DIR}/test-output/no-existe.shp"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/no-existe.png"
          "-DEXPECT_EXIT=2"
          -DMODE=snapshot
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.info minigis.snapshot PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- GeoPackage multicapa ------------------------------------------------
# Al abrir un contenedor con varias capas (un GeoPackage) deben cargarse TODAS:
# el fallo original solo mostraba la primera, tanto en el mapa como en el panel.
# El archivo de ejemplo tiene cuatro capas (cities, parcels, parcels_utm30 y
# area_sur, esta ultima muy lejos de las demas para que el encuadre inicial tenga
# que cubrir la union y no solo la primera capa).
set(MINIGIS_SAMPLE_GPKG "${MINIGIS_ROOT_DIR}/data/sample/geopackage/ejemplo.gpkg")

add_test(NAME minigis.geopackage
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GPKG}"
          -DMODE=geopackage
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.panel-capas
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GPKG}"
          -DMODE=check-layers
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.geopackage minigis.panel-capas PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- Banco de pruebas de rendimiento (--bench) ----------------------------
# Contrato del modo de medida: salida JSON estable y presupuesto respetado. Se
# ejecuta sobre los datos de ejemplo, que son diminutos: sirve para detectar
# roturas del contrato, no para medir rendimiento real.
add_test(NAME minigis.bench
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_SHP}"
          -DFRAMES=5
          -DSCENARIO=mixed
          -DBUDGET=500
          -DMODE=bench
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

# Puerta de regresion de rendimiento real. Al configurar con una capa pesada:
#   cmake ... -DMINIGIS_BENCH_DATASET=C:/datos/parcelas.gpkg -DMINIGIS_BENCH_BUDGET_MS=40
# se mide el p90 por cuadro de paneo/zoom sobre datos grandes y la prueba falla
# (codigo 6) si se supera el presupuesto. Asi una regresion de rendimiento rompe
# la compilacion y no la paciencia del usuario.
set(MINIGIS_BENCH_DATASET "" CACHE FILEPATH "Capa pesada para la puerta de rendimiento")
set(MINIGIS_BENCH_BUDGET_MS "150" CACHE STRING "Presupuesto de ms por cuadro (p90) para MINIGIS_BENCH_DATASET")

if(MINIGIS_BENCH_DATASET)
  add_test(NAME minigis.rendimiento
    COMMAND ${CMAKE_COMMAND}
            "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
            "-DSAMPLE=${MINIGIS_BENCH_DATASET}"
            "-DFRAMES=24"
            -DSCENARIO=mixed
            "-DBUDGET=${MINIGIS_BENCH_BUDGET_MS}"
            -DMODE=bench
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")
  set_tests_properties(minigis.rendimiento PROPERTIES TIMEOUT 900 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Indice de envolventes: comprueba que las envolventes coinciden con GDAL y que
# la capa conserva sus atributos despues de indexarla (regresion real: la tabla
# de atributos mostraba "(nulo)" en todas las filas).
add_test(NAME minigis.indice-envolventes
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_SHP}"
          -DMODE=index-shp
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

# Redibujado al cambiar simbologia o visibilidad: el lienzo cachea el ultimo
# cuadro para el paneo, y esa cache no debe servir una imagen obsoleta.
add_test(NAME minigis.redibujado
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_SHP}"
          -DMODE=check-refresh
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

# Etiquetado: campos, fuente, halo, escalas, solapes, repeticiones y la cache de
# textos. Se ejecuta en la plataforma sin ventana (la prueba comprueba la
# semantica de cada opcion; las comprobaciones de pixeles se omiten solas si la
# plataforma no dibuja texto).
add_test(NAME minigis.etiquetas
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_ROOT_DIR}/data/sample/shapefiles/parcels.shp"
          -DMODE=check-labels
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.etiquetas PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(minigis.bench PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(minigis.indice-envolventes PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(minigis.redibujado PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- Raster ---------------------------------------------------------------
# Contrato de --info y del dibujado raster, mas las capturas de interfaz y del
# dialogo de estilo. Los datos son los GeoTIFF de data/sample/rasters.
set(MINIGIS_SAMPLE_RASTER_DIR "${MINIGIS_ROOT_DIR}/data/sample/rasters")
set(MINIGIS_RASTER_ORTHO "${MINIGIS_SAMPLE_RASTER_DIR}/ortho_rgb.tif")
set(MINIGIS_RASTER_DEM "${MINIGIS_SAMPLE_RASTER_DIR}/dem_utm30.tif")
set(MINIGIS_RASTER_PALETTE "${MINIGIS_SAMPLE_RASTER_DIR}/landcover.tif")

add_test(NAME minigis.raster-info
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_RASTER_ORTHO}"
          "-DSAMPLE2=${MINIGIS_RASTER_DEM}"
          -DMODE=raster-info
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.raster-dibujo
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_RASTER_ORTHO}"
          "-DSAMPLE2=${MINIGIS_RASTER_DEM}"
          "-DSAMPLE3=${MINIGIS_RASTER_PALETTE}"
          -DMODE=check-raster
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.raster-dialogo
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_RASTER_DEM}"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/raster-dialog.png"
          -DMODE=raster-dialog
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.raster-ventana
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_RASTER_ORTHO}"
          "-DSAMPLE2=${MINIGIS_ROOT_DIR}/data/sample/shapefiles/parcels.shp"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/raster-window.png"
          -DMODE=snapshot-window
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.raster-info minigis.raster-dibujo minigis.raster-dialogo minigis.raster-ventana
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- Identificar ----------------------------------------------------------
# Contrato del clic: geometria exacta (huecos incluidos), radio en pixeles,
# alcance del arbol, panel, marcador y cache; consulta puntual por CLI y captura
# de la interfaz con resultados.
set(MINIGIS_SAMPLE_PARCELS "${MINIGIS_ROOT_DIR}/data/sample/shapefiles/parcels.shp")
set(MINIGIS_SAMPLE_CITIES "${MINIGIS_ROOT_DIR}/data/sample/shapefiles/cities.shp")
set(MINIGIS_SAMPLE_UTM "${MINIGIS_ROOT_DIR}/data/sample/shapefiles/parcels_utm30.shp")

add_test(NAME minigis.identificar
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_PARCELS}"
          "-DSAMPLE2=${MINIGIS_SAMPLE_CITIES}"
          "-DSAMPLE3=${MINIGIS_RASTER_ORTHO}"
          -DMODE=check-identify
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.identificar-cli
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_PARCELS}"
          "-DSAMPLE2=${MINIGIS_SAMPLE_CITIES}"
          "-DSAMPLE3=${MINIGIS_SAMPLE_UTM}"
          -DMODE=identify-at
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.identificar-ventana
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_PARCELS}"
          "-DSAMPLE2=${MINIGIS_SAMPLE_CITIES}"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/identify-window.png"
          -DMODE=identify-window
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.identificar minigis.identificar-cli minigis.identificar-ventana
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- Proyectos QGIS -------------------------------------------------------
# Lectura sin ventana del fixture: capas, orden, visibilidad, SRC, extension,
# simbologia, etiquetas y capa rota. El fixture se genera con
# tests/qgis/make_project_fixture.py y se versiona.
set(MINIGIS_SAMPLE_PROJECT "${MINIGIS_ROOT_DIR}/data/sample/projects/qgis_ejemplo.qgz")

add_test(NAME minigis.proyecto-info
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_PROJECT}"
          -DMODE=project-info
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.proyecto-info PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME minigis.proyecto-ventana
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_PROJECT}"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/proyecto-ventana.png"
          -DMODE=project-window
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.proyecto-copia
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_PROJECT}"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/proyecto-copia.qgz"
          -DMODE=save-project
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.proyecto-copia-qgs
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_PROJECT}"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/proyecto-copia.qgs"
          -DMODE=save-project
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.proyecto-ventana minigis.proyecto-copia minigis.proyecto-copia-qgs
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME minigis.proyecto
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_PROJECT}"
          -DMODE=check-project
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.proyecto PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MINIGIS_PROJECT_ARTIFACT=${CMAKE_BINARY_DIR}/test-output/proyecto-modificado.qgz")

# Las piezas de persistencia (arbol XML y ZIP) se prueban tambien directamente,
# sin pasar por la CLI: es la unica forma de comprobar la ida y vuelta del ZIP.
add_executable(minigisproyectoiotest minigisproyectoiotest.cpp)
target_link_libraries(minigisproyectoiotest PRIVATE MiniGIS::core Qt6::Core)

add_test(NAME minigis.proyecto-io
  COMMAND minigisproyectoiotest "${MINIGIS_SAMPLE_PROJECT}"
          "${CMAKE_BINARY_DIR}/test-output/proyecto-io.qgz")

# Puerta de rendimiento con un raster grande (opcional, como la del vectorial).
set(MINIGIS_RASTER_BENCH_DATASET "" CACHE FILEPATH "Raster grande para la puerta de rendimiento raster")
if(MINIGIS_RASTER_BENCH_DATASET)
  add_test(NAME minigis.rendimiento-raster
    COMMAND ${CMAKE_COMMAND}
            "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
            "-DSAMPLE=${MINIGIS_RASTER_BENCH_DATASET}"
            -DFRAMES=24
            -DSCENARIO=mixed
            "-DBUDGET=${MINIGIS_BENCH_BUDGET_MS}"
            -DMODE=bench
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")
  set_tests_properties(minigis.rendimiento-raster PROPERTIES
    TIMEOUT 900
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# --- KML / KMZ ------------------------------------------------------------
# Caso que fallaba: una MultiGeometry mixta acompanada de otro poligono se
# dibujaba sin ella y nadie lo avisaba. --check-kml compara lo que entrega OGR
# con lo que conserva el almacen de geometrias, de modo que la perdida rompe la
# prueba en vez de pasar inadvertida.
set(MINIGIS_SAMPLE_KML_DIR "${MINIGIS_ROOT_DIR}/data/sample/kml")

add_test(NAME minigis.kml-mundo
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_KML_DIR}/mundo.kml"
          -DMODE=kml
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.kml-mixto
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_KML_DIR}/mixto.kml"
          -DMODE=kml
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.kml-kmz
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_KML_DIR}/mixto.kmz"
          -DMODE=kml
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.kml-mundo minigis.kml-mixto minigis.kml-kmz
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Simbologia importada: dos estilos por elemento y un solo estilo. Comprueba
# ademas que el color no se lee al reves (LIBKML entrega #RRGGBBAA).
add_test(NAME minigis.kml-estilos
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_KML_DIR}/estilos_varios.kml"
          "-DEXPECT_STYLE_MODE=perFeature"
          "-DEXPECT_APPLIED=2"
          "-DEXPECT_COLOR=#ff0000"
          "-DEXPECT_WIDTH=3"
          -DMODE=kml-estilos
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.kml-estilo-unico
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_KML_DIR}/estilos.kml"
          "-DEXPECT_STYLE_MODE=single"
          "-DEXPECT_APPLIED=1"
          "-DEXPECT_COLOR=#ff0000"
          "-DEXPECT_WIDTH=4"
          -DMODE=kml-estilos
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.kml-estilos minigis.kml-estilo-unico
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Tope de estilos: con 70 estilos distintos manda el dominante y se informa, en
# lugar de pagar una pluma por elemento.
add_test(NAME minigis.kml-estilos-tope
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_KML_DIR}/estilos_muchos.kml"
          "-DEXPECT_STYLE_MODE=dominant"
          "-DEXPECT_APPLIED=1"
          -DMODE=kml-estilos
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.kml-estilos-tope PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Cargar un KML con la tabla de atributos abierta no debe bloquear la ventana:
# ajustar el ancho de columnas pedia atributos celda a celda y en KML eso es una
# relectura del origen por celda.
add_test(NAME minigis.kml-ventana
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_KML_DIR}/mixto.kml"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/kml-window.png"
          -DMODE=window-attributes
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

set_tests_properties(minigis.kml-ventana PROPERTIES
  TIMEOUT 120
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- GeoParquet -----------------------------------------------------------
# El motor solo puede abrir GeoParquet si GDAL se compilo con la feature
# `parquet` de vcpkg. Las pruebas cubren apertura, atributos, SRC, tipos
# mixtos, rechazos y el contrato de --check-geoparquet.
set(MINIGIS_SAMPLE_GEOPARQUET_DIR "${MINIGIS_ROOT_DIR}/data/sample/geoparquet")

add_test(NAME minigis.geoparquet-info
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/parcels.parquet"
          "-DEXPECT_FEATURES=5"
          "-DEXPECT_EPSG=4326"
          -DMODE=geoparquet-info
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.geoparquet-utm
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/parcels_utm30.parquet"
          "-DEXPECT_FEATURES=5"
          "-DEXPECT_EPSG=25830"
          -DMODE=geoparquet-info
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.geoparquet-snapshot
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/parcels.parquet"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/geoparquet.png"
          -DMODE=snapshot
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.geoparquet-check
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/parcels.parquet"
          "-DEXPECT_FEATURES=5"
          "-DEXPECT_STORE=5"
          "-DEXPECT_EPSG=4326"
          -DMODE=geoparquet
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

# La MultiGeometry mixta del KML, ahora escrita como GeoParquet: debe conservar
# el punto, la linea y los dos poligonos (uno dentro de la coleccion y otro
# suelto). Es el mismo caso que rompia el dibujado antes de la feature de KML.
add_test(NAME minigis.geoparquet-mixto
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/mixto.parquet"
          "-DEXPECT_FEATURES=2"
          "-DEXPECT_STORE=2"
          "-DEXPECT_POINTS=1"
          "-DEXPECT_LINES=1"
          "-DEXPECT_POLYGONS=2"
          -DMODE=geoparquet
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.geoparquet-sin-geometria
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/sin_geometria.parquet"
          "-DEXPECT_FEATURES=2"
          "-DEXPECT_WITHOUT=1"
          -DMODE=geoparquet
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.geoparquet-sin-crs
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/sin_crs.parquet"
          "-DEXPECT_FEATURES=5"
          "-DEXPECT_EPSG=0"
          -DEXPECT_CRS_UNKNOWN=1
          -DMODE=geoparquet
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.geoparquet-vacio
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/vacio.parquet"
          "-DEXPECT_FEATURES=0"
          -DMODE=geoparquet
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.geoparquet-fid
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/parcels_fid.parquet"
          "-DEXPECT_FEATURES=5"
          "-DEXPECT_FID=parcel_id"
          -DMODE=geoparquet
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

# Una tabla Parquet sin geometria no es un GeoParquet: codigo 5 y mensaje.
add_test(NAME minigis.geoparquet-tabla
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/tabla.parquet"
          "-DEXPECT_EXIT=5"
          "-DEXPECT_TEXT=no contiene ninguna columna de geometria"
          -DMODE=geoparquet-error
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

# Una carpeta con .parquet es una coleccion particionada: no se abre.
add_test(NAME minigis.geoparquet-carpeta
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}"
          "-DEXPECT_EXIT=3"
          "-DEXPECT_TEXT=carpeta con archivos .parquet"
          -DMODE=geoparquet-error
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

# Si el driver no esta compilado, el error debe decir que falta y como
# arreglarlo, no el «GDAL no pudo abrir» generico. GDAL_SKIP simula esa build.
add_test(NAME minigis.geoparquet-sin-driver
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DSAMPLE=${MINIGIS_SAMPLE_GEOPARQUET_DIR}/parcels.parquet"
          "-DEXPECT_EXIT=3"
          "-DEXPECT_TEXT=no incluye el driver Parquet"
          -DMODE=geoparquet-error
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")
set_tests_properties(minigis.geoparquet-sin-driver PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GDAL_SKIP=Parquet")

set_tests_properties(
  minigis.geoparquet-info minigis.geoparquet-utm minigis.geoparquet-snapshot
  minigis.geoparquet-check minigis.geoparquet-mixto minigis.geoparquet-sin-geometria
  minigis.geoparquet-sin-crs minigis.geoparquet-vacio minigis.geoparquet-fid
  minigis.geoparquet-tabla minigis.geoparquet-carpeta
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- PostGIS --------------------------------------------------------------
# Piezas que no necesitan servidor: URI pg:, catalogo Connections.xml, cadena
# de conexion y saneado de la contrasena. La integracion real es opt-in en
# tests/pg/ y se ejecuta solo si existen las variables MINIGIS_PG_TEST_*.
set(MINIGIS_SAMPLE_CONNECTIONS "${MINIGIS_ROOT_DIR}/data/sample/postgis/Connections.xml")

add_executable(minigispgtest minigispgtest.cpp)
target_link_libraries(minigispgtest PRIVATE MiniGIS::core Qt6::Core)

add_test(NAME minigis.pg-catalogo
  COMMAND minigispgtest "${MINIGIS_SAMPLE_CONNECTIONS}")

# Casos deterministas sin servidor: catalogo ausente (2), conexion desconocida
# (8) y uso incorrecto de --check-pg (1). Ninguno abre una conexion de red.
add_test(NAME minigis.pg-sin-catalogo
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DCONNECTIONS=${CMAKE_BINARY_DIR}/test-output/no-existe.xml"
          "-DSAMPLE=pg:no_existe/mgn_2025/areas_geoestadisticas_estatales"
          -DEXPECT_EXIT=2
          -DMODE=pg-info
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.pg-conexion-inexistente
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DCONNECTIONS=${MINIGIS_SAMPLE_CONNECTIONS}"
          "-DSAMPLE=pg:no_existe/mgn_2025/areas_geoestadisticas_estatales"
          -DEXPECT_EXIT=8
          -DMODE=pg-info
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.pg-cli-uso
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          -DEXPECT_EXIT=1
          -DMODE=pg-uso
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")

add_test(NAME minigis.pg-dialogo
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DCONNECTIONS=${MINIGIS_SAMPLE_CONNECTIONS}"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/pg-dialog.png"
          -DMODE=pg-dialog
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_test.cmake")
set_tests_properties(minigis.pg-dialogo PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Integracion real opt-in: se registran siempre y se omiten si no estan las
# variables MINIGIS_PG_TEST_* (SKIP_REGULAR_EXPRESSION). Ver run_pg_test.cmake.
add_test(NAME minigis.pg
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          -DMODE=check
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_pg_test.cmake")
set_tests_properties(minigis.pg PROPERTIES SKIP_REGULAR_EXPRESSION "OMITIDA")

add_test(NAME minigis.pg-window
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          "-DOUT=${CMAKE_BINARY_DIR}/test-output/pg-window.png"
          -DMODE=window
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_pg_test.cmake")
set_tests_properties(minigis.pg-window PROPERTIES
  SKIP_REGULAR_EXPRESSION "OMITIDA"
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME minigis.pg-bench
  COMMAND ${CMAKE_COMMAND}
          "-DMINIGIS_EXE=$<TARGET_FILE:minigis>"
          -DMODE=bench
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_pg_test.cmake")
set_tests_properties(minigis.pg-bench PROPERTIES SKIP_REGULAR_EXPRESSION "OMITIDA")
