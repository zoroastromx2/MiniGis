#pragma once

#include <QString>
#include <QStringList>

namespace minigis
{

/** Entorno GDAL/PROJ resuelto durante el arranque de la aplicacion. */
struct RuntimeInfo
{
  QString gdalVersion;
  QString projVersion;
  QString projDataDir;
  QString gdalDataDir;
  bool projDataFound = false;
  bool gdalDataFound = false;
  QStringList notes;

  /** Texto multilinea listo para --version o el dialogo "Acerca de". */
  QString summary() const;
};

/**
 * Registra todos los drivers de GDAL y localiza los directorios de datos
 * (proj.db y GDAL_DATA). Es idempotente: se puede llamar varias veces.
 *
 * Esta funcion debe ejecutarse antes de abrir cualquier capa.
 */
RuntimeInfo initializeGdal();

/** Ultimo mensaje de error emitido por GDAL (cadena vacia si no hubo). */
QString lastGdalError();

/** Limpia el ultimo error de GDAL antes de una operacion. */
void clearLastGdalError();

/** Nombres de los drivers de GDAL con capacidad vectorial, ordenados. */
QStringList vectorDriverNames();

} // namespace minigis
