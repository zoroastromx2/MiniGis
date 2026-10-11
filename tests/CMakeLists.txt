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
