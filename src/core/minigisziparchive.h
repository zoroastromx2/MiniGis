#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include "minigiserror.h"

namespace minigis
{

/** Una entrada completa de un ZIP, ya descomprimida en memoria. */
struct ZipEntry
{
  QString name;
  QByteArray data;

  bool isDirectory() const { return name.endsWith(QLatin1Char('/')); }
};

/**
 * Lectura y escritura de archivos ZIP con minizip.
 *
 * Un proyecto `.qgz` es un ZIP con el XML del proyecto y, a veces, archivos
 * auxiliares (estilos, adjuntos). Para guardar sin perder nada se leen todas
 * las entradas, se sustituye solo la del `.qgs` y se escribe un ZIP nuevo
 * completo.
 *
 * Se usa minizip (la misma biblioteca con la que GDAL lee `/vsizip/`) porque es
 * una API C estable y ya forma parte del arbol de dependencias; el
 * `QZipReader`/`QZipWriter` de Qt es API privada y no se usa.
 */
class ZipArchive
{
public:
  /**
   * Lee todas las entradas de un ZIP. Se omiten las entradas de directorio.
   * Devuelve false con un `Error` accionable si el archivo no es un ZIP valido
   * o si una entrada supera el limite de seguridad (256 MB).
   */
  static bool read(const QString &path, QVector<ZipEntry> *entries, Error *error);

  /**
   * Escribe un ZIP nuevo con las entradas indicadas. El llamador debe pasar una
   * ruta temporal: si algo falla, el archivo a medias se borra.
   */
  static bool write(const QString &path, const QVector<ZipEntry> &entries, Error *error);

  /** Nombre de la entrada del proyecto dentro del ZIP (la que acaba en .qgs). */
  static QString projectEntryName(const QVector<ZipEntry> &entries);
};

} // namespace minigis
