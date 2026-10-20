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
