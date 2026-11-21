#############################################################
# MiniGisDllVersions.cmake
#
# Genera un encabezado C++ con la version de cada DLL que instala vcpkg.
#
# Para que "Acerca de" pueda ensenar la version de todas las DLLs que usa la
# aplicacion hace falta una fuente que no sea el recurso de version del PE:
# arrow.dll, parquet.dll, proj_9.dll o geos_c.dll, entre otras, se compilan sin
# el. vcpkg si sabe que puerto y que version aporta cada archivo: lo deja en
# installed/vcpkg/info/<puerto>_<version>_<triplet>.list.
#
# El mapa se genera al configurar porque el arbol de vcpkg no se distribuye
# junto al ejecutable. Si el arbol no existe (o es una instalacion sin los
# .list), el encabezado queda vacio y "Acerca de" solo muestra las versiones
# que si llevan recurso en el PE.
#############################################################

function(minigis_generate_vcpkg_dll_versions out_header)
  set(_info_dir "${VCPKG_INSTALLED_DIR}/vcpkg/info")
  set(_header "${CMAKE_BINARY_DIR}/generated/minigisvcpkgversions.h")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

  set(_entries "")
  if(EXISTS "${_info_dir}")
    file(GLOB _lists "${_info_dir}/*_${VCPKG_TARGET_TRIPLET}.list")
    foreach(_list IN LISTS _lists)
      # <puerto>_<version>_<triplet>.list; el primer grupo es codicioso para que
      # un puerto con guiones (boost-filesystem) o una version con "+"
      # (libevent_2.1.12+20230128) se separen por el ultimo guion bajo util.
      # NAME_WE no sirve: corta por el primer punto y se comeria "20260107.1".
      get_filename_component(_name "${_list}" NAME)
      string(REGEX REPLACE "[.]list$" "" _stem "${_name}")
      if(NOT _stem MATCHES "^(.+)_([0-9][^_]*)_${VCPKG_TARGET_TRIPLET}$")
        continue()
      endif()
      set(_port "${CMAKE_MATCH_1}")
      set(_version "${CMAKE_MATCH_2}")

      # Solo la variante release: es la que se copia junto al ejecutable release
      # y las dos variantes comparten version del puerto.
      file(STRINGS "${_list}" _lines)
      foreach(_line IN LISTS _lines)
        if(_line MATCHES "^${VCPKG_TARGET_TRIPLET}/bin/([^/]+[.]dll)$")
          string(APPEND _entries "    {\"${CMAKE_MATCH_1}\", \"${_port}\", \"${_version}\"},\n")
        endif()
      endforeach()
    endforeach()
  endif()

  if(_entries STREQUAL "")
    # Un array de tamano cero no es C++ valido: una entrada vacia que el codigo
    # de ejecucion ignora.
    set(_entries "    {\"\", \"\", \"\"},\n")
  endif()

  file(WRITE "${_header}"
"// Generado por cmake/MiniGisDllVersions.cmake: no editar a mano.
//
// Version del puerto de vcpkg que aporta cada DLL. Es la fuente para las DLLs
// que no llevan recurso de version en el PE (arrow.dll, parquet.dll,
// proj_9.dll...). Las que si lo llevan se leen del propio archivo.
#pragma once

namespace minigis
{

struct VcpkgDllVersion
{
  const char *dll;
  const char *port;
  const char *version;
};

inline constexpr VcpkgDllVersion kVcpkgDllVersions[] = {
${_entries}};

} // namespace minigis
")

  set(${out_header} "${_header}" PARENT_SCOPE)
  message(STATUS "MiniGIS: versiones de DLLs de vcpkg en ${_header}")
endfunction()
