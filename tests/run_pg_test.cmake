#############################################################
# run_pg_test.cmake - pruebas PostGIS opt-in contra un servidor real
#
# Se registran siempre y se marcan como omitidas (SKIP_REGULAR_EXPRESSION
# "OMITIDA") si no estan las variables de entorno. Asi una maquina sin acceso al
# bdware no falla, y la que lo tiene solo necesita definirlas:
#
#   set MINIGIS_PG_TEST_CONNECTIONS=D:\...\Connections.xml
#   set MINIGIS_PG_TEST_URI=pg:dataware_FDAC/mgn_2025
#   set MINIGIS_PG_TEST_LAYER=pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_estatales
#   ctest --preset windows-msvc-release -R minigis.pg
#############################################################

if(NOT MINIGIS_EXE)
  message(FATAL_ERROR "Falta -DMINIGIS_EXE=<ruta al ejecutable>")
endif()

set(_connections "$ENV{MINIGIS_PG_TEST_CONNECTIONS}")
set(_uri "$ENV{MINIGIS_PG_TEST_URI}")
set(_layer "$ENV{MINIGIS_PG_TEST_LAYER}")

if(MODE STREQUAL "check")
  if(_connections STREQUAL "" OR _uri STREQUAL "")
    message("OMITIDA: define MINIGIS_PG_TEST_CONNECTIONS y MINIGIS_PG_TEST_URI")
    return()
  endif()

  execute_process(
    COMMAND "${MINIGIS_EXE}" --connections "${_connections}" --check-pg --json --layer "${_uri}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --check-pg real fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  foreach(_key IN ITEMS "\"ok\": true" "\"layers\"" "\"elapsedMs\"")
    if(NOT _out MATCHES "${_key}")
      message(FATAL_ERROR "La salida real de --check-pg no contiene ${_key}:\n${_out}")
    endif()
  endforeach()
  message(STATUS "minigis --check-pg contra el servidor real OK")

elseif(MODE STREQUAL "window")
  if(_connections STREQUAL "" OR _layer STREQUAL "")
    message("OMITIDA: define MINIGIS_PG_TEST_CONNECTIONS y MINIGIS_PG_TEST_LAYER")
    return()
  endif()

  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --connections "${_connections}" --snapshot-window "${OUT}"
            --layer "${_layer}" --size 1000x700
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --snapshot-window de PostGIS fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "No se genero la captura de la ventana: ${OUT}")
  endif()
  file(SIZE "${OUT}" _size)
  if(_size LESS 5000)
    message(FATAL_ERROR "La captura de la ventana parece vacia (${_size} bytes)")
  endif()
  message(STATUS "minigis --snapshot-window con PostGIS OK: ${_size} bytes")

elseif(MODE STREQUAL "bench")
  if(_connections STREQUAL "" OR _layer STREQUAL "")
    message("OMITIDA: define MINIGIS_PG_TEST_CONNECTIONS y MINIGIS_PG_TEST_LAYER")
    return()
  endif()

  execute_process(
    COMMAND "${MINIGIS_EXE}" --connections "${_connections}" --bench 12 --bench-budget 150 --json
            --layer "${_layer}" --size 900x600
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --bench de PostGIS fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  foreach(_key IN ITEMS "\"quality\"" "\"store\"" "\"budget_ok\": true")
    if(NOT _out MATCHES "${_key}")
      message(FATAL_ERROR "La salida de --bench no contiene ${_key}:\n${_out}")
    endif()
  endforeach()
  message(STATUS "minigis --bench de PostGIS OK")

else()
  message(FATAL_ERROR "MODE no soportado: ${MODE}")
endif()
