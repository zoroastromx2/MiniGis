#include "minigispgcatalog.h"

#include "minigisconnections.h"
#include "minigisgdalinit.h"
#include "minigispgcredentials.h"
#include "minigisstyle.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QSet>

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_core.h>
#include <ogr_srs_api.h>

namespace minigis
{
namespace
{

/** Traduce el tipo de OGR al tipo simplificado de MiniGIS (igual que la capa). */
GeometryKind kindForGeometryType(OGRwkbGeometryType geometryType)
{
  switch (OGR_GT_Flatten(geometryType))
  {
    case wkbPoint:
    case wkbMultiPoint:
      return GeometryKind::Point;

    case wkbLineString:
    case wkbMultiLineString:
    case wkbCircularString:
    case wkbCompoundCurve:
    case wkbMultiCurve:
      return GeometryKind::Line;

    case wkbPolygon:
    case wkbMultiPolygon:
    case wkbCurvePolygon:
    case wkbMultiSurface:
    case wkbTriangle:
      return GeometryKind::Polygon;

    default:
      return GeometryKind::Unknown;
  }
}

/** Nombre OGC sin espacios, que es el que documenta el contrato JSON. */
QString ogcGeometryName(OGRwkbGeometryType geometryType)
{
  switch (OGR_GT_Flatten(geometryType))
  {
    case wkbPoint:
      return QStringLiteral("Point");
    case wkbLineString:
      return QStringLiteral("LineString");
    case wkbPolygon:
      return QStringLiteral("Polygon");
    case wkbMultiPoint:
      return QStringLiteral("MultiPoint");
    case wkbMultiLineString:
      return QStringLiteral("MultiLineString");
    case wkbMultiPolygon:
      return QStringLiteral("MultiPolygon");
    case wkbGeometryCollection:
      return QStringLiteral("GeometryCollection");
    case wkbCircularString:
      return QStringLiteral("CircularString");
    case wkbCompoundCurve:
      return QStringLiteral("CompoundCurve");
    case wkbCurvePolygon:
      return QStringLiteral("CurvePolygon");
    case wkbMultiCurve:
      return QStringLiteral("MultiCurve");
    case wkbMultiSurface:
      return QStringLiteral("MultiSurface");
    case wkbTriangle:
      return QStringLiteral("Triangle");
    default:
      return QStringLiteral("Unknown");
  }
}

QString failureMessage(PgFailure failure, const PgConnection &connection)
{
  switch (failure)
  {
    case PgFailure::Authentication:
      return QStringLiteral("La contrasena de «%1» no es valida.").arg(connection.name);
    case PgFailure::Connection:
      return QStringLiteral("No se pudo conectar con %1:%2.")
        .arg(connection.server, QString::number(connection.port));
    case PgFailure::MissingObject:
      return QStringLiteral("No existe el esquema o la tabla solicitada en «%1».")
        .arg(connection.name);
    case PgFailure::DriverMissing:
      return QStringLiteral("Este GDAL no puede abrir PostGIS.");
    case PgFailure::Catalog:
    case PgFailure::UnknownConnection:
    case PgFailure::None:
    case PgFailure::Other:
      break;
  }
  return QStringLiteral("GDAL no pudo abrir la conexion PostGIS «%1».").arg(connection.name);
}

QString failureHint(PgFailure failure, const QString &gdalMessage)
{
  QString hint;
  switch (failure)
  {
    case PgFailure::Authentication:
      hint = QStringLiteral("Vuelve a escribirla; la de Connections.xml esta cifrada y "
                            "MiniGIS no la usa.");
      break;
    case PgFailure::Connection:
      hint = QStringLiteral("Comprueba la red o la VPN y que el servidor siga en ese puerto.");
      break;
    case PgFailure::MissingObject:
      hint = QStringLiteral("Vuelve a listar las tablas del esquema para ver cuales existen.");
      break;
    case PgFailure::DriverMissing:
      hint = QStringLiteral("Recompila con la feature postgresql de vcpkg (GDAL necesita libpq).");
      break;
    case PgFailure::Catalog:
    case PgFailure::UnknownConnection:
    case PgFailure::None:
    case PgFailure::Other:
      hint = QStringLiteral("Comprueba los datos de la conexion.");
      break;
  }

  if (!gdalMessage.isEmpty())
    hint += QStringLiteral("\nGDAL informa: ") + gdalMessage;
  return hint;
}

/**
 * Abre el dataset de un esquema (o de toda la base si `schema` esta vacio).
 * Devuelve nullptr y rellena `error`/`failure` si no se puede.
 */
GDALDatasetH openWithPassword(const PgConnection &connection, const QString &password,
                              const QString &schema, const QString &table, PgFailure *failure,
                              Error *error)
{
  const auto fail = [error](const QString &message, const QString &hint) -> GDALDatasetH {
    if (error)
      *error = Error {message, hint};
    return nullptr;
  };

  if (failure)
    *failure = PgFailure::None;

  if (!connection.isValid())
  {
    if (failure)
      *failure = PgFailure::Other;
    return fail(QStringLiteral("La conexion no tiene nombre, servidor o base."),
                QStringLiteral("Revisa el Connections.xml o elige otra conexion."));
  }

  initializeGdal();

  if (!GDALGetDriverByName("PostgreSQL"))
  {
    if (failure)
      *failure = PgFailure::DriverMissing;
    return fail(QStringLiteral("Este GDAL no puede abrir PostGIS."),
                QStringLiteral("Recompila con la feature postgresql de vcpkg: el driver necesita "
                               "libpq junto al ejecutable."));
  }

  PgSource source;
  source.connection = connection.name;
  source.schema = schema;
  source.table = table;

  const QString conninfo = buildConninfo(source, connection, password);
  clearLastGdalError();

  GDALDatasetH dataset =
    GDALOpenEx(conninfo.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
               nullptr);
  if (dataset)
    return dataset;

  // Nada de la cadena de conexion puede llegar al mensaje: se sanea siempre.
  const QString gdalMessage = scrubSecrets(lastGdalError());
  const PgFailure kind = classifyPgFailure(gdalMessage);
  if (failure)
    *failure = kind;

  return fail(failureMessage(kind, connection), failureHint(kind, gdalMessage));
}

// Conexiones de catalogo que se conservan mientras la aplicacion vive: volver a
// abrir el dialogo PostGIS no reconecta ni se autentica de nuevo. Son dos porque
// tienen forma distinta: una lista todos los esquemas y otra solo el esquema
// cuyas tablas se estan viendo. Se usan solo desde el hilo del dialogo o de la
// CLI; el cargador de capas abre su propio dataset para cada tabla.
QMutex gSessionMutex;
QString gSchemasKey;
GDALDatasetH gSchemasDataset = nullptr;
QString gTablesKey;
GDALDatasetH gTablesDataset = nullptr;

// Cache del catalogo para la sesion: reabrir el dialogo muestra esquemas y
// tablas al instante, sin consultar el servidor. «Actualizar» cierra la sesion
// y la vacia, de modo que una tabla nueva aparece al refrescar.
QString gCacheKey;
QStringList gSchemasCache;
bool gSchemasCached = false;
QHash<QString, QVector<PgTableInfo>> gTablesCache;

void closeCatalogSessionLocked()
{
  if (gSchemasDataset)
  {
    GDALClose(gSchemasDataset);
    gSchemasDataset = nullptr;
  }
  if (gTablesDataset)
  {
    GDALClose(gTablesDataset);
    gTablesDataset = nullptr;
  }
  gSchemasKey.clear();
  gTablesKey.clear();
  gCacheKey.clear();
  gSchemasCache.clear();
  gSchemasCached = false;
  gTablesCache.clear();
}

void closeCatalogSessionAtExit()
{
  QMutexLocker locker(&gSessionMutex);
  closeCatalogSessionLocked();
}

void ensureExitRoutine()
{
  static bool routineAdded = false;
  if (routineAdded)
    return;
  // Al cerrar la aplicacion se cierran las conexiones del catalogo, para no
  // dejar sockets abiertos en el servidor.
  qAddPostRoutine(closeCatalogSessionAtExit);
  routineAdded = true;
}

/** Dataset de todos los esquemas (para listar esquemas), abierto una vez. */
GDALDatasetH schemasDatasetLocked(const PgConnection &connection, const QString &password,
                                  PgFailure *failure, Error *error)
{
  const QString key = connection.fingerprint();
  if (gSchemasDataset && gSchemasKey == key)
    return gSchemasDataset;

  if (gSchemasDataset)
  {
    GDALClose(gSchemasDataset);
    gSchemasDataset = nullptr;
    gSchemasKey.clear();
  }

  GDALDatasetH dataset =
    openWithPassword(connection, password, QString(), QString(), failure, error);
  if (!dataset)
    return nullptr;

  gSchemasDataset = dataset;
  gSchemasKey = key;
  ensureExitRoutine();
  return gSchemasDataset;
}

/** Dataset de un esquema concreto (para listar sus tablas), abierto una vez. */
GDALDatasetH tablesDatasetLocked(const PgConnection &connection, const QString &password,
                                 const QString &schema, PgFailure *failure, Error *error)
{
  const QString key = connection.fingerprint() + QLatin1Char('|') + schema;
  if (gTablesDataset && gTablesKey == key)
    return gTablesDataset;

  if (gTablesDataset)
  {
    GDALClose(gTablesDataset);
    gTablesDataset = nullptr;
    gTablesKey.clear();
  }

  GDALDatasetH dataset =
    openWithPassword(connection, password, schema, QString(), failure, error);
  if (!dataset)
    return nullptr;

  gTablesDataset = dataset;
  gTablesKey = key;
  ensureExitRoutine();
  return gTablesDataset;
}

} // namespace

bool PgCatalog::driverAvailable()
{
  initializeGdal();
  return GDALGetDriverByName("PostgreSQL") != nullptr;
}

bool PgCatalog::listSchemas(const PgConnection &connection, const QString &password,
                            QStringList *schemas, PgFailure *failure, Error *error)
{
  if (schemas)
    schemas->clear();

  QMutexLocker locker(&gSessionMutex);

  const QString fingerprint = connection.fingerprint();
  if (gSchemasCached && gCacheKey == fingerprint && schemas)
  {
    *schemas = gSchemasCache;
    return true;
  }

  GDALDatasetH dataset = schemasDatasetLocked(connection, password, failure, error);
  if (!dataset)
    return false;

  QStringList found;
  QSet<QString> seen;
  const int layerCount = GDALDatasetGetLayerCount(dataset);

  for (int index = 0; index < layerCount; ++index)
  {
    OGRLayerH layer = GDALDatasetGetLayer(dataset, index);
    if (!layer)
      continue;

    const char *name = OGR_L_GetName(layer);
    const QString text = QString::fromUtf8(name ? name : "");
    if (text.isEmpty())
      continue;

    // GDAL nombra las capas "esquema.tabla" cuando abre varios esquemas. Se
    // asume un unico punto: un nombre de tabla con punto es una rareza que se
    // documenta en la spec.
    const int dot = text.indexOf(QLatin1Char('.'));
    const QString schemaName = dot > 0 ? text.left(dot) : QStringLiteral("public");

    if (seen.contains(schemaName))
      continue;
    seen.insert(schemaName);
    found << schemaName;
  }

  // El dataset NO se cierra: queda como conexion viva del catalogo.
  found.sort(Qt::CaseInsensitive);

  gSchemasCache = found;
  gSchemasCached = true;
  gCacheKey = fingerprint;

  if (schemas)
    *schemas = found;
  return true;
}

bool PgCatalog::listTables(const PgConnection &connection, const QString &password,
                           const QString &schema, QVector<PgTableInfo> *tables,
                           PgFailure *failure, Error *error)
{
  if (tables)
    tables->clear();

  QMutexLocker locker(&gSessionMutex);

  const QString cacheKey = connection.fingerprint() + QLatin1Char('|') + schema;
  const auto cached = gTablesCache.constFind(cacheKey);
  if (cached != gTablesCache.constEnd())
  {
    if (tables)
      *tables = cached.value();
    return true;
  }

  GDALDatasetH dataset = tablesDatasetLocked(connection, password, schema, failure, error);
  if (!dataset)
    return false;

  const int layerCount = GDALDatasetGetLayerCount(dataset);
  for (int index = 0; index < layerCount; ++index)
  {
    OGRLayerH layer = GDALDatasetGetLayer(dataset, index);
    if (!layer)
      continue;

    PgTableInfo info;
    const char *name = OGR_L_GetName(layer);
    info.name = QString::fromUtf8(name ? name : "");

    const OGRwkbGeometryType geometryType = OGR_L_GetGeomType(layer);
    info.geometryName = ogcGeometryName(geometryType);
    info.geometryKind = Style::kindName(kindForGeometryType(geometryType));

    // force = FALSE: el driver PostgreSQL responde con la estimacion de
    // pg_class, que basta para avisar antes de cargar y no bloquea la interfaz.
    info.features = static_cast<qint64>(OGR_L_GetFeatureCount(layer, FALSE));

    if (OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(layer))
    {
      if (const char *code = OSRGetAuthorityCode(srs, nullptr))
        info.srid = QString::fromUtf8(code).toInt();
    }

    if (tables)
      tables->append(info);
  }

  gTablesCache.insert(cacheKey, tables ? *tables : QVector<PgTableInfo>());
  return true;
}

void PgCatalog::closeSession()
{
  QMutexLocker locker(&gSessionMutex);
  closeCatalogSessionLocked();
}

GDALDatasetH openPgDataset(const PgSource &source, Error *error, PgFailure *failure)
{
  if (failure)
    *failure = PgFailure::None;

  if (error)
  {
    error->message.clear();
    error->hint.clear();
  }

  if (!source.isValid())
  {
    if (failure)
      *failure = PgFailure::Other;
    if (error)
      *error = Error {QStringLiteral("La fuente PostGIS no indica conexion y esquema."),
                      QStringLiteral("Usa el formato pg:conexion/esquema/tabla.")};
    return nullptr;
  }

  Error catalogError;
  const PgConnectionCatalog *catalog = PgConnectionCatalog::shared(&catalogError);
  if (!catalog)
  {
    if (failure)
      *failure = PgFailure::Catalog;
    if (error)
      *error = catalogError;
    return nullptr;
  }

  const PgConnection *connection = catalog->find(source.connection);
  if (!connection)
  {
    if (failure)
      *failure = PgFailure::UnknownConnection;
    if (error)
      *error = Error {QStringLiteral("No existe la conexion «%1».").arg(source.connection),
                      QStringLiteral("Disponibles: %1")
                        .arg(catalog->names().isEmpty() ? QStringLiteral("(ninguna)")
                                                        : catalog->names().join(QStringLiteral(", ")))};
    return nullptr;
  }

  // La contrasena recordada o la de libpq (PGPASSWORD/.pgpass): el usuario de
  // la CLI no tiene que escribirla en la linea de comandos.
  const QString password = PgCredentials::instance().passwordFor(*connection);

  PgFailure kind = PgFailure::None;
  GDALDatasetH dataset =
    openWithPassword(*connection, password, source.schema, source.table, &kind, error);
  if (failure)
    *failure = kind;
  return dataset;
}

} // namespace minigis
