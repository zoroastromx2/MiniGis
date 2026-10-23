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

elseif(MODE STREQUAL "geopackage")
  # Un GeoPackage contiene varias capas: --info debe describirlas TODAS y decir a
  # que archivo y en que posicion pertenece cada una. Antes solo se informaba de
  # la primera, que es el mismo fallo que dejaba una sola capa en el panel.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --info --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --info del GeoPackage fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    # Cuatro capas vectoriales en el JSON.
    string(REGEX MATCHALL "\"type\": \"vector\"" _vector_layers "${_out}")
    list(LENGTH _vector_layers _vector_count)
    if(NOT _vector_count EQUAL 4)
      message(FATAL_ERROR "El GeoPackage tiene 4 capas y --info informo de ${_vector_count}:\n${_out}")
    endif()

    foreach(_key IN ITEMS "\"container\": \"ejemplo.gpkg\"" "\"layerCount\": 4"
                          "\"layerIndex\": 0" "\"layerIndex\": 1" "\"layerIndex\": 3"
                          "\"assumed\": false"
                          "\"name\": \"cities\"" "\"name\": \"parcels\""
                          "\"name\": \"parcels_utm30\"" "\"name\": \"area_sur\"")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida JSON no contiene ${_key}:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --info del GeoPackage OK")
  endif()

elseif(MODE STREQUAL "check-layers")
  # Contrato visible en la interfaz: al abrir un GeoPackage se cargan todas sus
  # capas y el panel muestra un item con el nombre del archivo y una capa por
  # subitem. La comprobacion se hace sin ventana (--check-layers).
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-layers --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-layers fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    if(_out MATCHES "FALLO")
      message(FATAL_ERROR "El mapa o el panel no muestran todas las capas del GeoPackage:\n${_out}")
    endif()
    foreach(_key IN ITEMS "se cargan todas las capas del archivo"
                          "cada capa usa el SRC del archivo"
                          "el panel agrupa las capas bajo el nombre del archivo"
                          "aparece como un item con 4 capas"
                          "cities, parcels, parcels_utm30, area_sur"
                          "el encuadre inicial incluye todas las capas del archivo"
                          "cada capa del archivo se dibuja en el mapa")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La comprobacion «${_key}» no se ejecuto:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --check-layers OK")
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

elseif(MODE STREQUAL "raster-info")
  # Contrato de --info para raster: tipo, tamano, bandas, SRC y NoData.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --info --json --layer "${SAMPLE}" --layer "${SAMPLE2}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --info raster fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS "\"type\": \"raster\"" "\"width\": 256" "\"index\": 3" "\"epsg\": 25830")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida JSON del raster no contiene ${_key}:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --info raster OK")
  endif()

elseif(MODE STREQUAL "check-raster")
  # Contrato del dibujado raster: todas las comprobaciones deben pasar.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-raster --layer "${SAMPLE}" --layer "${SAMPLE2}" --layer "${SAMPLE3}"
            --size 900x600
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-raster fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    if(_out MATCHES "FALLO")
      message(FATAL_ERROR "El dibujado raster no cumple su contrato:\n${_out}")
    endif()
    foreach(_key IN ITEMS "cuadrantes en su posicion" "extension del raster" "reproyeccion UTM"
                          "NoData transparente" "estiramiento por defecto" "estiramiento por percentiles"
                          "paleta del archivo" "el cambio de estilo se ve en el mapa" "paneo reutilizado"
                          "lectura acotada" "sin margenes negros al reproyectar")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La comprobacion «${_key}» no se ejecuto:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --check-raster OK")
  endif()

elseif(MODE STREQUAL "raster-dialog")
  # Captura del dialogo de estilo raster (se comprueba que se escribe y no esta vacio).
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot-raster-dialog "${OUT}" --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --snapshot-raster-dialog fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT EXISTS "${OUT}")
      message(FATAL_ERROR "No se genero la captura del dialogo: ${OUT}\nSTDOUT:\n${_out}")
    endif()
    file(SIZE "${OUT}" _size)
    if(_size LESS 4000)
      message(FATAL_ERROR "La captura del dialogo parece vacia (${_size} bytes)")
    endif()
    message(STATUS "minigis --snapshot-raster-dialog OK: ${_size} bytes")
  endif()

elseif(MODE STREQUAL "snapshot-window")
  # Captura de la interfaz con raster + vectorial: valida que ambos se cargan.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot-window "${OUT}" --layer "${SAMPLE}" --layer "${SAMPLE2}" --size 1000x700
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --snapshot-window fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT EXISTS "${OUT}")
      message(FATAL_ERROR "No se genero la captura de la interfaz: ${OUT}\nSTDOUT:\n${_out}")
    endif()
    file(SIZE "${OUT}" _size)
    if(_size LESS 5000)
      message(FATAL_ERROR "La captura de la interfaz parece vacia (${_size} bytes)")
    endif()
    message(STATUS "minigis --snapshot-window OK: ${_size} bytes")
  endif()

elseif(MODE STREQUAL "check-identify")
  # Contrato de Identificar: geometria, huecos, radio, capas sin indice, modo
  # envolventes, orden, alcance del arbol, panel, marcador y cache.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-identify --layer "${SAMPLE}" --layer "${SAMPLE2}" --layer "${SAMPLE3}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-identify fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    if(_out MATCHES "FALLO")
      message(FATAL_ERROR "Identificar no cumple su contrato:\n${_out}")
    endif()
    foreach(_key IN ITEMS "clic en un punto identifica la ciudad"
                          "el hueco del poligono no se identifica"
                          "el radio es el que se pide"
                          "una capa sin indice se identifica"
                          "una capa indexada por envolventes"
                          "el recorte de resultados se declara"
                          "sin seleccion se consultan todas las capas visibles"
                          "la capa seleccionada manda"
                          "la capa seleccionada va primero"
                          "una capa oculta no se identifica"
                          "una capa raster seleccionada no limita"
                          "el panel agrupa por capa y muestra los campos"
                          "el marcador del clic se dibuja"
                          "identificar no invalida el cuadro cacheado"
                          "arrastrar no identifica"
                          "identificar selecciona el primer resultado"
                          "el objeto seleccionado se resalta en el mapa"
                          "un clic sin resultados limpia la seleccion"
                          "la tabla se mueve al registro seleccionado"
                          "la tabla cambia a la capa del objeto"
                          "la tabla oculta no se mueve"
                          "la seleccion no invalida el cuadro cacheado"
                          "el panel marca el elemento seleccionado")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La comprobacion «${_key}» no se ejecuto:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --check-identify OK")
  endif()

elseif(MODE STREQUAL "identify-at")
  # Consulta puntual: punto, capa limitada, capa reproyectada y hueco.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --identify-at -3.70379,40.41678 --json
            --size 1000x700 --extent -3.78,40.35,-3.66,40.43
            --layer "${SAMPLE}" --layer "${SAMPLE2}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --identify-at fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  foreach(_key IN ITEMS "\"layer\": \"cities\"" "\"name\": \"Madrid\"" "\"inside\": true"
                        "\"layers_searched\": 2" "\"truncated\": false" "\"hit_count\": 2"
                        "\"selection\": {" "\"fid\": 0")
    if(NOT _out MATCHES "${_key}")
      message(FATAL_ERROR "La salida JSON de --identify-at no contiene ${_key}:\n${_out}")
    endif()
  endforeach()

  execute_process(
    COMMAND "${MINIGIS_EXE}" --identify-at -3.70379,40.41678 --identify-layer parcels --json
            --size 1000x700 --extent -3.78,40.35,-3.66,40.43
            --layer "${SAMPLE}" --layer "${SAMPLE2}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --identify-at --identify-layer fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  foreach(_key IN ITEMS "\"layer\": \"parcels\"" "\"parcel_id\": \"P-001\""
                        "\"layers_searched\": 1" "\"scope\": \"layer\"")
    if(NOT _out MATCHES "${_key}")
      message(FATAL_ERROR "La salida JSON de --identify-layer no contiene ${_key}:\n${_out}")
    endif()
  endforeach()

  execute_process(
    COMMAND "${MINIGIS_EXE}" --identify-at -3.71,40.41 --json
            --size 1000x700 --extent -3.78,40.35,-3.66,40.43 --layer "${SAMPLE3}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --identify-at (UTM) fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  foreach(_key IN ITEMS "\"parcel_id\": \"P-001\"" "\"inside\": true")
    if(NOT _out MATCHES "${_key}")
      message(FATAL_ERROR "La salida JSON de la capa UTM no contiene ${_key}:\n${_out}")
    endif()
  endforeach()

  execute_process(
    COMMAND "${MINIGIS_EXE}" --identify-at -3.74,40.38 --json
            --size 1000x700 --extent -3.78,40.35,-3.66,40.43 --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --identify-at (hueco) fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(NOT _out MATCHES "\"hit_count\": 0")
    message(FATAL_ERROR "El hueco del poligono devolvio resultados:\n${_out}")
  endif()
  if(NOT _out MATCHES "\"selection\": null")
    message(FATAL_ERROR "El hueco del poligono dejo una seleccion:\n${_out}")
  endif()
  message(STATUS "minigis --identify-at OK")

elseif(MODE STREQUAL "identify-window")
  # Captura de la interfaz con el panel de Identificar lleno, el resalte y la
  # tabla de atributos en la fila del objeto.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot-window "${OUT}" --identify-at -3.70379,40.41678
            --with-attributes --size 1000x700 --layer "${SAMPLE}" --layer "${SAMPLE2}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --snapshot-window con --identify-at fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT EXISTS "${OUT}")
      message(FATAL_ERROR "No se genero la captura: ${OUT}\nSTDOUT:\n${_out}")
    endif()
    file(SIZE "${OUT}" _size)
    if(_size LESS 10000)
      message(FATAL_ERROR "La captura con el panel parece vacia (${_size} bytes)")
    endif()
    message(STATUS "minigis --snapshot-window + --identify-at OK: ${_size} bytes")
  endif()

else()
  message(FATAL_ERROR "MODE no soportado: ${MODE}")
endif()
