#pragma once

#include <QList>
#include <QString>

namespace minigis
{

/** DLL cargada en el proceso desde la carpeta del ejecutable. */
struct RuntimeDllInfo
{
  /** Ruta relativa a la carpeta del ejecutable, con separadores nativos. */
  QString path;

  /** Nombre del archivo, por ejemplo gdal.dll. */
  QString name;

  /** Version del archivo (recurso del PE) o del puerto de vcpkg; vacia si no se conoce. */
  QString version;

  /** Puerto de vcpkg que aporta la DLL (vacio para Qt o DLLs sin puerto). */
  QString component;
};

/**
 * DLLs cargadas en el proceso desde la carpeta del ejecutable, ordenadas por
 * ruta relativa.
 *
 * Solo se incluyen las que aporta la aplicacion (Qt, GDAL, vcpkg y sus
 * dependencias) y no las del sistema, que no distinguen una build de otra. El
 * dialogo "Acerca de" usa esta lista para ensenar que DLLs se estan ejecutando.
 *
 * En plataformas distintas de Windows devuelve una lista vacia: la lectura del
 * recurso de version y la enumeracion de modulos son APIs de Windows.
 */
QList<RuntimeDllInfo> loadedRuntimeDlls();

/**
 * Version de una DLL cargada, buscada por nombre de archivo.
 *
 * Devuelve una cadena vacia si la DLL no esta cargada o si no se conoce su
 * version. Se usa para resumir Arrow y Parquet, que el driver de GDAL carga
 * como dependencia.
 */
QString loadedRuntimeDllVersion(const QString &name);

/** Texto de una linea por DLL ("gdal.dll 3.12.4"), para diagnosticos y pruebas. */
QString runtimeDllsText();

} // namespace minigis
