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

set_tests_properties(minigis.bench PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
