############################################################
# minigis: ejecutable.
#
# Se construye como aplicacion de consola (sin WIN32) a proposito:
#   * los modos CLI (--info, --snapshot, --version) escriben en stdout/stderr
#   * Qt Creator muestra esa salida en el panel "Application Output"
#   * al depurar se ve igualmente la interfaz grafica con normalidad
# Si prefieres no ver la consola al lanzarlo desde el Explorador, comenta
# la propiedad WIN32_EXECUTABLE (perderias la salida CLI).

add_executable(minigis main.cpp)
add_executable(MiniGIS::app ALIAS minigis)

target_link_libraries(minigis
  PRIVATE
    minigis_gui
    minigis_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    ${MINIGIS_GDAL_TARGET}
    # Banderas de optimizacion (LTO, AVX2, fp:fast) tambien en el enlazado final.
    MiniGIS::optimizations
)

set_target_properties(minigis PROPERTIES
  AUTOMOC ON
  WIN32_EXECUTABLE FALSE
)

# Deja Qt y las dependencias de GDAL junto al ejecutable.
minigis_deploy_runtime(minigis)

install(TARGETS minigis RUNTIME DESTINATION bin)
