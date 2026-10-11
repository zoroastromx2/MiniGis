#############################################################
# MiniGisRuntime.cmake
#
# Deja junto al ejecutable todo lo necesario para poder lanzarlo
# desde Qt Creator o el Explorador sin preparar PATH a mano:
#   * DLLs de Qt y del cierre de enlazado del target (gdal.dll incluido)
#   * DLLs de las dependencias compartidas de GDAL instaladas por vcpkg
#   * plugins de plataforma y estilos de Qt (siempre)
#   * resto de plugins de Qt con windeployqt (opcional)
#
# Nota sobre copiar ficheros: se usa la forma "origenes... destino" y NO la
# opcion -t de copy_if_different, porque el CMake 3.30 que distribuye Qt
# Creator no la soporta: interpretaba "-t" como un archivo mas y tomaba la
# ultima DLL como carpeta de destino ("Target ... gdal.dll is not a directory").
#############################################################

option(MINIGIS_COPY_RUNTIME_DLLS
       "Copiar DLLs y plugins de Qt junto al ejecutable (Windows)" ON)

option(MINIGIS_DEPLOY_QT_RUNTIME
       "Ejecutar ademas windeployqt para desplegar el resto de plugins de Qt" ON)

# Copia las DLLs del arbol de vcpkg correspondiente a la configuracion actual.
# El script excluye a proposito las DLLs de Qt (nunca se deben mezclar dos
# instalaciones de Qt en la misma carpeta).
function(minigis_copy_vcpkg_dlls target)
  if(NOT WIN32 OR NOT MINIGIS_COPY_RUNTIME_DLLS)
    return()
  endif()

  if(NOT EXISTS "${MINIGIS_COPY_DLLS_SCRIPT}")
    return()
  endif()

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DMINIGIS_DLL_SRC_RELEASE=${MINIGIS_VCPKG_BIN_DIR}"
            "-DMINIGIS_DLL_SRC_DEBUG=${MINIGIS_VCPKG_DEBUG_BIN_DIR}"
            "-DMINIGIS_DLL_DST=$<TARGET_FILE_DIR:${target}>"
            "-DMINIGIS_DLL_CONFIG=$<CONFIG>"
            -P "${MINIGIS_COPY_DLLS_SCRIPT}"
    VERBATIM
    COMMENT "MiniGIS: copiando dependencias de GDAL (vcpkg) junto al ejecutable")
endfunction()

# Copia las DLLs de Qt y de cualquier dependencia compartida del cierre de
# enlazado del target (gdal.dll incluido).
function(minigis_copy_linked_dlls target)
  if(NOT WIN32 OR NOT MINIGIS_COPY_RUNTIME_DLLS)
    return()
  endif()

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:${target}>"
            "$<TARGET_FILE_DIR:${target}>"
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "MiniGIS: copiando DLLs de Qt junto al ejecutable")
endfunction()

# Localiza la raiz de plugins de la instalacion de Qt que se esta usando:
#   * Qt oficial: <qt>/plugins
#   * Qt de vcpkg: <prefijo>/Qt6/plugins (release) o <prefijo>/debug/Qt6/plugins
function(minigis_locate_qt_plugins out_var)
  set(_candidates "")
  foreach(_config IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG IMPORTED_LOCATION)
    get_target_property(_core_dll Qt6::Core ${_config})
    if(_core_dll AND NOT _core_dll MATCHES "-NOTFOUND$")
      get_filename_component(_core_dir "${_core_dll}" DIRECTORY)
      list(APPEND _candidates
           "${_core_dir}/../plugins"
           "${_core_dir}/../Qt6/plugins"
           "${_core_dir}/Qt6/plugins")
    endif()
  endforeach()

  foreach(_candidate IN LISTS _candidates)
    if(EXISTS "${_candidate}/platforms")
      get_filename_component(_candidate "${_candidate}" ABSOLUTE)
      set(${out_var} "${_candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set(${out_var} "" PARENT_SCOPE)
endfunction()

# Nombre de los ficheros de plugin que corresponden a la configuracion actual.
# Se usan etiquetas ("debug"/"release") en lugar de sufijos vacios porque una
# lista con el elemento vacio es, en CMake, una lista vacia y no copiaria nada.
# Con generadores multi-configuracion se copian las dos variantes (no colisionan
# porque los de depuracion acaban en "d").
function(minigis_plugin_variants out_var)
  if(CMAKE_CONFIGURATION_TYPES)
    set(${out_var} "release" "debug" PARENT_SCOPE)
  elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(${out_var} "debug" PARENT_SCOPE)
  else()
    set(${out_var} "release" PARENT_SCOPE)
  endif()
endfunction()

# Copia los plugins imprescindibles para que la aplicacion arranque: el de
# plataforma (windows para la ventana normal y offscreen para los modos
# --info/--snapshot) y el de estilos. No depende de windeployqt.
function(minigis_copy_qt_plugins target)
  if(NOT WIN32 OR NOT MINIGIS_COPY_RUNTIME_DLLS)
    return()
  endif()

  if(NOT TARGET Qt6::Core)
    return()
  endif()

  minigis_locate_qt_plugins(_minigis_qt_plugins_root)
  if(_minigis_qt_plugins_root STREQUAL "")
    message(WARNING
      "MiniGIS: no se localizaron los plugins de Qt.\n"
      "  La aplicacion no arrancara desde la carpeta de compilacion: define\n"
      "  QT_QPA_PLATFORM_PLUGIN_PATH o ejecuta windeployqt a mano.")
    return()
  endif()

  minigis_plugin_variants(_minigis_variants)

  set(_minigis_platform_plugins "")
  set(_minigis_style_plugins "")
  foreach(_variant IN LISTS _minigis_variants)
    if(_variant STREQUAL "debug")
      set(_suffix "d")
    else()
      set(_suffix "")
    endif()

    foreach(_platform IN ITEMS qwindows qoffscreen)
      if(EXISTS "${_minigis_qt_plugins_root}/platforms/${_platform}${_suffix}.dll")
        list(APPEND _minigis_platform_plugins "${_minigis_qt_plugins_root}/platforms/${_platform}${_suffix}.dll")
      endif()
    endforeach()

    foreach(_style IN ITEMS qmodernwindowsstyle qwindowsvistastyle)
      if(EXISTS "${_minigis_qt_plugins_root}/styles/${_style}${_suffix}.dll")
        list(APPEND _minigis_style_plugins "${_minigis_qt_plugins_root}/styles/${_style}${_suffix}.dll")
      endif()
    endforeach()
  endforeach()

  if(_minigis_platform_plugins)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/platforms"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              ${_minigis_platform_plugins}
              "$<TARGET_FILE_DIR:${target}>/platforms"
      VERBATIM
      COMMENT "MiniGIS: copiando plugins de plataforma de Qt (windows/offscreen)")
  endif()

  if(_minigis_style_plugins)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/styles"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              ${_minigis_style_plugins}
              "$<TARGET_FILE_DIR:${target}>/styles"
      VERBATIM
      COMMENT "MiniGIS: copiando estilos de Qt")
  endif()
endfunction()

# Despliega el resto de plugins de Qt (formatos de imagen, TLS, iconengines...)
# con windeployqt, si esta disponible en la instalacion de Qt en uso.
function(minigis_deploy_qt_plugins target)
  if(NOT WIN32 OR NOT MINIGIS_DEPLOY_QT_RUNTIME)
    return()
  endif()

  if(NOT TARGET Qt6::qmake)
    message(STATUS "MiniGIS: sin Qt6::qmake no se puede ejecutar windeployqt; se usan solo los plugins copiados")
    return()
  endif()

  get_target_property(_minigis_qmake Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(_minigis_qt_bin_dir "${_minigis_qmake}" DIRECTORY)
  set(_minigis_windeployqt "${_minigis_qt_bin_dir}/windeployqt.exe")

  if(NOT EXISTS "${_minigis_windeployqt}")
    message(STATUS
      "MiniGIS: windeployqt no esta en ${_minigis_qt_bin_dir}; se usan solo los plugins copiados\n"
      "  (si Qt viene del arbol de vcpkg es lo esperado: faltan imageformats/tls, no los de plataforma).")
    return()
  endif()

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${_minigis_windeployqt}"
            --no-translations
            --no-compiler-runtime
            --no-system-d3d-compiler
            "$<IF:$<CONFIG:Debug>,--debug,--release>"
            "$<TARGET_FILE:${target}>"
    VERBATIM
    COMMENT "MiniGIS: desplegando el resto de plugins de Qt con windeployqt")
endfunction()

function(minigis_deploy_runtime target)
  minigis_copy_linked_dlls(${target})
  minigis_copy_vcpkg_dlls(${target})
  minigis_copy_qt_plugins(${target})
  minigis_deploy_qt_plugins(${target})

  # Qt avisa por consola si no existe <ejecutable>/lib/fonts. Se crea el
  # directorio para dejar limpia la salida de los modos sin ventana. Si algun
  # dia quieres texto en la plataforma "offscreen", copia ahi una fuente libre
  # (por ejemplo DejaVuSans.ttf).
  if(WIN32 AND MINIGIS_COPY_RUNTIME_DLLS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/lib/fonts"
      VERBATIM
      COMMENT "MiniGIS: preparando el directorio de fuentes de Qt")
  endif()
endfunction()
