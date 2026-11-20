#include "minigiskmlinspect.h"

#include "minigisgeometrykinds.h"

#include "minigisgeometrystore.h"
#include "minigisvectorlayer.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

#include <algorithm>

#include <gdal.h>
#include <ogr_api.h>

namespace minigis
{
namespace
{

/** Cuenta los elementos de una capa leyendolos con la API C de OGR. */
void countLayer(VectorLayer &layer, KmlLayerReport *report)
{
  OGRLayerH handle = layer.ogrLayerHandle();
  if (!handle)
    return;

  // Estilo de cada elemento, contado por texto: el dominante es el que mas se
  // repite y es el que se aplicaria a toda la capa si no hay tope.
  QMap<QString, int> styles;

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

    if (const char *style = OGR_F_GetStyleString(feature); style && *style)
      ++styles[QString::fromUtf8(style)];

    ++report->features;
    OGR_F_Destroy(feature);
  }

  report->distinctStyles = styles.size();
  int bestCount = 0;
  for (auto it = styles.cbegin(); it != styles.cend(); ++it)
  {
    if (it.value() > bestCount)
    {
      bestCount = it.value();
      report->styleString = it.key();
    }
  }
}

QJsonObject layerJson(const KmlLayerReport &layer)
{
  QJsonObject kinds;
  kinds.insert(QStringLiteral("point"), layer.points);
  kinds.insert(QStringLiteral("line"), layer.lines);
  kinds.insert(QStringLiteral("polygon"), layer.polygons);

  QJsonArray extent;
  if (layer.extent.isValid())
  {
    extent.append(layer.extent.xMin());
    extent.append(layer.extent.yMin());
    extent.append(layer.extent.xMax());
    extent.append(layer.extent.yMax());
  }

  QJsonObject json;
  json.insert(QStringLiteral("name"), layer.name);
  json.insert(QStringLiteral("features"), layer.features);
  json.insert(QStringLiteral("geometry"), layer.geometryName());
  json.insert(QStringLiteral("kinds"), kinds);
  json.insert(QStringLiteral("withoutGeometry"), layer.withoutGeometry);
  json.insert(QStringLiteral("unsupportedParts"), layer.unsupportedParts);
  json.insert(QStringLiteral("storeFeatures"), layer.storeFeatures);
  json.insert(QStringLiteral("distinctStyles"), layer.distinctStyles);
  json.insert(QStringLiteral("attributeCache"), layer.attributeCache);
  json.insert(QStringLiteral("extent"), extent);
  json.insert(QStringLiteral("crsEpsg"), layer.crsEpsg);
  return json;
}

/** Simbologia aplicada, para que se pueda comprobar sin abrir la ventana. */
QJsonObject styleJson(const Style &style)
{
  QJsonObject json;
  json.insert(QStringLiteral("strokeColor"), style.strokeColor.name(QColor::HexRgb));
  json.insert(QStringLiteral("strokeWidth"), style.strokeWidth);
  json.insert(QStringLiteral("fillEnabled"), style.fillEnabled);
  json.insert(QStringLiteral("fillColor"), style.fillColor.name(QColor::HexRgb));
  json.insert(QStringLiteral("fillOpacity"), style.opacity);
  return json;
}

} // namespace

QString KmlLayerReport::geometryName() const
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

bool KmlLayerReport::missingInStore() const
{
  return storeFeatures >= 0 && storeFeatures < drawableFeatures();
}

bool KmlReport::ok() const
{
  if (layers.isEmpty())
    return false;

  for (const KmlLayerReport &layer : layers)
  {
    if (layer.unsupportedParts > 0 || layer.missingInStore() || !layer.attributeCache)
      return false;
  }
  return true;
}

QJsonObject KmlReport::toJson() const
{
  QJsonArray jsonLayers;
  for (const KmlLayerReport &layer : layers)
    jsonLayers.append(layerJson(layer));

  QJsonObject root;
  root.insert(QStringLiteral("ok"), ok());
  root.insert(QStringLiteral("driver"), driver);
  root.insert(QStringLiteral("layers"), jsonLayers);
  root.insert(QStringLiteral("declaredStyles"), declaredStyles);
  root.insert(QStringLiteral("appliedStyles"), appliedStyles);
  root.insert(QStringLiteral("styleMode"), styleMode);
  if (appliedStyles > 0)
    root.insert(QStringLiteral("style"), styleJson(style));
  return root;
}

QString KmlReport::toText() const
{
  QStringList lines;
  lines << QStringLiteral("Archivo : %1").arg(QDir::toNativeSeparators(path));
  lines << QStringLiteral("Driver  : %1").arg(driver);

  for (const KmlLayerReport &layer : layers)
  {
    lines << QStringLiteral("Capa    : %1").arg(layer.name);
    lines << QStringLiteral("  Elementos : %1 (%2 punto(s), %3 linea(s), %4 poligono(s)), "
                            "sin geometria %5, partes no soportadas %6")
               .arg(QString::number(layer.features), QString::number(layer.points),
                    QString::number(layer.lines), QString::number(layer.polygons),
                    QString::number(layer.withoutGeometry), QString::number(layer.unsupportedParts));
    lines << QStringLiteral("  Tipo      : %1").arg(layer.geometryName());

    if (layer.storeFeatures >= 0)
    {
      lines << QStringLiteral("  Almacen   : %1 de %2 elementos con geometria")
                 .arg(QString::number(layer.storeFeatures), QString::number(layer.drawableFeatures()));
    }
    else
    {
      lines << QStringLiteral("  Almacen   : no construido (presupuesto de vertices)");
    }

    if (layer.distinctStyles > 0)
    {
      lines << QStringLiteral("  Estilos   : %1 distinto(s), %2 aplicado(s), dominante «%3»")
                   .arg(QString::number(layer.distinctStyles), QString::number(layer.appliedStyles),
                        layer.styleString);
    }
  }

  if (layers.isEmpty())
    lines << QStringLiteral("Capa    : (el archivo no contiene capas vectoriales)");

  lines << QStringLiteral("Resultado  : %1")
           .arg(ok() ? QStringLiteral("OK") : QStringLiteral("FALLO"));
  return lines.join(QLatin1Char('\n'));
}

bool inspectKml(const QString &path, KmlReport *report, Error *error)
{
  if (!report)
    return false;

  *report = KmlReport();
  report->path = path;

  Error listError;
  const QStringList names = VectorLayer::layerNames(path, &listError);
  if (names.isEmpty())
  {
    // Distinguir "no es un archivo vectorial" (codigo 3) de "abre pero no tiene
    // capas" (codigo 5): la CLI necesita saber cual de los dos es.
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
    const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, index, &layerError);
    if (!layer)
    {
      if (error)
        *error = layerError;
      return false;
    }

    if (index == 0)
      report->driver = layer->driverName();

    KmlLayerReport layerReport;
    layerReport.name = layer->name();
    layerReport.crsEpsg = layer->crsEpsg();
    layerReport.extent = layer->extent();

    // La tabla de atributos vuelve a pedir las mismas filas en cada repintado:
    // una lectura cuesta milisegundos en KML, asi que la segunda debe salir de
    // la cache. Se comprueba aqui, sin ventana, con dos lecturas de la fila 0.
    if (layer->featureCount() > 0)
    {
      layer->fieldValues(0);
      const quint64 readsBefore = layer->attributeReads();
      const QVector<QString> again = layer->fieldValues(0);
      layerReport.attributeCache = !again.isEmpty() && layer->attributeReads() == readsBefore;
    }

    countLayer(*layer, &layerReport);

    // La otra orilla: cuantos de esos elementos conserva el almacen que usa el
    // renderizador. Sin esta comparacion, un elemento descartado es invisible.
    QString storeError;
    const std::shared_ptr<GeometryStore> store =
      GeometryStore::buildFromLayer(*layer, 0, GeometryStore::Options(), {}, &storeError);
    layerReport.storeFeatures = store ? store->stats().features : -1;
    layerReport.appliedStyles = store ? static_cast<int>(store->styles().size()) : 0;
    if (store && !store->styles().empty())
      report->style = store->styles().front();

    // La tabla de atributos vuelve a pedir las mismas filas en cada repintado:
    // una lectura cuesta milisegundos en KML, asi que la segunda debe salir de
    // la cache. Se comprueba aqui, sin ventana, con dos lecturas de la fila 0.
    report->layers.append(layerReport);
  }

  // Resumen de simbologia: que se ha aplicado de verdad y como. "dominant"
  // significa que el archivo declaraba mas estilos que el tope y se ha usado el
  // mayoritario; nunca se aplica de menos en silencio.
  for (const KmlLayerReport &layer : report->layers)
  {
    report->declaredStyles += layer.distinctStyles;
    report->appliedStyles = std::max(report->appliedStyles, layer.appliedStyles);
  }

  if (report->appliedStyles == 0)
    report->styleMode = QStringLiteral("none");
  else if (report->appliedStyles > 1)
    report->styleMode = QStringLiteral("perFeature");
  else if (report->declaredStyles > report->appliedStyles)
    report->styleMode = QStringLiteral("dominant");
  else
    report->styleMode = QStringLiteral("single");

  return true;
}

} // namespace minigis
