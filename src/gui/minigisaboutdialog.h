#pragma once

#include <QDialog>

class QTableWidget;

namespace minigis
{

/**
 * Dialogo "Acerca de MiniGIS".
 *
 * Ademas de las versiones del motor, muestra la tabla de DLLs que el proceso
 * ha cargado desde la carpeta del ejecutable. Es la forma de comprobar de un
 * vistazo que build de GDAL, Arrow y Parquet se esta ejecutando: el driver de
 * GeoParquet vive dentro de gdal.dll y las DLLs de Arrow no llevan recurso de
 * version en el PE, asi que su version se toma del puerto de vcpkg que las
 * instala.
 */
class AboutDialog : public QDialog
{
  Q_OBJECT

public:
  explicit AboutDialog(QWidget *parent = nullptr);

private:
  QTableWidget *mDllTable = nullptr;
};

} // namespace minigis
