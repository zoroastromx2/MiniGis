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
  minigislabelstyle.h
  minigislabelrenderer.h
  minigislabelrenderer.cpp
  minigisshapefileindex.h
  minigisshapefileindex.cpp
  minigisvectorloader.h
  minigisvectorloader.cpp
  minigismaprenderer.h
  minigismaprenderer.cpp
  minigisfastrenderer.h
  minigisfastrenderer.cpp
)

add_library(minigis_core STATIC ${MINIGIS_CORE_SRCS})
add_library(MiniGIS::core ALIAS minigis_core)

target_include_directories(minigis_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(minigis_core PRIVATE MiniGIS::optimizations)

target_link_libraries(minigis_core
  PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Concurrent
    ${MINIGIS_GDAL_TARGET}
)

target_compile_definitions(minigis_core
  PUBLIC
    MINIGIS_VERSION="${PROJECT_VERSION}"
    MINIGIS_SOURCE_DIR="${MINIGIS_ROOT_DIR}"
    MINIGIS_SAMPLE_DATA_DIR="${MINIGIS_ROOT_DIR}/data/sample"
    MINIGIS_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    MINIGIS_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    MINIGIS_BUILD_TYPE="${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}"
  PRIVATE
    # Rutas de datos de GDAL/PROJ instaladas por vcpkg (se usan como ultimo
    # recurso si las variables de entorno GDAL_DATA/PROJ_DATA no existen).
    MINIGIS_VCPKG_SHARE_DIR="${MINIGIS_VCPKG_SHARE_DIR}"
)

set_target_properties(minigis_core PROPERTIES
  AUTOMOC ON
  POSITION_INDEPENDENT_CODE ON
)
