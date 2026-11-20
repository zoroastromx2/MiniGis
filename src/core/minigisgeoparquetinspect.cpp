#include "minigisgeoparquetinspect.h"

#include "minigisgeometrykinds.h"
#include "minigisgeometrystore.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_spatialref.h>

namespace minigis
{
namespace
{

/** Cuenta los elementos y las partes de una capa leyendola con la API C de OGR. */
void countLayer(VectorLayer &layer, GeoParquetLayerReport *report)
{
  OGRLayerH handle = layer.ogrLayerHandle();
  if (!handle)
    return;

  OGR_L_ResetReading(handle);
  while (OGRFeatureH feature = OGR_L_GetNextFeature(handle))
  {
    const OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    if (!geometry)
    {
      ++report->withoutGeometry;
    }
    else
    {
      GeometryKindCounts counts;
      countGeometryKinds(geometry, counts);
      report->points += counts.points;
      report->lines += counts.lines;
      report->polygons += counts.polygons;
      report->unsupportedParts += counts.unsupported;
    }

    ++report->features;
    OGR_F_Destroy(feature);
  }
}

QJsonArray extentArray(const Rectangle &extent)
{
  QJsonArray array;
  if (!extent.isValid())
    return array;

  array.append(extent.xMin());
  array.append(extent.yMin());
  array.append(extent.xMax());
  array.append(extent.yMax());
  return array;
}

QJsonObject layerJson(const GeoParquetLayerReport &layer)
{
  QJsonArray fields;
  for (const VectorLayer::FieldInfo &field : layer.fields)
  {
    QJsonObject fieldJson;
    fieldJson.insert(QStringLiteral("name"), field.name);
    fieldJson.insert(QStringLiteral("type"), field.typeName);
    fields.append(fieldJson);
  }

  QJsonObject json;
  json.insert(QStringLiteral("name"), layer.name);
  json.insert(QStringLiteral("geometry"), layer.geometryName());
  json.insert(QStringLiteral("features"), layer.features);
  json.insert(QStringLiteral("withoutGeometry"), layer.withoutGeometry);
  json.insert(QStringLiteral("unsupportedParts"), layer.unsupportedParts);
  json.insert(QStringLiteral("points"), layer.points);
  json.insert(QStringLiteral("lines"), layer.lines);
  json.insert(QStringLiteral("polygons"), layer.polygons);
  json.insert(QStringLiteral("storeFeatures"), layer.storeFeatures);
  json.insert(QStringLiteral("missingInStore"), layer.missingInStore());
  json.insert(QStringLiteral("crsEpsg"), layer.crsEpsg);
  json.insert(QStringLiteral("crsName"), layer.crsName);
  json.insert(QStringLiteral("crsDescription"), layer.crsDescription);
  json.insert(QStringLiteral("extent"), extentArray(layer.extent));
  json.insert(QStringLiteral("extent4326"), extentArray(layer.extent4326));
  json.insert(QStringLiteral("fields"), fields);
  json.insert(QStringLiteral("fidColumn"), layer.fidColumn);
  json.insert(QStringLiteral("attributeCache"), layer.attributeCache);
  return json;
}

} // namespace

QString GeoParquetLayerReport::geometryName() const
{
  const int kinds = (points > 0 ? 1 : 0) + (lines > 0 ? 1 : 0) + (polygons > 0 ? 1 : 0);
  if (kinds == 0)
    return QStringLiteral("Sin geometria");
  if (kinds > 1)
    return QStringLiteral("Mixta");
  if (points > 0)
    return QStringLiteral("Punto");
  if (lines > 0)
    return QStringLiteral("Linea");
  return QStringLiteral("Poligono");
}

bool GeoParquetLayerReport::missingInStore() const
{
  return storeFeatures >= 0 && storeFeatures < drawableFeatures();
}

bool GeoParquetReport::ok() const
{
  if (layers.isEmpty())
    return false;

  for (const GeoParquetLayerReport &layer : layers)
  {
    if (layer.unsupportedParts > 0 || layer.missingInStore() || !layer.attributeCache)
      return false;
  }
  return true;
}

QJsonObject GeoParquetReport::toJson() const
{
  QJsonArray jsonLayers;
  for (const GeoParquetLayerReport &layer : layers)
    jsonLayers.append(layerJson(layer));

  QJsonObject root;
  root.insert(QStringLiteral("path"), path);
  root.insert(QStringLiteral("driver"), driver);
  root.insert(QStringLiteral("layers"), jsonLayers);
  root.insert(QStringLiteral("ok"), ok());
  return root;
}

QString GeoParquetReport::toText() const
{
  QStringList lines;
  lines << QStringLiteral("Archivo : %1").arg(QDir::toNativeSeparators(path));
  lines << QStringLiteral("Driver  : %1").arg(driver);

  for (const GeoParquetLayerReport &layer : layers)
  {
    lines << QStringLiteral("Capa    : %1").arg(layer.name);
    lines << QStringLiteral("  Elementos : %1 (%2 punto(s), %3 linea(s), %4 poligono(s)), "
                            "sin geometria %5, partes no soportadas %6")
               .arg(QString::number(layer.features), QString::number(layer.points),
                    QString::number(layer.lines), QString::number(layer.polygons),
                    QString::number(layer.withoutGeometry), QString::number(layer.unsupportedParts));
    lines << QStringLiteral("  Tipo      : %1").arg(layer.geometryName());
    const QString crsText = layer.crsDescription.isEmpty()
                              ? QStringLiteral("desconocido")
                              : layer.crsDescription;
    if (layer.crsEpsg > 0)
      lines << QStringLiteral("  SRC       : %1 (EPSG:%2)").arg(crsText).arg(layer.crsEpsg);
    else
      lines << QStringLiteral("  SRC       : %1").arg(crsText);

    if (layer.storeFeatures >= 0)
    {
      lines << QStringLiteral("  Almacen   : %1 de %2 elementos con geometria")
                 .arg(QString::number(layer.storeFeatures), QString::number(layer.drawableFeatures()));
    }
    else
    {
      lines << QStringLiteral("  Almacen   : no construido (presupuesto de vertices)");
    }

    if (!layer.fidColumn.isEmpty())
      lines << QStringLiteral("  FID       : columna «%1»").arg(layer.fidColumn);
  }

  if (layers.isEmpty())
    lines << QStringLiteral("Capa    : (el archivo no contiene capas vectoriales)");

  lines << QStringLiteral("Resultado  : %1")
             .arg(ok() ? QStringLiteral("OK") : QStringLiteral("FALLO"));
  return lines.join(QLatin1Char('\n'));
}

bool inspectGeoParquet(const QString &path, GeoParquetReport *report, Error *error,
                       VectorOpenIssue *issue)
{
  if (!report)
    return false;

  *report = GeoParquetReport();
  report->path = path;
  if (issue)
    *issue = VectorOpenIssue::None;

  Error listError;
  const QStringList names = VectorLayer::layerNames(path, &listError);
  if (names.isEmpty())
  {
    // Distinguir «no es un archivo vectorial» (codigo 3) de «abre pero no tiene
    // capas» (codigo 5): la CLI necesita saber cual de los dos es.
    const QByteArray utf8Path = path.toUtf8();
    GDALDatasetH probe =
      GDALOpenEx(utf8Path.constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!probe)
    {
      if (error)
      {
        *error = listError.isEmpty()
                   ? Error {QStringLiteral("No se pudo abrir «%1» como archivo vectorial.")
                              .arg(QDir::toNativeSeparators(path)),
                            QStringLiteral("Comprueba el contenido con: gdalinfo \"%1\"")
                              .arg(QDir::toNativeSeparators(path))}
                   : listError;
      }
      return false;
    }

    GDALClose(probe);
    return true;
  }

  for (int index = 0; index < names.size(); ++index)
  {
    Error layerError;
    VectorOpenIssue layerIssue = VectorOpenIssue::None;
    const std::shared_ptr<VectorLayer> layer =
      VectorLayer::open(path, index, &layerError, nullptr, &layerIssue);
    if (!layer)
    {
      if (error)
        *error = layerError;
      if (issue)
        *issue = layerIssue;
      return false;
    }

    if (index == 0)
      report->driver = layer->driverName();

    GeoParquetLayerReport layerReport;
    layerReport.name = layer->name();
    layerReport.crsEpsg = layer->crsEpsg();
    layerReport.crsDescription = layer->crsDescription();
    layerReport.extent = layer->extent();
    layerReport.extent4326 = layer->extentInEpsg(4326);
    layerReport.fields = layer->fields();

    if (const OGRSpatialReference *srs = layer->spatialRef())
    {
      const char *name = srs->GetName();
      if (name)
        layerReport.crsName = QString::fromUtf8(name);
    }

    if (OGRLayerH handle = layer->ogrLayerHandle())
    {
      if (const char *fidColumn = OGR_L_GetFIDColumn(handle))
        layerReport.fidColumn = QString::fromUtf8(fidColumn);
    }

    // La tabla de atributos vuelve a pedir las mismas filas en cada repintado:
    // la segunda lectura debe salir de la cache. Se comprueba aqui, sin ventana.
    if (layer->featureCount() > 0 && !layer->fields().isEmpty())
    {
      const QVector<QString> first = layer->fieldValues(0);
      const quint64 readsBefore = layer->attributeReads();
      const QVector<QString> again = layer->fieldValues(0);
      layerReport.attributeCache =
        !first.isEmpty() && again == first && layer->attributeReads() == readsBefore;
    }

    countLayer(*layer, &layerReport);

    // La otra orilla: cuantos de esos elementos conserva el almacen que usa el
    // renderizador. Se construye en el SRC del lienzo para ejercitar tambien la
    // reproyeccion (el LCC de los archivos del usuario).
    QString storeError;
    const std::shared_ptr<GeometryStore> store =
      GeometryStore::buildFromLayer(*layer, 4326, GeometryStore::Options(), {}, &storeError);
    layerReport.storeFeatures = store ? store->stats().features : -1;

    report->layers.append(layerReport);
  }

  return true;
}

} // namespace minigis
