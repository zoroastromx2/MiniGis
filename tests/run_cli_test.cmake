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

elseif(MODE STREQUAL "dlls")
  # --dlls es la misma lista que ensena "Acerca de": nombre de cada DLL cargada
  # desde la carpeta del ejecutable y su version. Comprueba que estan las que
  # aporta la feature parquet (Arrow/Parquet no llevan version en el PE: la
  # cifra tiene que venir del mapa de puertos de vcpkg generado al configurar).
  execute_process(
    COMMAND "${MINIGIS_EXE}" --dlls
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --dlls fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS "gdal.dll +3[.]" "Qt6Core.dll +6[.]"
                          "arrow.dll +25[.]" "parquet.dll +25[.]")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La lista de DLLs no contiene «${_key}»:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --dlls OK")
  endif()

elseif(MODE STREQUAL "about")
  # Captura del dialogo "Acerca de" (no debe quedar vacio). El contenido de la
  # tabla es el mismo de --dlls, que ya comprueba los nombres y las versiones.
  # La plataforma offscreen no tiene fuentes y el texto sale como cajas, asi que
  # el umbral de tamano es bajo: solo descarta un dialogo sin tabla.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot-about "${OUT}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --snapshot-about fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT EXISTS "${OUT}")
      message(FATAL_ERROR "No se genero la captura del dialogo: ${OUT}")
    endif()
    file(SIZE "${OUT}" _size)
    if(_size LESS 4000)
      message(FATAL_ERROR "La captura del dialogo parece vacia (${_size} bytes)")
    endif()
    message(STATUS "minigis --snapshot-about OK: ${_size} bytes")
  endif()

elseif(MODE STREQUAL "rendimiento-geoparquet")
  # Puerta opt-in con una capa GeoParquet grande real (no versionada). Se omite
  # si no esta MINIGIS_GEOPARQUET_BENCH_DATASET; el presupuesto se puede ajustar
  # con MINIGIS_GEOPARQUET_BUDGET_MS (200 ms por defecto, mediana del cuadro).
  if("$ENV{MINIGIS_GEOPARQUET_BENCH_DATASET}" STREQUAL "")
    message("OMITIDA: define MINIGIS_GEOPARQUET_BENCH_DATASET para medir la capa grande")
  else()
    set(_dataset "$ENV{MINIGIS_GEOPARQUET_BENCH_DATASET}")
    set(_budget "200")
    if(NOT "$ENV{MINIGIS_GEOPARQUET_BUDGET_MS}" STREQUAL "")
      set(_budget "$ENV{MINIGIS_GEOPARQUET_BUDGET_MS}")
    endif()

    execute_process(
      COMMAND "${MINIGIS_EXE}" --bench 24 --bench-scenario mixed --size 1200x800 --json
              --layer "${_dataset}"
      RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "minigis --bench de la capa grande fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()

    if(NOT _out MATCHES "\"mode\": \"memory\"")
      message(FATAL_ERROR "La capa grande no se dibujo desde el almacen:\n${_out}")
    endif()

    if(NOT _out MATCHES "\"median_ms\": ([0-9.]+)")
      message(FATAL_ERROR "La salida de --bench no trae la mediana:\n${_out}")
    endif()
    set(_median "${CMAKE_MATCH_1}")
    if(_median GREATER _budget)
      message(FATAL_ERROR "Mediana de ${_median} ms por cuadro: supera el presupuesto de ${_budget} ms")
    endif()

    if(NOT _out MATCHES "\"features\": ([0-9]+)")
      message(FATAL_ERROR "La salida de --bench no trae los elementos del almacen:\n${_out}")
    endif()
    if(CMAKE_MATCH_1 EQUAL 0)
      message(FATAL_ERROR "El almacen se construyo vacio:\n${_out}")
    endif()

    message(STATUS "minigis rendimiento-geoparquet OK: mediana ${_median} ms (presupuesto ${_budget} ms)")
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

elseif(MODE STREQUAL "check-mosaic")
  # Contrato del mosaico con las 6 hojas del fixture (incluida la replica).
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-mosaic
            --layer "${SAMPLE}" --layer "${SAMPLE2}" --layer "${SAMPLE3}"
            --layer "${SAMPLE4}" --layer "${SAMPLE5}" --layer "${SAMPLE6}"
            --size 900x600 --mosaic-cache "${MOSAIC_CACHE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-mosaic fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    if(_out MATCHES "FALLO")
      message(FATAL_ERROR "El mosaico no cumple su contrato:\n${_out}")
    endif()
    foreach(_key IN ITEMS "todas las hojas estan en el mosaico" "la extension union coincide con GDAL"
                          "cada hoja se dibuja en su sitio" "sin costuras entre hojas contiguas"
                          "la replica del mosaico esta disponible" "la replica reproduce las hojas"
                          "la replica y las hojas dan el mismo mapa" "la segunda vez se reutiliza la cache"
                          "se descarta lo que no esta en la vista" "lectura acotada al lienzo"
                          "cambiar el estilo redibuja el mosaico" "el paneo reutilizado coincide"
                          "el cuadro cabe en el presupuesto")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La comprobacion «${_key}» no se ejecuto:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --check-mosaic OK")
  endif()
elseif(MODE STREQUAL "mosaic-info")
  # El mosaico se describe como UN objeto (type=mosaic) cuando se declara con lista.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --info --json --layer-list "${MOSAIC_LIST}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --info del mosaico fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(NOT _out MATCHES "\"type\": \"mosaic\"")
    message(FATAL_ERROR "El mosaico no se describe como tal:\n${_out}")
  endif()
  if(NOT _out MATCHES "\"tiles\": 6")
    message(FATAL_ERROR "El mosaico no informa de sus 6 hojas:\n${_out}")
  endif()
  message(STATUS "minigis --info del mosaico OK")
elseif(MODE STREQUAL "mosaic-window")
  # Captura de la interfaz con el mosaico cargado y su fila en el panel.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot-window "${OUT}"
            --layer "${SAMPLE}" --layer "${SAMPLE2}" --layer "${SAMPLE3}"
            --layer "${SAMPLE4}" --layer "${SAMPLE5}" --layer "${SAMPLE6}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --snapshot-window del mosaico fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "No se genero la captura: ${OUT}")
  endif()
  file(SIZE "${OUT}" _size)
  if(_size LESS 8000)
    message(FATAL_ERROR "La captura del mosaico parece vacia (${_size} bytes)")
  endif()
  message(STATUS "minigis --snapshot-window del mosaico OK: ${_size} bytes")
elseif(MODE STREQUAL "mosaic-progress")
  # Captura de la ventana de progreso durante una carga real de hojas.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot-progress "${OUT}"
            --layer "${SAMPLE}" --layer "${SAMPLE2}" --layer "${SAMPLE3}"
            --layer "${SAMPLE4}" --layer "${SAMPLE5}" --layer "${SAMPLE6}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --snapshot-progress fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "No se genero la captura del progreso: ${OUT}")
  endif()
  if(NOT _out MATCHES "progreso 100")
    message(FATAL_ERROR "La ventana de progreso no llego al 100 %:\n${_out}")
  endif()
  message(STATUS "minigis --snapshot-progress OK")
elseif(MODE STREQUAL "rendimiento-mosaico")
  # Puerta opt-in con el conjunto real: se omite si no hay MINIGIS_MOSAIC_DATASET.
  set(_dataset "$ENV{MINIGIS_MOSAIC_DATASET}")
  if(NOT _dataset)
    message(STATUS "PUERTA-OMITIDA: define MINIGIS_MOSAIC_DATASET con la carpeta o la lista de hojas")
    return()
  endif()

  set(_list "${MOSAIC_LIST}")
  if(IS_DIRECTORY "${_dataset}")
    file(GLOB _tiles "${_dataset}/*.tif" "${_dataset}/*.TIF" "${_dataset}/*.jpg" "${_dataset}/*.JPG"
                    "${_dataset}/*.jpeg" "${_dataset}/*.jp2")
    list(LENGTH _tiles _count)
    if(_count EQUAL 0)
      message(STATUS "PUERTA-OMITIDA: no hay imagenes en ${_dataset}")
      return()
    endif()
    file(WRITE "${_list}" "")
    foreach(_tile IN LISTS _tiles)
      file(APPEND "${_list}" "${_tile}\n")
    endforeach()
  else()
    set(_list "${_dataset}")
  endif()

  set(_start_time "$ENV{MINIGIS_MOSAIC_TIMEOUT}")
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-mosaic --layer-list "${_list}" --size 1200x800
            --mosaic-cache "${MOSAIC_CACHE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "La puerta de rendimiento del mosaico fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(NOT _out MATCHES "el cuadro cabe en el presupuesto")
    message(FATAL_ERROR "No se midio el presupuesto de cuadro:\n${_out}")
  endif()
  message(STATUS "minigis --check-mosaic con el conjunto real OK")
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

elseif(MODE STREQUAL "project-info")
  # Contrato de lectura de un proyecto QGIS: capas, orden, visibilidad, SRC,
  # extension, estilos, etiquetas y capas no disponibles.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --project-info --json "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --project-info fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS
            "\"crsEpsg\": 25830"
            "\"canvasEpsg\": 25830"
            "\"layerCount\": 5"
            "\"available\": 4"
            "\"name\": \"cities\""
            "\"name\": \"parcels_categorizado\""
            "\"name\": \"orto\""
            "\"type\": \"raster\""
            "\"visible\": false"
            "\"styleRepresentable\": false"
            "\"labelsRepresentable\": true"
            "\"minimumScale\": 66.14"
            "\"maximumScale\": 1.3229"
            "\"haloWidth\": 5.6693"
            "\"opacity\": 80"
            "\"no_existe\""
            "\"no existe el archivo")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida de --project-info no contiene ${_key}:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --project-info OK")
  endif()

elseif(MODE STREQUAL "project-window")
  # Captura de la interfaz con un proyecto abierto: panel, titulo y mapa deben
  # reflejar el proyecto, no una lista de capas suelta.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot-window "${OUT}" "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --snapshot-window del proyecto fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT EXISTS "${OUT}")
      message(FATAL_ERROR "No se genero la captura del proyecto: ${OUT}")
    endif()
    file(SIZE "${OUT}" _size)
    if(_size LESS 10000)
      message(FATAL_ERROR "La captura del proyecto parece vacia (${_size} bytes)")
    endif()
    message(STATUS "minigis --snapshot-window del proyecto OK: ${_size} bytes")
  endif()

elseif(MODE STREQUAL "save-project")
  # Guardar una copia (en la misma carpeta o en otra) y reabrirla: las rutas
  # relativas se reescriben y el proyecto sigue teniendo sus 4 capas abribles.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  file(REMOVE "${OUT}" "${OUT}.bak")

  execute_process(
    COMMAND "${MINIGIS_EXE}" --save-project "${OUT}" "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "minigis --save-project fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "No se creo el proyecto guardado: ${OUT}")
  endif()

  execute_process(
    COMMAND "${MINIGIS_EXE}" --project-info --json "${OUT}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "El proyecto guardado no se puede leer (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  foreach(_key IN ITEMS "\"available\": 4" "\"layerCount\": 5" "\"crsEpsg\": 25830")
    if(NOT _out MATCHES "${_key}")
      message(FATAL_ERROR "El proyecto guardado no conserva ${_key}:\n${_out}")
    endif()
  endforeach()
  if(OUT MATCHES "\\.qgs$")
    if(NOT _out MATCHES "\"container\": \"qgs\"")
      message(FATAL_ERROR "El proyecto guardado en .qgs no se reconoce como XML plano:\n${_out}")
    endif()
  else()
    if(NOT _out MATCHES "\"container\": \"qgz\"")
      message(FATAL_ERROR "El proyecto guardado en .qgz no se reconoce como comprimido:\n${_out}")
    endif()
  endif()

  # Un segundo guardado sobre el mismo archivo debe dejar la version anterior.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --save-project "${OUT}" "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0 OR NOT EXISTS "${OUT}.bak")
    message(FATAL_ERROR "El segundo guardado no creo la copia .bak (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  message(STATUS "minigis --save-project OK: ${OUT}")

elseif(MODE STREQUAL "check-project")
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-project "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-project fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(_out MATCHES "FALLO")
      message(FATAL_ERROR "El contrato del proyecto tiene fallos:\n${_out}")
    endif()
    foreach(_key IN ITEMS "se abre el proyecto en la ventana"
                          "capas abribles del proyecto"
                          "orden y nombres del proyecto"
                          "SRC del lienzo"
                          "encuadre del proyecto"
                          "visibilidad del proyecto"
                          "simbologia de parcels"
                          "etiquetas de cities"
                          "estilo del raster"
                          "el proyecto queda modificado"
                          "el titulo marca los cambios"
                          "se guardan visibilidad y estilo"
                          "se guardan las etiquetas"
                          "se conserva el nodo desconocido"
                          "la capa anadida se guarda"
                          "la capa cerrada se elimina"
                          "el proyecto final conserva las capas no disponibles")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La comprobacion «${_key}» no se ejecuto:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --check-project OK")
  endif()

elseif(MODE STREQUAL "geoparquet")
  # Contrato de --check-geoparquet: el archivo debe leerse completo y el almacen
  # debe conservar todos los elementos con geometria. Las expectativas son
  # opcionales para que el mismo modo sirva a varios fixtures.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-geoparquet --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-geoparquet fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS "\"ok\": true" "\"driver\": \"Parquet\"")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida de --check-geoparquet no contiene ${_key}:\n${_out}")
      endif()
    endforeach()

    if(DEFINED EXPECT_FEATURES AND NOT _out MATCHES "\"features\": ${EXPECT_FEATURES}")
      message(FATAL_ERROR "Se esperaban ${EXPECT_FEATURES} elementos:\n${_out}")
    endif()
    if(DEFINED EXPECT_STORE AND NOT _out MATCHES "\"storeFeatures\": ${EXPECT_STORE}")
      message(FATAL_ERROR "Se esperaban ${EXPECT_STORE} elementos en el almacen:\n${_out}")
    endif()
    if(DEFINED EXPECT_WITHOUT AND NOT _out MATCHES "\"withoutGeometry\": ${EXPECT_WITHOUT}")
      message(FATAL_ERROR "Se esperaban ${EXPECT_WITHOUT} elementos sin geometria:\n${_out}")
    endif()
    if(DEFINED EXPECT_EPSG AND NOT _out MATCHES "\"crsEpsg\": ${EXPECT_EPSG}")
      message(FATAL_ERROR "Se esperaba EPSG:${EXPECT_EPSG}:\n${_out}")
    endif()
    if(DEFINED EXPECT_POINTS AND NOT _out MATCHES "\"points\": ${EXPECT_POINTS}")
      message(FATAL_ERROR "Se esperaban ${EXPECT_POINTS} puntos:\n${_out}")
    endif()
    if(DEFINED EXPECT_LINES AND NOT _out MATCHES "\"lines\": ${EXPECT_LINES}")
      message(FATAL_ERROR "Se esperaban ${EXPECT_LINES} lineas:\n${_out}")
    endif()
    if(DEFINED EXPECT_POLYGONS AND NOT _out MATCHES "\"polygons\": ${EXPECT_POLYGONS}")
      message(FATAL_ERROR "Se esperaban ${EXPECT_POLYGONS} poligonos:\n${_out}")
    endif()
    if(DEFINED EXPECT_FID AND NOT _out MATCHES "\"fidColumn\": \"${EXPECT_FID}\"")
      message(FATAL_ERROR "Se esperaba la columna FID ${EXPECT_FID}:\n${_out}")
    endif()
    if(DEFINED EXPECT_CRS_UNKNOWN AND NOT _out MATCHES "\"crsName\": \"\"")
      message(FATAL_ERROR "Se esperaba un SRC desconocido:\n${_out}")
    endif()
    message(STATUS "minigis --check-geoparquet OK")
  endif()

elseif(MODE STREQUAL "geoparquet-info")
  execute_process(
    COMMAND "${MINIGIS_EXE}" --info --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --info de GeoParquet fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS "\"driver\": \"Parquet\"")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida de --info no contiene ${_key}:\n${_out}")
      endif()
    endforeach()
    if(DEFINED EXPECT_FEATURES AND NOT _out MATCHES "\"features\": ${EXPECT_FEATURES}")
      message(FATAL_ERROR "Se esperaban ${EXPECT_FEATURES} elementos:\n${_out}")
    endif()
    if(DEFINED EXPECT_EPSG AND NOT _out MATCHES "\"epsg\": ${EXPECT_EPSG}")
      message(FATAL_ERROR "Se esperaba EPSG:${EXPECT_EPSG}:\n${_out}")
    endif()
    message(STATUS "minigis --info (GeoParquet) OK")
  endif()

elseif(MODE STREQUAL "geoparquet-error")
  # Rechazos: una tabla Parquet sin geometria (5) o una carpeta particionada (3).
  execute_process(
    COMMAND "${MINIGIS_EXE}" --info --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --info fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(DEFINED EXPECT_TEXT)
    set(_all "${_out}\n${_err}")
    if(NOT _all MATCHES "${EXPECT_TEXT}")
      message(FATAL_ERROR "El mensaje no contiene «${EXPECT_TEXT}»:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
  endif()
  message(STATUS "minigis rechazo de GeoParquet OK")

elseif(MODE STREQUAL "kml")
  # El archivo debe leerse completo: ni un elemento con geometria puede quedarse
  # por el camino. La comprobacion compara lo que entrega OGR con lo que conserva
  # el almacen de geometrias, que es donde se perdia la MultiGeometry mixta.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-kml --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-kml fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    if(NOT _out MATCHES "\"ok\": true")
      message(FATAL_ERROR "El KML no se lee completo:\n${_out}")
    endif()
    if(NOT _out MATCHES "\"driver\": \"LIBKML\"")
      message(FATAL_ERROR "El KML no se abrio con el driver LIBKML:\n${_out}")
    endif()
    foreach(_key IN ITEMS "\"features\"" "\"geometry\"" "\"kinds\""
                          "\"withoutGeometry\"" "\"unsupportedParts\"" "\"storeFeatures\""
                          "\"attributeCache\": true"
                          "\"crsEpsg\": 4326")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida de --check-kml no contiene ${_key}:\n${_out}")
      endif()
    endforeach()
    message(STATUS "minigis --check-kml OK")
  endif()

elseif(MODE STREQUAL "kml-estilos")
  # Contrato de la simbologia importada: el archivo declara dos estilos y cada
  # elemento se dibuja con el suyo. Un solo estilo por capa se informa como
  # "single"; por encima del tope, "dominant".
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-kml --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-kml fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS "\"styleMode\": \"${EXPECT_STYLE_MODE}\""
                          "\"appliedStyles\": ${EXPECT_APPLIED}")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida de --check-kml no contiene ${_key}:\n${_out}")
      endif()
    endforeach()

    # Color y grosor solo cuando la prueba los fija: con mas estilos que el tope
    # manda el dominante y no tiene sentido afirmar cual es.
    if(NOT "${EXPECT_COLOR}" STREQUAL "")
      foreach(_key IN ITEMS "\"strokeColor\": \"${EXPECT_COLOR}\"" "\"strokeWidth\": ${EXPECT_WIDTH}")
        if(NOT _out MATCHES "${_key}")
          message(FATAL_ERROR "La salida de --check-kml no contiene ${_key}:\n${_out}")
        endif()
      endforeach()
    endif()
    message(STATUS "minigis --check-kml (estilos) OK")
  endif()

elseif(MODE STREQUAL "window-attributes")
  # Cargar una capa con la tabla de atributos a la vista no debe bloquear la
  # ventana. El fallo original: adjustar el ancho de columnas pedia data() de
  # todas las celdas y cada celda era una lectura de atributos en el origen.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --snapshot-window "${OUT}" --layer "${SAMPLE}" --with-attributes
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err
    TIMEOUT 120)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --snapshot-window --with-attributes fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT EXISTS "${OUT}")
      message(FATAL_ERROR "No se genero la captura: ${OUT}\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
    file(SIZE "${OUT}" _size)
    if(_size LESS 5000)
      message(FATAL_ERROR "La captura parece vacia (${_size} bytes)")
    endif()
    message(STATUS "minigis ventana con tabla de atributos OK: ${_size} bytes")
  endif()

elseif(MODE STREQUAL "pg-dialog")
  # Captura del dialogo PostGIS: no abre red, solo carga el catalogo.
  get_filename_component(_outdir "${OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_outdir}")
  if(EXISTS "${OUT}")
    file(REMOVE "${OUT}")
  endif()
  execute_process(
    COMMAND "${MINIGIS_EXE}" --connections "${CONNECTIONS}" --snapshot-pg-dialog "${OUT}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --snapshot-pg-dialog fallo (codigo ${_rc})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  if(EXPECT_EXIT EQUAL 0)
    if(NOT EXISTS "${OUT}")
      message(FATAL_ERROR "No se genero la captura del dialogo: ${OUT}")
    endif()
    file(SIZE "${OUT}" _size)
    if(_size LESS 5000)
      message(FATAL_ERROR "La captura del dialogo parece vacia (${_size} bytes)")
    endif()
    message(STATUS "minigis --snapshot-pg-dialog OK: ${_size} bytes")
  endif()

elseif(MODE STREQUAL "pg-info")
  # --info de una fuente pg: tiene que informar de conexion, esquema y tabla, y
  # no filtrar la contrasena. Los casos de error (catalogo ausente, conexion
  # desconocida) se comprueban solo por el codigo de salida y no tocan la red.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --connections "${CONNECTIONS}" --info --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --info de PostGIS fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS "\"connection\"" "\"schema\"" "\"table\"" "\"driver\": \"PostgreSQL\"")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida de --info no contiene ${_key}:\n${_out}")
      endif()
    endforeach()
    if(_out MATCHES "password")
      message(FATAL_ERROR "La salida de --info filtra la contrasena:\n${_out}")
    endif()
    message(STATUS "minigis --info de PostGIS OK")
  endif()

elseif(MODE STREQUAL "pg-check")
  # Contrato de --check-pg: inventario del esquema en JSON, sin contrasena.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --connections "${CONNECTIONS}" --check-pg --json --layer "${SAMPLE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-pg fallo (codigo ${_rc}, esperado ${EXPECT_EXIT})\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()

  if(EXPECT_EXIT EQUAL 0)
    foreach(_key IN ITEMS "\"ok\": true" "\"connection\"" "\"schema\"" "\"layers\""
                          "\"openOk\"" "\"elapsedMs\"")
      if(NOT _out MATCHES "${_key}")
        message(FATAL_ERROR "La salida de --check-pg no contiene ${_key}:\n${_out}")
      endif()
    endforeach()
    if(_out MATCHES "password")
      message(FATAL_ERROR "La salida de --check-pg filtra la contrasena:\n${_out}")
    endif()
    message(STATUS "minigis --check-pg OK")
  endif()

elseif(MODE STREQUAL "pg-uso")
  # --check-pg sin -l es un error de uso (1), no un fallo de conexion.
  execute_process(
    COMMAND "${MINIGIS_EXE}" --check-pg
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "minigis --check-pg sin capa debio salir con ${EXPECT_EXIT} y salio con ${_rc}\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
  endif()
  message(STATUS "minigis --check-pg sin capa OK (codigo ${_rc})")

else()
  message(FATAL_ERROR "MODE no soportado: ${MODE}")
endif()
