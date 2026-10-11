#############################################################
# MiniGisVcpkg.cmake
#
# Configuracion de vcpkg siguiendo el enfoque de QGIS
# (cmake/VcpkgToolchain.cmake): el proyecto consume dependencias
# declaradas en un manifiesto (vcpkg/vcpkg.json) y reutiliza el
# arbol ya instalado por vcpkg cuando existe.
#
# Este archivo se incluye ANTES de project() para que las variables
# de vcpkg esten definidas cuando el toolchain de vcpkg se ejecute.
#############################################################

get_filename_component(MINIGIS_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

option(MINIGIS_USE_VCPKG_MANIFEST
       "Resolver dependencias con el manifiesto de vcpkg (OFF = usar el arbol ya instalado)" OFF)

# --- Localizacion de vcpkg ------------------------------------------------
if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  set(MINIGIS_VCPKG_ROOT "$ENV{VCPKG_ROOT}" CACHE PATH "Raiz de vcpkg")
elseif(EXISTS "E:/vcpkg/scripts/buildsystems/vcpkg.cmake")
  set(MINIGIS_VCPKG_ROOT "E:/vcpkg" CACHE PATH "Raiz de vcpkg")
elseif(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
  set(MINIGIS_VCPKG_ROOT "C:/vcpkg" CACHE PATH "Raiz de vcpkg")
else()
  set(MINIGIS_VCPKG_ROOT "" CACHE PATH "Raiz de vcpkg")
endif()

if(MINIGIS_VCPKG_ROOT STREQUAL "")
  message(FATAL_ERROR
    "No se encontro vcpkg.\n"
    "  Define la variable de entorno VCPKG_ROOT, o configura con:\n"
    "    -DMINIGIS_VCPKG_ROOT=<ruta a vcpkg>\n"
    "  o pasa directamente el toolchain:\n"
    "    -DCMAKE_TOOLCHAIN_FILE=<ruta a vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

# Toolchain: si el usuario no lo paso por linea de comandos (presets/kit),
# lo fijamos aqui, como hace QGIS en cmake/VcpkgToolchain.cmake.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${MINIGIS_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE FILEPATH "Toolchain de vcpkg")
  message(STATUS "MiniGIS: usando toolchain de vcpkg en ${CMAKE_TOOLCHAIN_FILE}")
endif()

# --- Triplet y directorio de instalacion ---------------------------------
if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
  set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "Triplet de vcpkg (x64-windows = MSVC dinamico)")
endif()

if(NOT DEFINED VCPKG_INSTALLED_DIR OR VCPKG_INSTALLED_DIR STREQUAL "")
  set(VCPKG_INSTALLED_DIR "${MINIGIS_VCPKG_ROOT}/installed" CACHE PATH "Arbol 'installed' de vcpkg")
endif()

# --- Manifiesto -----------------------------------------------------------
if(MINIGIS_USE_VCPKG_MANIFEST)
  set(VCPKG_MANIFEST_MODE ON CACHE BOOL "vcpkg en modo manifiesto")
  if(NOT DEFINED VCPKG_MANIFEST_DIR OR VCPKG_MANIFEST_DIR STREQUAL "")
    set(VCPKG_MANIFEST_DIR "${MINIGIS_ROOT_DIR}/vcpkg" CACHE PATH "Directorio de vcpkg.json")
  endif()
  if(NOT EXISTS "${VCPKG_MANIFEST_DIR}/vcpkg.json")
    message(FATAL_ERROR "No existe el manifiesto ${VCPKG_MANIFEST_DIR}/vcpkg.json")
  endif()

  # Importante: en modo manifiesto vcpkg considera "ajenos" los paquetes que no
  # estan en el manifiesto y los ELIMINA del arbol de instalacion, ademas de
  # recompilar lo que no coincida en ABI. Por eso se instala siempre en un
  # arbol propio del proyecto y nunca se toca el arbol compartido de vcpkg.
  set(VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed" CACHE PATH
      "Arbol 'installed' propio del proyecto (modo manifiesto)" FORCE)
  message(WARNING
    "MiniGIS: vcpkg en modo manifiesto.\n"
    "  Se instalara en: ${VCPKG_INSTALLED_DIR}\n"
    "  La primera configuracion puede tardar bastante (compila GDAL y sus dependencias).\n"
    "  El modo recomendado y rapido es -DMINIGIS_USE_VCPKG_MANIFEST=OFF, que reutiliza\n"
    "  las dependencias ya compiladas en ${MINIGIS_VCPKG_ROOT}/installed.")
  # En modo manifiesto el triplet es el que vcpkg decida; se respeta el pedido.
  set(VCPKG_BUILD_TYPE "release" CACHE STRING "Solo release: reduce a la mitad el tiempo de compilacion")
else()
  # Modo clasico: sin comprobacion de ABI, se consume lo que ya hay instalado.
  set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "vcpkg en modo manifiesto")
  list(PREPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  message(STATUS "MiniGIS: vcpkg en modo clasico, reutilizando ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

# --- Rutas utiles para el codigo y para copiar DLLs ----------------------
set(MINIGIS_VCPKG_TRIPLET_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
    CACHE PATH "Prefijo de vcpkg para el triplet activo")
set(MINIGIS_VCPKG_SHARE_DIR "${MINIGIS_VCPKG_TRIPLET_DIR}/share"
    CACHE PATH "Datos de GDAL/PROJ instalados por vcpkg")
set(MINIGIS_VCPKG_BIN_DIR "${MINIGIS_VCPKG_TRIPLET_DIR}/bin"
    CACHE PATH "DLLs release de vcpkg")
set(MINIGIS_VCPKG_DEBUG_BIN_DIR "${MINIGIS_VCPKG_TRIPLET_DIR}/debug/bin"
    CACHE PATH "DLLs debug de vcpkg")

foreach(_required "${MINIGIS_VCPKG_TRIPLET_DIR}/share/gdal/GDALConfig.cmake")
  if(NOT EXISTS "${_required}")
    message(WARNING
      "vcpkg no tiene GDAL instalado para el triplet ${VCPKG_TARGET_TRIPLET}.\n"
      "  Compilalo una vez con:\n"
      "    ${MINIGIS_VCPKG_ROOT}/vcpkg.exe install gdal --triplet ${VCPKG_TARGET_TRIPLET}\n"
      "  (o usa el modo manifiesto: -DMINIGIS_USE_VCPKG_MANIFEST=ON)")
  endif()
endforeach()
