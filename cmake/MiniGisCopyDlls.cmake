#############################################################
# MiniGisCopyDlls.cmake  (se ejecuta con: cmake -P)
#
# Copia las DLLs de las dependencias compartidas instaladas por
# vcpkg junto al ejecutable, eligiendo el arbol debug o release
# segun la configuracion que se este construyendo.
#
# IMPORTANTE: las DLLs de Qt se excluyen a proposito. El proyecto usa
# el Qt oficial (D:/QtWin/...), pero el arbol de vcpkg puede tener
# otro Qt instalado para otros proyectos. Copiar ambas versiones en
# la misma carpeta mezcla dos instalaciones de Qt y la aplicacion
# termina sin encontrar sus plugins de plataforma. Los archivos
# bin\Qt6*.dll ya los copia CMake a partir del cierre de enlazado del
# ejecutable (ver MiniGisRuntime.cmake).
#############################################################

if(MINIGIS_DLL_CONFIG STREQUAL "Debug" AND EXISTS "${MINIGIS_DLL_SRC_DEBUG}")
  set(_src "${MINIGIS_DLL_SRC_DEBUG}")
else()
  set(_src "${MINIGIS_DLL_SRC_RELEASE}")
endif()

if(NOT EXISTS "${_src}")
  message(STATUS "MiniGIS: no hay DLLs de vcpkg en ${_src} (nada que copiar)")
  return()
endif()

file(GLOB _candidates "${_src}/*.dll")

set(_dlls "")
set(_skipped_qt 0)
foreach(_dll IN LISTS _candidates)
  get_filename_component(_name "${_dll}" NAME)
  if(_name MATCHES "^Qt6")
    math(EXPR _skipped_qt "${_skipped_qt} + 1")
  else()
    list(APPEND _dlls "${_dll}")
  endif()
endforeach()

if(NOT _dlls)
  message(STATUS "MiniGIS: ${_src} no contiene DLLs que copiar")
  return()
endif()

list(LENGTH _dlls _count)
# Forma "origenes... destino": la opcion -t no existe en el CMake 3.30 que
# distribuye Qt Creator, y alli el ultimo origen se interpretaba como carpeta.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_dlls} "${MINIGIS_DLL_DST}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(WARNING "MiniGIS: fallo la copia de DLLs de vcpkg (codigo ${_rc})")
else()
  message(STATUS "MiniGIS: ${_count} DLLs de vcpkg en ${MINIGIS_DLL_DST} (${_skipped_qt} DLLs de Qt ignoradas)")
endif()
