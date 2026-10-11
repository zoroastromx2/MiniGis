############################################################
# minigis_gui: widgets (lienzo, panel de capas, tabla de atributos).
# Depende de minigis_core, igual que qgis_gui depende de qgis_core.

set(MINIGIS_GUI_SRCS
  minigismapcanvas.h
  minigismapcanvas.cpp
  minigisattributetablemodel.h
  minigisattributetablemodel.cpp
  minigislayerpanel.h
  minigislayerpanel.cpp
  minigisstyledialog.h
  minigisstyledialog.cpp
  minigismainwindow.h
  minigismainwindow.cpp
)

add_library(minigis_gui STATIC ${MINIGIS_GUI_SRCS})
add_library(MiniGIS::gui ALIAS minigis_gui)

target_include_directories(minigis_gui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(minigis_gui
  PUBLIC
    minigis_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

set_target_properties(minigis_gui PROPERTIES AUTOMOC ON)
