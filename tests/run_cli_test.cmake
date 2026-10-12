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

elseif(MODE STREQUAL "index-shp")
  # El indice de envolventes debe coincidir con lo que devuelve GDAL y no debe
  # dejar la capa sin atributos. Lo segundo es un contrato facil de romper: al
  # indexar se ignoran temporalmente todos los campos (para no leer un DBF de
  # gigabytes) y basta un `return` que se salte la restauracion para que la tabla
  # de atributos muestre "(nulo)" en todas las filas.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --index-shp "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --index-shp fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    if(NOT _out MATCHES "0 discrepancias")
      message(FATAL_ERROR "El indice de envolventes no coincide con GDAL:\n${_out}")
    endif()
    if(NOT _out MATCHES "Atributos tras indexar: 0 de")
      message(FATAL_ERROR "La capa se quedo sin atributos tras indexarla:\n${_out}")
    endif()
    message(STATUS "minigis --index-shp OK")
  endif()

elseif(MODE STREQUAL "check-refresh")
  # El mapa debe redibujarse al cambiar la simbologia o la visibilidad. El lienzo
  # cachea el ultimo cuadro para reutilizarlo al desplazar, y una cache mal
  # invalidada hacia que esos cambios no se vieran hasta mover la vista.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-refresh -l "${SAMPLE}" --size 600x400
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-refresh fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    if(_out MATCHES "FALLO")
      message(FATAL_ERROR "El mapa no se redibuja al cambiar simbologia o visibilidad:\n${_out}")
    endif()
    if(NOT _out MATCHES "la capa oculta desaparece del mapa")
      message(FATAL_ERROR "La comprobacion de visibilidad no se ejecuto:\n${_out}")
    endif()
    message(STATUS "minigis --check-refresh OK")
  endif()

elseif(MODE STREQUAL "check-labels")
  # Etiquetado: campos, escalas, solapes, repeticiones, cache de textos y el
  # invariante del paneo con etiquetas dibujadas encima del cuadro reutilizado.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-labels -l "${SAMPLE}" --size 600x400
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-labels fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    if(_out MATCHES "FALLO")
      message(FATAL_ERROR "El etiquetado no cumple su contrato:\n${_out}")
    endif()
    if(NOT _out MATCHES "se etiqueta un elemento por campo")
      message(FATAL_ERROR "La comprobacion de campos no se ejecuto:\n${_out}")
    endif()
    if(NOT _out MATCHES "paneo con etiquetas")
      message(FATAL_ERROR "La comprobacion de paneo con etiquetas no se ejecuto:\n${_out}")
    endif()
    message(STATUS "minigis --check-labels OK")
  endif()

else()
  message(FATAL_ERROR "MODE no soportado: ${MODE}")
endif()
