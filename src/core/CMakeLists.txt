############################################################
# minigis_core: acceso a datos (GDAL/OGR), estilos y renderizado.
# No depende de widgets, igual que qgis_core.

set(MINIGIS_CORE_SRCS
  minigiserror.h
  minigisgdalinit.h
  minigisgdalinit.cpp
  minigislayerkind.h
  minigislayerkind.cpp
  minigisrectangle.h
  minigisrectangle.cpp
  minigisxmltree.h
  minigisxmltree.cpp
  minigisziparchive.h
  minigisziparchive.cpp
  minigisqgissymbology.h
  minigisqgissymbology.cpp
  minigisqgisproject.h
  minigisqgisproject.cpp
  minigisstyle.h
  minigisstyle.cpp
  minigisrasterstyle.h
  minigisrasterlayer.h
  minigisrasterlayer.cpp
  minigisrasterrenderer.h
  minigisrasterrenderer.cpp
  minigisvectorlayer.h
  minigisvectorlayer.cpp
  minigisgeometrystore.h
  minigisgeometrystore.cpp
  minigiskmlinspect.h
  minigiskmlinspect.cpp
  minigisgeometrykinds.h
  minigisgeometryhit.h
  minigisgeometryhit.cpp
  minigisidentify.h
  minigisidentify.cpp
  minigislabelstyle.h
  minigislabelrenderer.h
  minigislabelrenderer.cpp
  minigisshapefileindex.h
  minigisshapefileindex.cpp
  minigisvectorloader.h
  minigisvectorloader.cpp
  minigispgsource.h
  minigispgsource.cpp
  minigisconnections.h
  minigisconnections.cpp
  minigispgcredentials.h
  minigispgcredentials.cpp
  minigispgcatalog.h
  minigispgcatalog.cpp
  minigispgcheck.h
  minigispgcheck.cpp
  minigispgsession.h
  minigispgsession.cpp
  minigismaprenderer.h
  minigismaprenderer.cpp
  minigisfastrenderer.h
  minigisfastrenderer.cpp
)

add_library(minigis_core STATIC ${MINIGIS_CORE_SRCS})
add_library(MiniGIS::core ALIAS minigis_core)

target_include_directories(minigis_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# minizip lo instala vcpkg como dependencia de GDAL; se usa para leer y escribir
# el ZIP de los proyectos .qgz (la API C clasica zip.h/unzip.h).
find_package(MINIZIP CONFIG REQUIRED)

target_link_libraries(minigis_core PRIVATE MiniGIS::optimizations)

if(WIN32)
  # DPAPI (CryptProtectData) para recordar la contrasena de PostGIS sin
  # guardarla en claro en el perfil del usuario.
  target_link_libraries(minigis_core PRIVATE crypt32)
endif()

target_link_libraries(minigis_core
  PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Concurrent
    MINIZIP::minizip
    ${MINIGIS_GDAL_TARGET}
)

target_compile_definitions(minigis_core
  PUBLIC
    MINIGIS_VERSION="${PROJECT_VERSION}"
    MINIGIS_SOURCE_DIR="${MINIGIS_ROOT_DIR}"
    MINIGIS_SAMPLE_DATA_DIR="${MINIGIS_ROOT_DIR}/data/sample"
    MINIGIS_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    MINIGIS_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    # $<CONFIG> y no CMAKE_BUILD_TYPE + CMAKE_CONFIGURATION_TYPES: con un
    # generador multi-config (Visual Studio, Ninja Multi-Config) la segunda es
    # una lista "Debug;Release;..." y, al ir entre comillas, los ";" partirian
    # la definicion en varias: moc recibia "-DMINIGIS_BUILD_TYPE=\"ReleaseDebug"
    # y fallaba con un error de sintaxis en qnamespace.h.
    MINIGIS_BUILD_TYPE="$<CONFIG>"
  PRIVATE
    # Rutas de datos de GDAL/PROJ instaladas por vcpkg (se usan como ultimo
    # recurso si las variables de entorno GDAL_DATA/PROJ_DATA no existen).
    MINIGIS_VCPKG_SHARE_DIR="${MINIGIS_VCPKG_SHARE_DIR}"
)

set_target_properties(minigis_core PROPERTIES
  AUTOMOC ON
  POSITION_INDEPENDENT_CODE ON
)
