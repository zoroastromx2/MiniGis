#############################################################
# run_cli_test.cmake - pruebas de humo del ejecutable MiniGIS
#
# Uso:
#   cmake -DMINIGIS_EXE=... -DMODE=info|snapshot -DSAMPLE=... [-DOUT=...]
#         [-DEXPECT_EXIT=0] -P run_cli_test.cmake
#############################################################

if(NOT MINIGIS_EXE)
  message(FATAL_ERROR "Falta -DMINIGIS_EXE=<ruta al ejecutable>")
endif()
if(NOT EXISTS "${MINIGIS_EXE}")
  message(FATAL_ERROR "No existe el ejecutable: ${MINIGIS_EXE}")
endif()
if(NOT EXPECT_EXIT)
  set(EXPECT_EXIT 0)
endif()

if(MODE STREQUAL "info")
  execute_process(
    COMMAND "${MINIGIS_EXE}" --info --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --info fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT _out MATCHES "\"features\"")
      message(FATAL_ERROR "La salida JSON no contiene el recuento de elementos:\n${_out}")
    endif()
    if(NOT _out MATCHES "EPSG:4326")
      message(FATAL_ERROR "La salida JSON no informa del SRC EPSG:4326:\n${_out}")
    endif()
    message(STATUS "minigis --info OK")
  endif()

elseif(MODE STREQUAL "snapshot")
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot "${OUT}" --layer "${SAMPLE}" --size 900x600
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --snapshot fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT EXISTS "${OUT}")
      message(FATAL_ERROR "No se genero el PNG: ${OUT}\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
    file(SIZE "${OUT}" _size)
    if(_size LESS 2048)
      message(FATAL_ERROR "El PNG generado parece vacio (${_size} bytes)")
    endif()
    message(STATUS "minigis --snapshot OK: ${OUT} (${_size} bytes)")
  endif()

elseif(MODE STREQUAL "bench")
  # Contrato del banco de pruebas: salida JSON estable y presupuesto respetado.
  # Es la base de la puerta de regresion de rendimiento en CI.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --bench "${FRAMES}" --bench-scenario "${SCENARIO}"
            --json --bench-budget "${BUDGET}" --layer "${SAMPLE}" --size 900x600
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --bench fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS "\"quality\"" "\"draft\"" "\"median_ms\"" "\"p90_ms\"" "\"store\"" "\"budget_ok\"")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida JSON de --bench no contiene ${_key}:\n${_out}")
      endif()
    endforeach()
    if(NOT _out MATCHES "\"budget_ok\": true")
      message(FATAL_ERROR "El presupuesto de --bench no se cumplio:\n${_out}")
    endif()
    message(STATUS "minigis --bench OK")
  endif()

else()
  message(FATAL_ERROR "MODE no soportado: ${MODE}")
endif()
