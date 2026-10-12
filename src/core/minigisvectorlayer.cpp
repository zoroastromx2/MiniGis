#include "minigisvectorlayer.h"

#include "minigisgdalinit.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

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

std::shared_ptr<VectorLayer> VectorLayer::open(const QString &path, Error *error)
{
  const auto fail = [error](const QString &message, const QString &hint) -> std::shared_ptr<VectorLayer> {
    if (error)
      *error = Error {message, hint};
    return {};
  };

  if (path.trimmed().isEmpty())
  {
    return fail(QStringLiteral("No se ha indicado ninguna capa."),
                QStringLiteral("Arrastra un archivo .shp sobre el lienzo o usa Archivo > Abrir capa (Ctrl+O)."));
  }

  const QFileInfo fileInfo(path);
  if (!fileInfo.exists())
  {
    return fail(QStringLiteral("No existe el archivo «%1».").arg(QDir::toNativeSeparators(path)),
                QStringLiteral("Comprueba la ruta. Un shapefile necesita sus ficheros .shp, .shx y .dbf en la misma carpeta."));
  }

  clearLastGdalError();
  const QByteArray utf8Path = path.toUtf8();
  GDALDatasetH datasetHandle =
    GDALOpenEx(utf8Path.constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);

  if (!datasetHandle)
  {
    QString hint = QStringLiteral("Formatos vectoriales habituales: .shp, .geojson, .gpkg, .kml, .gml. "
                                  "Puedes inspeccionar el archivo con: gdalinfo \"%1\"")
                     .arg(QDir::toNativeSeparators(path));
    const QString gdalMessage = lastGdalError();
    if (!gdalMessage.isEmpty())
      hint = QStringLiteral("GDAL informa: %1\n%2").arg(gdalMessage, hint);

    return fail(QStringLiteral("GDAL no pudo abrir «%1».").arg(QDir::toNativeSeparators(path)), hint);
  }

  OGRLayerH layerHandle = GDALDatasetGetLayer(datasetHandle, 0);
  if (!layerHandle)
  {
    GDALClose(datasetHandle);
    return fail(QStringLiteral("«%1» no contiene ninguna capa vectorial legible.").arg(QDir::toNativeSeparators(path)),
                QStringLiteral("Comprueba el contenido con: ogrinfo -so \"%1\"").arg(QDir::toNativeSeparators(path)));
  }

  std::shared_ptr<VectorLayer> layer(new VectorLayer);
  layer->mDatasetHandle = datasetHandle;
  layer->mLayerHandle = layerHandle;
  layer->mSourcePath = path;

  const char *layerName = OGR_L_GetName(layerHandle);
  layer->mName = QString::fromUtf8(layerName ? layerName : "");

  if (GDALDriverH driver = GDALGetDatasetDriver(datasetHandle))
  {
    const char *driverName = GDALGetDescription(driver);
    layer->mDriverName = QString::fromUtf8(driverName ? driverName : "");
  }

  layer->readMetadata();
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

  OGREnvelope envelope;
  std::memset(&envelope, 0, sizeof(envelope));
  if (OGR_L_GetExtent(mLayerHandle, &envelope, 1) == OGRERR_NONE && envelope.IsInit())
    mExtent = Rectangle(envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY);
  else
    mExtent.setNull();
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

  return name.isEmpty() ? prefix : prefix + QStringLiteral(" - ") + name;
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

  OGRFeatureH feature = OGR_L_GetFeature(mLayerHandle, static_cast<GIntBig>(row));
  if (!feature)
    return {};

  QString value;
  if (OGR_F_IsFieldSetAndNotNull(feature, fieldIndex))
  {
    const char *raw = OGR_F_GetFieldAsString(feature, fieldIndex);
    if (raw)
      value = QString::fromUtf8(raw);
  }

  OGR_F_Destroy(feature);
  return value;
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

QVector<QString> VectorLayer::fieldValues(qint64 row) const{
  QVector<QString> values;
  if (!mLayerHandle)
    return values;

  OGRFeatureH feature = OGR_L_GetFeature(mLayerHandle, static_cast<GIntBig>(row));
  if (!feature)
    return values;

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
  return values;
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
