#############################################################
# MiniGIS - visor SIG minimo basado en Qt 6 y GDAL
#
# Convenciones tomadas de QGIS:
#   * CMake con rango de politicas y modulos propios en cmake/
#   * vcpkg en modo manifiesto (vcpkg/vcpkg.json) como en cmake/VcpkgToolchain.cmake
#   * Capas separadas: src/core (datos), src/gui (widgets), src/app (ejecutable)
#   * find_package(GDAL) para el acceso a formatos vectoriales y raster
#############################################################
cmake_minimum_required(VERSION 3.22...4.3)

set(CMAKE_COLOR_MAKEFILE ON)
set(MINIGIS_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${MINIGIS_CMAKE_DIR}")

# vcpkg y utilidades de runtime se resuelven antes de project() para que el
# toolchain de vcpkg este configurado cuando se busquen Qt y GDAL.
include(MiniGisVcpkg)
include(MiniGisRuntime)
set(MINIGIS_COPY_DLLS_SCRIPT "${MINIGIS_CMAKE_DIR}/MiniGisCopyDlls.cmake")

project(MiniGIS
        VERSION 0.1.0
        DESCRIPTION "Visor SIG minimo (Qt 6 + GDAL) para cargar shapefiles"
        LANGUAGES CXX)

# --- Opciones -------------------------------------------------------------
option(MINIGIS_BUILD_TESTS "Construir pruebas de humo (CTest)" ON)

# --- Estandar de C++ ------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Salidas predecibles en el arbol de compilacion -----------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Qt 6 -----------------------------------------------------------------
# El toolchain de vcpkg antepone su arbol de instalacion a CMAKE_PREFIX_PATH y a
# CMAKE_FIND_ROOT_PATH. Si en ese arbol hay otro Qt (por ejemplo el qtbase que
# instalan otros proyectos, como hace QGIS), Qt6Core y Qt6CoreTools se
# resolverian contra el y la mezcla con el Qt del kit rompe la configuracion
# (sin plugins de plataforma, ASSERT en qstylehints, sin windeployqt...).
#
# Antes de buscar Qt se hacen tres cosas:
#   1) elegir la instalacion de Qt (Qt6_DIR del kit/preset, nunca la de vcpkg)
#   2) descartar rutas de Qt ya cacheadas que apunten al arbol de vcpkg
#   3) excluir temporalmente el arbol de vcpkg de la busqueda de Qt
function(minigis_pin_qt_tools qt_cmake_dir)
  foreach(_tool_package IN ITEMS Qt6CoreTools Qt6GuiTools Qt6WidgetsTools Qt6LinguistTools
                               Qt6TestTools Qt6NetworkTools Qt6ConcurrentTools Qt6SvgTools Qt6ToolsTools)
    if(EXISTS "${qt_cmake_dir}/${_tool_package}/${_tool_package}Config.cmake")
      set(${_tool_package}_DIR "${qt_cmake_dir}/${_tool_package}"
          CACHE PATH "Paquete de herramientas de Qt (fijado por MiniGIS)" FORCE)
    endif()
  endforeach()
endfunction()

file(TO_CMAKE_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}" _minigis_vcpkg_prefix)

# 1) Instalacion de Qt: Qt6_DIR explicito que no sea el de vcpkg; si el kit solo
#    define CMAKE_PREFIX_PATH, se busca ahi un Qt que no pertenezca a vcpkg.
set(_minigis_qt_dir_is_usable FALSE)
if(Qt6_DIR AND EXISTS "${Qt6_DIR}/Qt6Config.cmake")
  string(FIND "${Qt6_DIR}" "${_minigis_vcpkg_prefix}" _minigis_qt_dir_in_vcpkg)
  if(NOT _minigis_qt_dir_in_vcpkg EQUAL 0)
    set(_minigis_qt_dir_is_usable TRUE)
  else()
    message(STATUS "MiniGIS: Qt6_DIR apunta al Qt de vcpkg (${Qt6_DIR}); se busca el Qt del kit")
  endif()
endif()

if(NOT _minigis_qt_dir_is_usable)
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    file(TO_CMAKE_PATH "${_prefix}" _candidate)
    if(_candidate STREQUAL "")
      continue()
    endif()

    string(FIND "${_candidate}" "${_minigis_vcpkg_prefix}" _candidate_in_vcpkg)
    if(_candidate_in_vcpkg EQUAL 0)
      continue()
    endif()

    if(EXISTS "${_candidate}/lib/cmake/Qt6/Qt6Config.cmake")
      set(Qt6_DIR "${_candidate}/lib/cmake/Qt6" CACHE PATH
          "Instalacion de Qt usada por MiniGIS (se descarta el Qt de vcpkg)" FORCE)
      message(STATUS "MiniGIS: Qt del kit detectado en ${_candidate}")
      break()
    endif()
  endforeach()
endif()

# 2) Variables de Qt que hayan quedado cacheadas apuntando al arbol de vcpkg.
foreach(_qt_var IN ITEMS Qt6Core_DIR Qt6Gui_DIR Qt6Widgets_DIR Qt6Network_DIR Qt6Sql_DIR
                            Qt6EntryPointPrivate_DIR Qt6CoreTools_DIR Qt6GuiTools_DIR
                            Qt6WidgetsTools_DIR Qt6LinguistTools_DIR Qt6ToolsTools_DIR
                            Qt6NetworkTools_DIR Qt6ConcurrentTools_DIR Qt6SvgTools_DIR
                            Qt6TestTools_DIR)
  if(DEFINED ${_qt_var})
    string(FIND "${${_qt_var}}" "${_minigis_vcpkg_prefix}" _minigis_qt_var_in_vcpkg)
    if(_minigis_qt_var_in_vcpkg EQUAL 0)
      message(STATUS "MiniGIS: se descarta ${_qt_var}=${${_qt_var}} (es del arbol de vcpkg)")
      unset(${_qt_var} CACHE)
      unset(${_qt_var})
    endif()
  endif()
endforeach()

if(Qt6_DIR AND EXISTS "${Qt6_DIR}/Qt6Config.cmake")
  get_filename_component(_minigis_qt_cmake_dir "${Qt6_DIR}" DIRECTORY)
  minigis_pin_qt_tools("${_minigis_qt_cmake_dir}")
endif()

# 3) Busqueda de Qt con el arbol de vcpkg fuera de juego. Hay que quitarlo de
#    CMAKE_PREFIX_PATH ademas de CMAKE_FIND_ROOT_PATH: Qt Creator inyecta un
#    "provider" de find_package que busca en CMAKE_PREFIX_PATH y, si el arbol de
#    vcpkg esta ahi, encuentra su Qt y reescribe Qt6_DIR. Se restaura despues,
#    porque GDAL si tiene que salir de vcpkg.
set(_minigis_prefix_path_backup "${CMAKE_PREFIX_PATH}")
set(_minigis_find_root_backup "${CMAKE_FIND_ROOT_PATH}")
set(_minigis_vcpkg_find_root_backup "${VCPKG_CMAKE_FIND_ROOT_PATH}")

list(REMOVE_ITEM CMAKE_PREFIX_PATH
     "${_minigis_vcpkg_prefix}"
     "${_minigis_vcpkg_prefix}/debug")
list(REMOVE_ITEM CMAKE_FIND_ROOT_PATH
     "${_minigis_vcpkg_prefix}"
     "${_minigis_vcpkg_prefix}/debug")
set(VCPKG_CMAKE_FIND_ROOT_PATH "")

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
qt_standard_project_setup(REQUIRES 6.5)

set(CMAKE_PREFIX_PATH "${_minigis_prefix_path_backup}")
set(CMAKE_FIND_ROOT_PATH "${_minigis_find_root_backup}")
set(VCPKG_CMAKE_FIND_ROOT_PATH "${_minigis_vcpkg_find_root_backup}")

message(STATUS "MiniGIS: Qt en ${Qt6_DIR}")

# Coherencia: todas las piezas de Qt deben ser de la misma version.
if(Qt6Core_VERSION AND NOT Qt6Core_VERSION VERSION_EQUAL Qt6_VERSION)
  message(FATAL_ERROR
    "Se han mezclado dos instalaciones de Qt:\n"
    "  Qt6      : ${Qt6_VERSION} (${Qt6_DIR})\n"
    "  Qt6Core  : ${Qt6Core_VERSION} (${Qt6Core_DIR})\n"
    "Define el kit completo, por ejemplo:\n"
    "  -DQt6_DIR=D:/QtWin/6.10.3/msvc2022_64/lib/cmake/Qt6")
endif()

# Aviso si, aun asi, Qt ha salido del arbol de vcpkg.
if(Qt6_DIR MATCHES "vcpkg" AND NOT MINIGIS_ALLOW_VCPKG_QT)
  message(WARNING
    "Qt6 se ha resuelto desde vcpkg (${Qt6_DIR}).\n"
    "  MiniGIS esta pensado para el Qt oficial (D:/QtWin/6.10.3/msvc2022_64).\n"
    "  Fija el kit con -DQt6_DIR=<Qt>/lib/cmake/Qt6 o define MINIGIS_ALLOW_VCPKG_QT=ON.")
endif()

# --- GDAL -----------------------------------------------------------------
# vcpkg instala GDALConfig.cmake -> target GDAL::GDAL (igual que hace QGIS).
find_package(GDAL CONFIG QUIET)
if(NOT GDAL_FOUND AND NOT TARGET GDAL::GDAL)
  find_package(GDAL REQUIRED)
endif()

if(TARGET GDAL::GDAL)
  set(MINIGIS_GDAL_TARGET GDAL::GDAL)
else()
  add_library(minigis_gdal INTERFACE)
  target_include_directories(minigis_gdal INTERFACE ${GDAL_INCLUDE_DIRS})
  target_link_libraries(minigis_gdal INTERFACE ${GDAL_LIBRARIES})
  set(MINIGIS_GDAL_TARGET minigis_gdal)
endif()

# En Windows, la configuracion Debug enlaza con la variante Debug de cada
# dependencia. Si vcpkg solo tiene la release, el enlace fallaria con un error
# confuso: mejor avisar ahora y decir exactamente que hacer.
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND TARGET GDAL::GDAL)
  set(_minigis_gdal_debug_files "")
  foreach(_property IMPORTED_LOCATION_DEBUG IMPORTED_IMPLIB_DEBUG)
    get_target_property(_value GDAL::GDAL ${_property})
    if(_value AND NOT _value MATCHES "-NOTFOUND$")
      list(APPEND _minigis_gdal_debug_files "${_value}")
    endif()
  endforeach()

  foreach(_file IN LISTS _minigis_gdal_debug_files)
    if(NOT EXISTS "${_file}")
      message(FATAL_ERROR
        "La configuracion Debug necesita la variante Debug de GDAL y falta:\n"
        "  ${_file}\n"
        "Opciones:\n"
        "  1) Usa el preset pensado para depurar sin esa variante:\n"
        "       cmake --preset windows-msvc-reldbg\n"
        "  2) Instala tambien la variante Debug en vcpkg:\n"
        "       ${MINIGIS_VCPKG_ROOT}/vcpkg.exe install gdal --triplet ${VCPKG_TARGET_TRIPLET}")
    endif()
  endforeach()
endif()

# --- Opciones de compilacion (MSVC) ---------------------------------------
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra)
endif()

# --- Optimizacion de las rutas calientes (Windows / MSVC) -----------------
# El renderizado vectorial pasa el tiempo en bucles cortos sobre buffers de
# coordenadas (proyeccion de vertices y simplificacion), asi que estas banderas
# afectan de lleno al rendimiento de dibujado. Se aplican como una interfaz que
# enlazan los objetivos, y solo en configuraciones optimizadas.
#
#   /O2        ya lo activa CMAKE_BUILD_TYPE=Release (no se repite aqui)
#   /GL /LTCG  optimizacion en tiempo de enlace: permite incorporar (inline) el
#              codigo de minigis_core dentro del ejecutable y eliminar las
#              llamadas que cruzan la unidad de compilacion, que es donde
#              estaban parte de los bucles de proyeccion.
#   /arch:AVX2 permite vectorizar los bucles de coordenadas. Solo si el parque
#              de equipos lo soporta: AVX2 es de 2013 en adelante.
#   /fp:fast   relaja el modelo de coma flotante (reordenacion, sin NaN/Inf
#              estrictos). Aceptable para dibujar, no para calculo geodetico
#              de precision: por eso es opcional.
option(MINIGIS_LTO "Optimizacion en tiempo de enlace (/GL /LTCG) en Release" ON)
option(MINIGIS_AVX2 "Vectorizacion con /arch:AVX2 (requiere CPU con AVX2)" OFF)
option(MINIGIS_FAST_MATH "Relajar coma flotante con /fp:fast (menos estricto)" OFF)

add_library(minigis_optimizations INTERFACE)
add_library(MiniGIS::optimizations ALIAS minigis_optimizations)

if(MSVC)
  target_compile_options(minigis_optimizations INTERFACE
    $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Oi /Gy /Zc:inline>)

  if(MINIGIS_LTO)
    target_compile_options(minigis_optimizations INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/GL>)
    target_link_options(minigis_optimizations INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/LTCG /OPT:REF /OPT:ICF>)
  endif()

  if(MINIGIS_AVX2)
    target_compile_options(minigis_optimizations INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/arch:AVX2>)
  endif()

  if(MINIGIS_FAST_MATH)
    target_compile_options(minigis_optimizations INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/fp:fast>)
  endif()
else()
  include(CheckIPOSupported)
  if(MINIGIS_LTO)
    check_ipo_supported(RESULT _minigis_ipo_ok OUTPUT _minigis_ipo_error)
    if(_minigis_ipo_ok)
      set_property(TARGET minigis_optimizations PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
      message(STATUS "MiniGIS: LTO no disponible (${_minigis_ipo_error})")
    endif()
  endif()

  if(MINIGIS_FAST_MATH)
    target_compile_options(minigis_optimizations INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-ffast-math>)
  endif()
endif()

# PGO (optimizacion guiada por perfil): no se puede automatizar en una sola
# configuracion, pero estas son las dos fases. La de instrumentacion compila y
# ejecuta el banco de pruebas (--bench) para recoger un perfil real de dibujado:
#
#   cmake --preset windows-msvc-release -DCMAKE_CXX_FLAGS="/GENPROFILE" \
#         -DCMAKE_EXE_LINKER_FLAGS="/LTCG:PGINSTRUMENT" -DMINIGIS_LTO=OFF
#   ninja && minigis --bench 60 --bench-scenario mixed -l mapa.gpkg --size 1600x900
#   cmake --preset windows-msvc-release -DCMAKE_CXX_FLAGS="/USEPROFILE" \
#         -DCMAKE_EXE_LINKER_FLAGS="/LTCG:PGOptimize"
#   ninja

# --- MSVC localizado: prefijo de /showIncludes ----------------------------
# Con Ninja, las dependencias de cabeceras se obtienen espiando la salida de
# /showIncludes. Si el compilador esta en otro idioma (por ejemplo es-ES:
# "Nota: inclusion del archivo:") CMake no reconoce el prefijo: la consola se
# llena de ruido y, peor, no se registran las cabeceras (las recompilaciones
# incrementales dejan de ser fiables). Aqui se detecta el prefijo real.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_CL_SHOWINCLUDES_PREFIX)
  set(_minigis_probe_source "${CMAKE_BINARY_DIR}/minigis_showincludes_probe.cpp")
  set(_minigis_probe_object "${CMAKE_BINARY_DIR}/minigis_showincludes_probe.obj")
  file(WRITE "${_minigis_probe_source}" "#include <cstdio>\nint main() { return 0; }\n")

  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c "${_minigis_probe_source}"
            "/Fo${_minigis_probe_object}"
    OUTPUT_VARIABLE _minigis_probe_output
    ERROR_VARIABLE _minigis_probe_error
    RESULT_VARIABLE _minigis_probe_result)

  if(_minigis_probe_result EQUAL 0)
    string(REPLACE "\n" ";" _minigis_probe_lines "${_minigis_probe_output}")
    foreach(_line IN LISTS _minigis_probe_lines)
      # Cada nota tiene la forma "<prefijo><ruta absoluta>"; el prefijo es lo
      # que queda al quitar la ruta.
      if(_line MATCHES "([A-Za-z]:[\\\\/].*)$")
        set(_minigis_include_path "${CMAKE_MATCH_1}")
        string(REPLACE "${_minigis_include_path}" "" _minigis_prefix "${_line}")
        string(STRIP "${_minigis_prefix}" _minigis_prefix)
        if(NOT _minigis_prefix STREQUAL "")
          set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_minigis_prefix}" CACHE STRING
              "Prefijo de las notas de /showIncludes del compilador" FORCE)
        endif()
        break()
      endif()
    endforeach()
  endif()

  file(REMOVE "${_minigis_probe_source}" "${_minigis_probe_object}")

  if(CMAKE_CL_SHOWINCLUDES_PREFIX)
    message(STATUS "Prefijo /showIncludes: «${CMAKE_CL_SHOWINCLUDES_PREFIX}»")
  else()
    message(WARNING
      "MiniGIS: no se pudo detectar el prefijo de /showIncludes.\n"
      "  La compilacion funcionara, pero las dependencias de cabeceras pueden no registrarse.\n"
      "  Solucion: instala el paquete de idioma ingles de Visual Studio o define\n"
      "  -DCMAKE_CL_SHOWINCLUDES_PREFIX=\"<prefijo>\".")
  endif()
endif()

# --- Informe de configuracion (util para depurar kits en Qt Creator) ------
if(DEFINED GDAL_VERSION)
  set(_minigis_gdal_version "${GDAL_VERSION}")
else()
  set(_minigis_gdal_version "detectada por el target ${MINIGIS_GDAL_TARGET}")
endif()

message(STATUS "")
message(STATUS "==================== MiniGIS ${PROJECT_VERSION} ====================")
message(STATUS " Compilador : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS " Generador  : ${CMAKE_GENERATOR}  Build type: ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS " Qt6        : ${Qt6_VERSION}  (${Qt6_DIR})")
message(STATUS " GDAL       : ${_minigis_gdal_version}  -> ${MINIGIS_GDAL_TARGET}")
message(STATUS " vcpkg      : ${VCPKG_INSTALLED_DIR}  triplet=${VCPKG_TARGET_TRIPLET}  manifest=${VCPKG_MANIFEST_MODE}")
message(STATUS " DLLs vcpkg : ${MINIGIS_VCPKG_BIN_DIR}")
message(STATUS " Ejecutable : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS " Optimizacion: LTO=${MINIGIS_LTO} AVX2=${MINIGIS_AVX2} fp:fast=${MINIGIS_FAST_MATH}")
message(STATUS "==================================================================")
message(STATUS "")

add_subdirectory(src)

if(MINIGIS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
