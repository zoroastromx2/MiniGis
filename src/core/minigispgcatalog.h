#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <gdal.h>

#include "minigiserror.h"
#include "minigispgsource.h"

namespace minigis
{

/** Tabla con geometria de un esquema PostGIS, tal como la entrega GDAL. */
struct PgTableInfo
{
  QString name;
  /** Nombre OGC estable: Point, MultiPolygon... */
  QString geometryName;
  /** Tipo simplificado para la interfaz: Punto, Linea, Poligono. */
  QString geometryKind;
  /** Estimacion de pg_class; -1 si GDAL no la conoce. */
  qint64 features = -1;
  int srid = 0;
  /** False si el diagnostico no pudo abrir la tabla por separado. */
  bool openOk = true;
};

/**
 * Catalogo de una base PostGIS usando solo la API C de GDAL.
 *
 * Por que en dos pasos: abrir la base entera devuelve miles de capas (5 121 en
 * el bdware del usuario), lo que no se puede presentar ni cargar. La apertura
 * global sirve solo para enumerar esquemas; cada esquema se abre aparte y
 * devuelve sus tablas con geometria y numero de elementos.
 */
class PgCatalog
{
public:
  /** True si este GDAL trae el driver PostgreSQL. */
  static bool driverAvailable();

  /**
   * Esquemas con capas vectoriales, ordenados. Abre la base sin acotar y
   * deduplica el prefijo de cada nombre (`esquema.tabla`); una capa sin punto
   * se considera del esquema `public`.
   */
  static bool listSchemas(const PgConnection &connection, const QString &password,
                          QStringList *schemas, PgFailure *failure = nullptr,
                          Error *error = nullptr);

  /**
   * Tablas con geometria del esquema, en el orden de GDAL, con su tipo y su
   * numero estimado de elementos. Un esquema sin capas devuelve la lista vacia
   * sin error.
   */
  static bool listTables(const PgConnection &connection, const QString &password,
                         const QString &schema, QVector<PgTableInfo> *tables,
                         PgFailure *failure = nullptr, Error *error = nullptr);

  /**
   * Cierra la conexion de catalogo que se conserva entre dialogos. Se usa al
   * pulsar «Actualizar» para ver tablas nuevas o tras cambiar la contrasena.
   */
  static void closeSession();
};

/**
 * Abre el dataset de una fuente pg: con la conexion y la contrasena resueltas
 * (catalogo compartido y PgCredentials). Es el unico camino para abrir PostGIS:
 * concentra el saneado de la cadena y la clasificacion del fallo.
 *
 * Devuelve nullptr y deja el motivo en `error`/`failure`; el dataset lo cierra
 * quien lo abre con GDALClose.
 */
GDALDatasetH openPgDataset(const PgSource &source, Error *error = nullptr,
                           PgFailure *failure = nullptr);

} // namespace minigis
