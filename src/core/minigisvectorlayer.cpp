#include "minigisvectorlayer.h"

#include "minigisconnections.h"
#include "minigisgdalinit.h"
#include "minigisgeometrykinds.h"
#include "minigispgcatalog.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cstring>

#include <cpl_conv.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogr_core.h>
#include <ogr_spatialref.h>

namespace minigis
{
namespace
{

/** Indice de paleta que se asigna a cada capa nueva. */
int nextStyleIndex()
{
  static int counter = 0;
  return counter++;
}

/** Traduce el tipo de geometria de OGR al tipo simplificado de MiniGIS. */
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

/**
 * Texto con el que se nombra una fuente en los mensajes: la ruta nativa para un
 * archivo y la URI tal cual para PostGIS (toNativeSeparators convertiria sus
 * barras en '\').
 */
QString sourceLabel(const QString &path)
{
  return PgSource::looksLike(path) ? path : QDir::toNativeSeparators(path);
}

/**
 * Abre el dataset vectorial de un archivo o de una fuente pg:. Es el unico
 * sitio que decide como se informa de una fuente que no existe o que GDAL no
 * reconoce, para que abrir una capa y enumerar las capas den el mismo mensaje.
 */
GDALDatasetH openVectorDataset(const QString &path, Error *error, PgFailure *failure = nullptr,
                               PgSource *pgSource = nullptr)
{
  const auto fail = [error, failure](const QString &message, const QString &hint) -> GDALDatasetH {
    if (error)
      *error = Error {message, hint};
    if (failure)
      *failure = PgFailure::Other;
    return nullptr;
  };

  if (failure)
    *failure = PgFailure::None;

  if (path.trimmed().isEmpty())
  {
    return fail(QStringLiteral("No se ha indicado ninguna capa."),
                QStringLiteral("Arrastra un archivo .shp sobre el lienzo o usa Archivo > Abrir capa (Ctrl+O)."));
  }

  if (PgSource::looksLike(path))
  {
    PgSource source;
    Error parseError;
    if (!PgSource::parse(path, &source, &parseError))
    {
      if (error)
        *error = parseError;
      if (failure)
        *failure = PgFailure::Other;
      return nullptr;
    }

    if (pgSource)
      *pgSource = source;
    return openPgDataset(source, error, failure);
  }

  const QFileInfo fileInfo(path);
  if (fileInfo.isDir())
  {
    // Una carpeta de .parquet es una coleccion particionada: el driver Parquet
    // de esta build no la lee (necesita arrow[dataset]). Se rechaza aqui, antes
    // de que GDAL la confunda con otra cosa, con una accion concreta.
    const QStringList parquetFiles =
      QDir(path).entryList(QStringList {QStringLiteral("*.parquet"), QStringLiteral("*.geoparquet")},
                           QDir::Files);
    if (!parquetFiles.isEmpty())
    {
      return fail(QStringLiteral("«%1» es una carpeta con archivos .parquet.").arg(QDir::toNativeSeparators(path)),
                  QStringLiteral("Abre uno de los archivos .parquet, no la carpeta: las colecciones "
                                 "particionadas de Parquet no estan soportadas."));
    }
  }

  if (!fileInfo.exists())
  {
    return fail(QStringLiteral("No existe el archivo «%1».").arg(QDir::toNativeSeparators(path)),
                QStringLiteral("Comprueba la ruta. Un shapefile necesita sus ficheros .shp, .shx y .dbf en la misma carpeta."));
  }

  // El sintoma es confuso: el archivo es valido pero GDAL no lo reconoce porque
  // el driver no se compilo. Se detecta antes de abrir para dar la accion exacta
  // en lugar del «no pudo abrir» generico.
  const QString suffix = fileInfo.suffix().toLower();
  const bool parquetPath = suffix == QStringLiteral("parquet") || suffix == QStringLiteral("geoparquet");
  if (parquetPath && GDALGetDriverByName("Parquet") == nullptr)
  {
    return fail(QStringLiteral("Este GDAL no incluye el driver Parquet, asi que no puede abrir «%1».")
                  .arg(QDir::toNativeSeparators(path)),
                QStringLiteral("Recompila GDAL con la feature parquet de vcpkg:\n"
                               "  vcpkg install \"gdal[parquet]\" --triplet x64-windows\n"
                               "y vuelve a configurar y compilar MiniGIS."));
  }

  clearLastGdalError();
  const QByteArray utf8Path = path.toUtf8();
  GDALDatasetH datasetHandle =
    GDALOpenEx(utf8Path.constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);

  if (!datasetHandle)
  {
    QString hint = QStringLiteral("Formatos vectoriales habituales: .shp, .geojson, .gpkg, .kml, .gml, .parquet. "
                                  "Puedes inspeccionar el archivo con: gdalinfo \"%1\"")
                     .arg(QDir::toNativeSeparators(path));
    const QString gdalMessage = lastGdalError();
    if (!gdalMessage.isEmpty())
      hint = QStringLiteral("GDAL informa: %1\n%2").arg(gdalMessage, hint);

    return fail(QStringLiteral("GDAL no pudo abrir «%1».").arg(QDir::toNativeSeparators(path)), hint);
  }

  return datasetHandle;
}

} // namespace

VectorLayer::~VectorLayer()
{
  if (mDatasetHandle)
  {
    GDALClose(mDatasetHandle);
    mDatasetHandle = nullptr;
    mLayerHandle = nullptr;
  }
}

std::shared_ptr<VectorLayer> VectorLayer::open(const QString &path, Error *error,
                                               PgFailure *pgFailure, VectorOpenIssue *issue)
{
  return open(path, 0, error, pgFailure, issue);
}

QStringList VectorLayer::layerNames(const QString &path, Error *error, PgFailure *pgFailure)
{
  const auto fail = [error](const QString &message, const QString &hint) -> QStringList {
    if (error)
      *error = Error {message, hint};
    return {};
  };

  Error openError;
  PgFailure failure = PgFailure::None;
  GDALDatasetH datasetHandle = openVectorDataset(path, &openError, &failure);

  if (!datasetHandle)
  {
    if (error)
      *error = openError;
    if (pgFailure)
      *pgFailure = failure;
    return {};
  }

  const int layerCount = GDALDatasetGetLayerCount(datasetHandle);
  QStringList names;
  names.reserve(layerCount);

  for (int index = 0; index < layerCount; ++index)
  {
    OGRLayerH layerHandle = GDALDatasetGetLayer(datasetHandle, index);
    if (!layerHandle)
      continue;

    const char *layerName = OGR_L_GetName(layerHandle);
    names.append(QString::fromUtf8(layerName ? layerName : ""));
  }

  GDALClose(datasetHandle);

  if (names.isEmpty())
  {
    return fail(QStringLiteral("«%1» no contiene ninguna capa vectorial legible.").arg(sourceLabel(path)),
                QStringLiteral("Comprueba el contenido con: ogrinfo -so \"%1\"").arg(sourceLabel(path)));
  }

  return names;
}

std::shared_ptr<VectorLayer> VectorLayer::open(const QString &path, int layerIndex, Error *error,
                                               PgFailure *pgFailure, VectorOpenIssue *issue)
{
  const auto fail = [error](const QString &message, const QString &hint) -> std::shared_ptr<VectorLayer> {
    if (error)
      *error = Error {message, hint};
    return {};
  };

  if (issue)
    *issue = VectorOpenIssue::None;

  Error openError;
  PgFailure failure = PgFailure::None;
  PgSource pgSource;
  GDALDatasetH datasetHandle = openVectorDataset(path, &openError, &failure, &pgSource);
  if (!datasetHandle)
  {
    if (error)
      *error = openError;
    if (pgFailure)
      *pgFailure = failure;
    return {};
  }

  const int layerCount = GDALDatasetGetLayerCount(datasetHandle);
  if (layerCount <= 0)
  {
    GDALClose(datasetHandle);
    return fail(QStringLiteral("«%1» no contiene ninguna capa vectorial legible.").arg(sourceLabel(path)),
                QStringLiteral("Comprueba el contenido con: ogrinfo -so \"%1\"").arg(sourceLabel(path)));
  }

  if (layerIndex < 0 || layerIndex >= layerCount)
  {
    GDALClose(datasetHandle);
    return fail(QStringLiteral("«%1» no tiene una capa en la posicion %2.")
                  .arg(sourceLabel(path))
                  .arg(layerIndex + 1),
                QStringLiteral("El archivo contiene %1 capa(s); usa un indice entre 1 y %1.")
                  .arg(layerCount));
  }

  OGRLayerH layerHandle = GDALDatasetGetLayer(datasetHandle, layerIndex);
  if (!layerHandle)
  {
    GDALClose(datasetHandle);
    return fail(QStringLiteral("«%1» no contiene ninguna capa vectorial legible.").arg(sourceLabel(path)),
                QStringLiteral("Comprueba el contenido con: ogrinfo -so \"%1\"").arg(sourceLabel(path)));
  }

  QString driverName;
  if (GDALDriverH driver = GDALGetDatasetDriver(datasetHandle))
  {
    const char *rawDriverName = GDALGetDescription(driver);
    driverName = QString::fromUtf8(rawDriverName ? rawDriverName : "");
  }

  // Un .parquet valido puede ser una tabla sin geometria. No es un GeoParquet y
  // abrirlo como capa vacia haria creer que el dibujo fallo; se rechaza aqui,
  // solo para el driver Parquet, sin cambiar el resto de formatos.
  const bool isParquet = driverName.compare(QStringLiteral("Parquet"), Qt::CaseInsensitive) == 0;
  OGRFeatureDefnH definition = OGR_L_GetLayerDefn(layerHandle);
  const bool hasGeometryFields = definition && OGR_FD_GetGeomFieldCount(definition) > 0;
  if (isParquet && OGR_L_GetGeomType(layerHandle) == wkbNone && !hasGeometryFields)
  {
    GDALClose(datasetHandle);
    if (issue)
      *issue = VectorOpenIssue::NoGeometry;
    return fail(QStringLiteral("«%1» es un archivo Parquet, pero no contiene ninguna columna de geometria.")
                  .arg(sourceLabel(path)),
                QStringLiteral("MiniGIS abre GeoParquet, no tablas Parquet. Exporta la capa con geometria "
                               "desde QGIS, DuckDB u ogr2ogr y vuelve a intentarlo."));
  }

  std::shared_ptr<VectorLayer> layer(new VectorLayer);
  layer->mDatasetHandle = datasetHandle;
  layer->mLayerHandle = layerHandle;
  layer->mSourcePath = path;
  layer->mDriverName = driverName;

  if (pgSource.isValid())
  {
    layer->mPgSource = pgSource;
    layer->mContainerName = pgSource.connection + QLatin1Char('/') + pgSource.schema;
    if (const PgConnectionCatalog *catalog = PgConnectionCatalog::shared(nullptr))
    {
      if (const PgConnection *connection = catalog->find(pgSource.connection))
        layer->mPgConnection = *connection;
    }
  }
  else
  {
    layer->mContainerName = QFileInfo(path).fileName();
  }

  layer->mLayerIndex = layerIndex;
  layer->mLayerCount = layerCount;

  const char *layerName = OGR_L_GetName(layerHandle);
  layer->mName = QString::fromUtf8(layerName ? layerName : "");

  layer->readMetadata();
  // La herencia del SRC necesita el SRC ya resuelto y la extension de la capa
  // (para elegir la capa del archivo cuyas coordenadas encajan), asi que va
  // despues de leer los metadatos.
  layer->adoptSiblingSpatialRef(layerCount);
  layer->readFields();
  layer->mStyle = Style::defaultForKind(layer->mKind, nextStyleIndex());

  return layer;
}

void VectorLayer::readMetadata()
{
  const OGRwkbGeometryType geometryType = OGR_L_GetGeomType(mLayerHandle);
  mKind = kindForGeometryType(geometryType);

  const char *typeName = OGRGeometryTypeToName(geometryType);
  mGeometryTypeName = QString::fromUtf8(typeName ? typeName : "desconocido");

  // KML y otros formatos no declaran el tipo de geometria de la capa. Sin esto,
  // el panel y --info dirian "Unknown (any)" aunque la capa este llena de
  // poligonos, y el usuario no sabria que tiene delante.
  if (mKind == GeometryKind::Unknown)
    summarizeGeometryKinds();

  OGREnvelope envelope;
  std::memset(&envelope, 0, sizeof(envelope));
  if (OGR_L_GetExtent(mLayerHandle, &envelope, 1) == OGRERR_NONE && envelope.IsInit())
    mExtent = Rectangle(envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY);
  else
    mExtent.setNull();
}

void VectorLayer::summarizeGeometryKinds()
{
  // Muestra acotada: recorrer la capa entera al abrir costaria tanto como
  // indexarla y este resumen solo alimenta el panel y --info. Los elementos con
  // geometria mixta y los que no traen geometria se reparten solos.
  constexpr int kSampleFeatures = 64;

  GeometryKindCounts counts;
  OGR_L_ResetReading(mLayerHandle);
  int read = 0;
  while (OGRFeatureH feature = OGR_L_GetNextFeature(mLayerHandle))
  {
    countGeometryKinds(OGR_F_GetGeometryRef(feature), counts);
    OGR_F_Destroy(feature);
    if (++read >= kSampleFeatures)
      break;
  }
  OGR_L_ResetReading(mLayerHandle);

  if (counts.drawable() == 0)
    return; // sin geometria dibujable: "desconocido" es la verdad

  const int kinds =
    (counts.points > 0 ? 1 : 0) + (counts.lines > 0 ? 1 : 0) + (counts.polygons > 0 ? 1 : 0);
  if (kinds > 1)
  {
    mKind = GeometryKind::Unknown;
    mGeometryTypeName = QStringLiteral("Mixta");
    return;
  }

  if (counts.points > 0)
  {
    mKind = GeometryKind::Point;
    mGeometryTypeName = QStringLiteral("Punto");
  }
  else if (counts.lines > 0)
  {
    mKind = GeometryKind::Line;
    mGeometryTypeName = QStringLiteral("Linea");
  }
  else
  {
    mKind = GeometryKind::Polygon;
    mGeometryTypeName = QStringLiteral("Poligono");
  }
}

void VectorLayer::readFields()
{
  mFields.clear();

  OGRFeatureDefnH definition = OGR_L_GetLayerDefn(mLayerHandle);
  if (!definition)
    return;

  const int fieldCount = OGR_FD_GetFieldCount(definition);
  mFields.reserve(fieldCount);

  for (int i = 0; i < fieldCount; ++i)
  {
    OGRFieldDefnH field = OGR_FD_GetFieldDefn(definition, i);
    if (!field)
      continue;

    FieldInfo info;
    const char *fieldName = OGR_Fld_GetNameRef(field);
    info.name = QString::fromUtf8(fieldName ? fieldName : "");
    const OGRFieldType fieldType = OGR_Fld_GetType(field);
    const char *typeName = OGR_GetFieldTypeName(fieldType);
    info.typeName = QString::fromUtf8(typeName ? typeName : "");
    info.width = OGR_Fld_GetWidth(field);
    info.isNumeric = (fieldType == OFTInteger || fieldType == OFTInteger64 || fieldType == OFTReal
                      || fieldType == OFTIntegerList || fieldType == OFTInteger64List || fieldType == OFTRealList);
    mFields.append(info);
  }
}

void VectorLayer::adoptSiblingSpatialRef(int layerCount)
{
  if (!mDatasetHandle || !mLayerHandle)
    return;

  // Si la capa declara su SRC no hay nada que suponer.
  if (OGR_L_GetSpatialRef(mLayerHandle) != nullptr)
    return;

  OGRSpatialReference *best = nullptr;
  double bestOverlap = -1.0;

  for (int index = 0; index < layerCount; ++index)
  {
    if (index == mLayerIndex)
      continue;

    OGRLayerH other = GDALDatasetGetLayer(mDatasetHandle, index);
    if (!other)
      continue;

    auto *candidate = reinterpret_cast<OGRSpatialReference *>(OGR_L_GetSpatialRef(other));
    if (!candidate)
      continue;

    // Se prefiere la capa cuyas coordenadas se solapan con esta: en un archivo
    // con capas en SRC distintos, la primera que aparezca puede ser la equivocada.
    OGREnvelope envelope;
    std::memset(&envelope, 0, sizeof(envelope));
    double overlap = 0.0;
    if (OGR_L_GetExtent(other, &envelope, 1) == OGRERR_NONE && envelope.IsInit() && mExtent.isValid())
    {
      const double xMin = std::max(mExtent.xMin(), envelope.MinX);
      const double yMin = std::max(mExtent.yMin(), envelope.MinY);
      const double xMax = std::min(mExtent.xMax(), envelope.MaxX);
      const double yMax = std::min(mExtent.yMax(), envelope.MaxY);
      if (xMax > xMin && yMax > yMin)
        overlap = (xMax - xMin) * (yMax - yMin);
    }

    if (overlap > bestOverlap)
    {
      bestOverlap = overlap;
      best = candidate;
    }
  }

  if (best)
    mAssumedSrs.reset(best->Clone());
}

qint64 VectorLayer::featureCount() const
{
  if (mFeatureCount >= 0)
    return mFeatureCount;

  if (!mLayerHandle)
  {
    mFeatureCount = 0;
    return mFeatureCount;
  }

  GIntBig count = OGR_L_GetFeatureCount(mLayerHandle, 1);
  if (count < 0)
  {
    // Hay drivers que no pueden contar sin recorrer la capa.
    count = 0;
    OGR_L_ResetReading(mLayerHandle);
    while (OGRFeatureH feature = OGR_L_GetNextFeature(mLayerHandle))
    {
      ++count;
      OGR_F_Destroy(feature);
    }
  }

  mFeatureCount = static_cast<qint64>(count);
  return mFeatureCount;
}

const OGRSpatialReference *VectorLayer::spatialRef() const
{
  // Si la capa no declara SRC se usa el heredado de otra capa del archivo (ver
  // adoptSiblingSpatialRef); sin el, sus coordenadas no se pueden reproyectar.
  if (mAssumedSrs)
    return mAssumedSrs.get();

  if (!mLayerHandle)
    return nullptr;

  OGRSpatialReferenceH handle = OGR_L_GetSpatialRef(mLayerHandle);
  return reinterpret_cast<const OGRSpatialReference *>(handle);
}

int VectorLayer::crsEpsg() const
{
  const OGRSpatialReference *srs = spatialRef();
  if (!srs)
    return 0;

  const char *code = srs->GetAuthorityCode(nullptr);
  if (code && *code)
    return QString::fromUtf8(code).toInt();

  // Muchos .prj no declaran la autoridad: se intenta deducir del WKT.
  std::unique_ptr<OGRSpatialReference> clone(srs->Clone());
  if (!clone)
    return 0;

  clone->AutoIdentifyEPSG();
  code = clone->GetAuthorityCode(nullptr);
  return (code && *code) ? QString::fromUtf8(code).toInt() : 0;
}

QString VectorLayer::crsDescription() const
{
  const OGRSpatialReference *srs = spatialRef();
  if (!srs)
    return QStringLiteral("SRC desconocido (el archivo no incluye definicion de proyeccion)");

  const char *rawName = srs->GetName();
  const QString name = rawName ? QString::fromUtf8(rawName) : QString();

  const int epsg = crsEpsg();
  QString prefix;
  if (epsg > 0)
    prefix = QStringLiteral("EPSG:%1").arg(epsg);
  else
    prefix = srs->IsProjected() ? QStringLiteral("SRC proyectado sin codigo EPSG")
                                : QStringLiteral("SRC geografico sin codigo EPSG");

  const QString base = name.isEmpty() ? prefix : prefix + QStringLiteral(" - ") + name;

  // Se advierte de que el SRC es una suposicion: el usuario debe saberlo si las
  // capas no se colocan donde espera.
  if (mAssumedSrs)
    return base + QStringLiteral(" (la capa no declara SRC; se usa el de otra capa del archivo)");

  return base;
}

QString VectorLayer::crsWkt() const
{
  const OGRSpatialReference *srs = spatialRef();
  if (!srs)
    return {};

  char *wkt = nullptr;
  if (srs->exportToWkt(&wkt) != OGRERR_NONE || !wkt)
    return {};

  const QString result = QString::fromUtf8(wkt);
  CPLFree(wkt);
  return result;
}

QString VectorLayer::fieldValue(qint64 row, int fieldIndex) const
{
  if (!mLayerHandle || fieldIndex < 0 || fieldIndex >= mFields.size())
    return {};

  // Se pasa por fieldValues() para aprovechar la cache de filas: la tabla pide
  // los campos de una fila seguidos y no tiene sentido releer el elemento por
  // cada uno de los 166 campos de un KML.
  const QVector<QString> values = fieldValues(row);
  if (fieldIndex >= values.size())
    return {};
  return values.at(fieldIndex);
}

Rectangle VectorLayer::featureEnvelope(qint64 row) const
{
  Rectangle result;
  if (!mLayerHandle)
    return result;

  OGRFeatureH feature = OGR_L_GetFeature(mLayerHandle, static_cast<GIntBig>(row));
  if (!feature)
    return result;

  if (OGRGeometryH geometry = OGR_F_GetGeometryRef(feature))
  {
    OGREnvelope envelope;
    std::memset(&envelope, 0, sizeof(envelope));
    OGR_G_GetEnvelope(geometry, &envelope);
    if (envelope.IsInit())
      result = Rectangle(envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY);
  }

  OGR_F_Destroy(feature);
  return result;
}

QVector<QString> VectorLayer::fieldValues(qint64 row) const
{
  QVector<QString> values;
  if (!mLayerHandle)
    return values;

  if (const QVector<QString> *cached = mAttributeCache.object(row))
    return *cached;

  const qint64 fid = fidForRow(row);
  if (fid < 0)
    return values;

  OGRFeatureH feature = OGR_L_GetFeature(mLayerHandle, static_cast<GIntBig>(fid));
  if (!feature)
    return values;

  ++mAttributeReads;

  values.reserve(mFields.size());
  for (int i = 0; i < mFields.size(); ++i)
  {
    QString value;
    if (OGR_F_IsFieldSetAndNotNull(feature, i))
    {
      const char *raw = OGR_F_GetFieldAsString(feature, i);
      if (raw)
        value = QString::fromUtf8(raw);
    }
    values.append(value);
  }

  OGR_F_Destroy(feature);

  // La cache es FIFO por coste: cada fila cuenta uno y al pasar de 200 se
  // descarta la mas antigua. Con 166 campos son unos pocos MB como maximo.
  mAttributeCache.insert(row, new QVector<QString>(values), 1);
  return values;
}

QVector<QVector<QString>> VectorLayer::fieldValuesPage(qint64 firstRow, int count) const
{
  QVector<QVector<QString>> rows;
  if (!mLayerHandle || firstRow < 0 || count <= 0)
    return rows;

  const qint64 total = featureCount();
  if (firstRow >= total)
    return rows;

  const qint64 wanted = std::min<qint64>(count, total - firstRow);

  // Posicionarse por indice es lo barato; si el driver no lo soporta se recorre
  // desde el principio, que sigue siendo una sola pasada secuencial.
  if (OGR_L_SetNextByIndex(mLayerHandle, static_cast<GIntBig>(firstRow)) != OGRERR_NONE)
  {
    OGR_L_ResetReading(mLayerHandle);
    for (qint64 skipped = 0; skipped < firstRow; ++skipped)
    {
      OGRFeatureH skip = OGR_L_GetNextFeature(mLayerHandle);
      if (!skip)
        return rows;
      OGR_F_Destroy(skip);
    }
  }

  rows.reserve(static_cast<int>(wanted));
  for (qint64 i = 0; i < wanted; ++i)
  {
    OGRFeatureH feature = OGR_L_GetNextFeature(mLayerHandle);
    if (!feature)
      break;

    ++mAttributeReads;

    QVector<QString> values;
    values.reserve(mFields.size());
    for (int field = 0; field < mFields.size(); ++field)
    {
      QString value;
      if (OGR_F_IsFieldSetAndNotNull(feature, field))
      {
        const char *raw = OGR_F_GetFieldAsString(feature, field);
        if (raw)
          value = QString::fromUtf8(raw);
      }
      values.append(value);
    }

    OGR_F_Destroy(feature);
    rows.append(values);
  }

  return rows;
}

void VectorLayer::ensureRowToFid() const
{
  if (mRowMapReady)
    return;
  mRowMapReady = true;
  mRowsAreFids = true;

  const qint64 total = featureCount();
  if (!mLayerHandle || total <= 0)
    return;

  // Camino rapido: si el FID coincide con la fila (shapefile, GeoPackage,
  // GeoJSON) no hay que recorrer nada. Se comprueba con la primera y la ultima.
  OGRFeatureH first = OGR_L_GetFeature(mLayerHandle, 0);
  OGRFeatureH last = OGR_L_GetFeature(mLayerHandle, static_cast<GIntBig>(total - 1));
  const bool contiguous = (first != nullptr) && (last != nullptr);
  if (first)
    OGR_F_Destroy(first);
  if (last)
    OGR_F_Destroy(last);
  if (contiguous)
    return;

  // KML y otros formatos: los FID no empiezan en 0 ni son contiguos. El mapa se
  // construye con una sola pasada secuencial, que es la unica forma de ir de
  // fila a FID; sin el, la primera fila salia vacia y faltaba el ultimo
  // elemento.
  mRowsAreFids = false;
  mRowToFid.clear();
  mRowToFid.reserve(static_cast<size_t>(total));
  OGR_L_ResetReading(mLayerHandle);
  while (OGRFeatureH feature = OGR_L_GetNextFeature(mLayerHandle))
  {
    mRowToFid.push_back(static_cast<qint64>(OGR_F_GetFID(feature)));
    OGR_F_Destroy(feature);
  }
  OGR_L_ResetReading(mLayerHandle);
}

qint64 VectorLayer::fidForRow(qint64 row) const
{
  if (!mLayerHandle || row < 0)
    return -1;

  ensureRowToFid();
  if (mRowsAreFids)
    return row;
  if (row >= static_cast<qint64>(mRowToFid.size()))
    return -1;
  return mRowToFid[static_cast<size_t>(row)];
}

qint64 VectorLayer::rowForFid(qint64 fid) const
{
  if (!mLayerHandle || fid < 0)
    return -1;

  ensureRowToFid();
  if (mRowsAreFids)
    return fid;

  for (size_t index = 0; index < mRowToFid.size(); ++index)
  {
    if (mRowToFid[index] == fid)
      return static_cast<qint64>(index);
  }
  return -1;
}

Rectangle VectorLayer::extentInEpsg(int destinationEpsg) const
{
  if (!mExtent.isValid() || destinationEpsg <= 0)
    return mExtent;

  const OGRSpatialReference *source = spatialRef();
  if (!source)
    return mExtent;

  OGRSpatialReference destination;
  if (destination.importFromEPSG(destinationEpsg) != OGRERR_NONE)
    return mExtent;
  destination.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  std::unique_ptr<OGRSpatialReference> sourceClone(source->Clone());
  if (!sourceClone)
    return mExtent;
  sourceClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  if (sourceClone->IsSame(&destination))
    return mExtent;

  std::unique_ptr<OGRCoordinateTransformation> transformation(
    OGRCreateCoordinateTransformation(sourceClone.get(), &destination));
  if (!transformation)
    return mExtent;

  // Con las cuatro esquinas y el centro se obtiene una envolvente suficiente
  // para encuadrar la capa (las reproyecciones no son afines, por eso el centro).
  double x[5] = {mExtent.xMin(), mExtent.xMax(), mExtent.xMin(), mExtent.xMax(), mExtent.centerX()};
  double y[5] = {mExtent.yMin(), mExtent.yMin(), mExtent.yMax(), mExtent.yMax(), mExtent.centerY()};

  if (!transformation->Transform(5, x, y))
    return mExtent;

  Rectangle result;
  for (int i = 0; i < 5; ++i)
    result.extend(x[i], y[i]);

  return result;
}

QString VectorLayer::displayName() const
{
  return QStringLiteral("%1 (%2)").arg(mName).arg(featureCount());
}

} // namespace minigis
