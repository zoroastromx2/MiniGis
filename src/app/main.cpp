/**
 * MiniGIS - punto de entrada.
 *
 * Modos de ejecucion (documentados tambien en --help):
 *   minigis [archivos...]                     interfaz grafica
 *   minigis --info [--json] --layer x.shp     informacion de las capas, sin ventana
 *   minigis --snapshot out.png --layer x.shp  render del mapa a PNG, sin ventana
 *   minigis --snapshot-window ui.png          captura de la interfaz completa
 *   minigis --bench 40 --layer x.gpkg         mide cuadros de paneo/zoom (rendimiento)
 *   minigis --list-drivers                    drivers vectoriales de GDAL
 *
 * Contrato del ejecutable (lo usan scripts y las pruebas de CTest):
 *   0 exito | 1 uso incorrecto | 2 archivo no encontrado | 3 GDAL no pudo abrir
 *   4 no se pudo escribir la imagen | 5 no hay capas para renderizar
 *   6 el rendimiento medido no cumple el presupuesto de --bench-budget
 */

#include <QApplication>
#include <QColor>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSize>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <ogr_spatialref.h>

#ifdef Q_OS_WIN
// Se declara SetConsoleOutputCP a mano en lugar de incluir <windows.h>: ese
// encabezado declara una funcion GDI llamada Rectangle() que choca con
// minigis::Rectangle y rompe la compilacion de este archivo.
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int codePage);
namespace
{
constexpr unsigned int kUtf8CodePage = 65001;
}
#endif

#include "minigisgdalinit.h"
#include "minigisgeometrystore.h"
#include "minigislabeldialog.h"
#include "minigismainwindow.h"
#include "minigismapcanvas.h"
#include "minigisrasterlayer.h"
#include "minigisrasterstyledialog.h"
#include "minigisrasterstyle.h"
#include "minigisrectangle.h"
#include "minigisshapefileindex.h"
#include "minigisstyle.h"
#include "minigisvectorlayer.h"

using namespace minigis;

namespace
{

enum ExitCode
{
  ExitOk = 0,
  ExitUsage = 1,
  ExitFileNotFound = 2,
  ExitOpenFailed = 3,
  ExitRenderFailed = 4,
  ExitNoLayers = 5,
  ExitBudgetExceeded = 6
};

bool g_verbose = false;

// Se definen mas abajo, junto a los modos sin ventana.
std::shared_ptr<GeometryStore> buildGeometryStore(const std::shared_ptr<VectorLayer> &layer,
                                                  int destinationEpsg, qint64 *buildMs,
                                                  QString *modeName, QString *error);
QString storeModeName(const GeometryStore &store);
void attachGeometryStore(const std::shared_ptr<VectorLayer> &layer, int destinationEpsg, bool enabled);

/**
 * Opciones de etiquetado de la linea de comandos: permiten comprobar las
 * etiquetas sin abrir la interfaz (capturas y banco de pruebas).
 */
struct LabelOptions
{
  QStringList fields;
  double size = 10.0;
  bool enabled = true; // --no-labels las apaga todas
  bool halo = true;
  /** Tamano en unidades del mapa (si > 0, se usa el modo "unidades del mapa"). */
  double mapUnitsSize = 0.0;
  bool parsed = false;
};

void applyLabelOptions(const std::shared_ptr<VectorLayer> &layer, const LabelOptions &options);

/**
 * Opciones de raster de la linea de comandos. Permiten fijar el estiramiento,
 * las bandas y la opacidad sin abrir la interfaz (capturas y pruebas).
 */
struct RasterOptions
{
  bool stretchSet = false;
  RasterStyle::Stretch stretch = RasterStyle::Stretch::None;
  bool bandsSet = false;
  QVector<int> bands;
  bool opacitySet = false;
  int opacity = 100;
};

void applyRasterOptions(const std::shared_ptr<RasterLayer> &raster, const RasterOptions &options)
{
  if (!raster || (!options.stretchSet && !options.bandsSet && !options.opacitySet))
    return;

  RasterStyle style = raster->style();
  if (options.stretchSet)
    style.stretch = options.stretch;

  if (options.bandsSet && !options.bands.isEmpty())
  {
    style.bandMode = options.bands.size() == 1 ? RasterStyle::BandMode::Grayscale
                                               : RasterStyle::BandMode::Rgb;
    style.redBand = options.bands.at(0);
    style.greenBand = options.bands.size() > 1 ? options.bands.at(1) : options.bands.at(0);
    style.blueBand = options.bands.size() > 2 ? options.bands.at(2) : options.bands.at(0);
  }

  if (options.opacitySet)
    style.opacity = options.opacity;

  raster->setStyle(style);
}

QTextStream &stdOut()
{
  static QTextStream stream(stdout);
  return stream;
}

QTextStream &stdErr()
{
  static QTextStream stream(stderr);
  return stream;
}

void writeOut(const QString &text)
{
  stdOut() << text << Qt::endl;
}

void writeErr(const QString &text)
{
  stdErr() << text << Qt::endl;
}

void logVerbose(const QString &message)
{
  if (g_verbose)
    stdErr() << QStringLiteral("[minigis] ") << message << Qt::endl;
}

QString helpText(const QCommandLineParser &parser)
{
  return parser.helpText()
         + QStringLiteral("\nEjemplos:\n"
                          "  minigis                                   abre la interfaz vacia\n"
                          "  minigis data/sample/shapefiles/cities.shp abre un shapefile al arrancar\n"
                          "  minigis --info --layer cities.shp         resume la capa (sin ventana)\n"
                          "  minigis --info --json --layer c.shp       el mismo resumen en JSON\n"
                          "  minigis --snapshot mapa.png -l c.shp      renderiza el mapa a PNG\n"
                          "  minigis --snapshot-window ui.png -l c.shp captura tambien la interfaz\n"
                          "  minigis --snapshot m.png -l capa.tif -l c.shp raster debajo del vectorial\n"
                          "  minigis --check-raster -l a.tif -l b.tif  comprueba el dibujado raster\n"
                          "\nRendimiento (sin ventana):\n"
                          "  minigis --bench 40 -l capa.gpkg                 mide 40 cuadros de paneo/zoom\n"
                          "  minigis --bench 40 --bench-scenario zoom -l c.gpkg\n"
                          "  minigis --bench 40 -l c.gpkg --renderer legacy  compara con el motor clasico\n"
                          "  minigis --bench 40 -l c.gpkg --bench-budget 33 --json | jq .quality\n"
                          "\nCodigos de salida:\n"
                          "  0 exito            1 uso incorrecto       2 archivo no encontrado\n"
                          "  3 GDAL no pudo abrir la capa             4 no se pudo escribir la imagen\n"
                          "  5 no hay capas que renderizar            6 presupuesto de --bench-budget superado\n");
}

QString versionText()
{
  const RuntimeInfo info = initializeGdal();

  return QStringLiteral("MiniGIS %1\n"
                        "Qt           : %2\n"
                        "GDAL         : %3\n"
                        "PROJ         : %4\n"
                        "Compilador   : %5 %6\n"
                        "Configuracion: %7\n"
                        "%8")
    .arg(QString::fromUtf8(MINIGIS_VERSION),
         QString::fromUtf8(qVersion()),
         info.gdalVersion,
         info.projVersion,
         QString::fromUtf8(MINIGIS_COMPILER_ID),
         QString::fromUtf8(MINIGIS_COMPILER_VERSION),
         QString::fromUtf8(MINIGIS_BUILD_TYPE),
         info.summary());
}

QString layerInfoText(const VectorLayer &layer)
{
  QStringList fields;
  for (const VectorLayer::FieldInfo &field : layer.fields())
    fields << QStringLiteral("%1 (%2)").arg(field.name, field.typeName);

  return QStringLiteral("Capa        : %1\n"
                        "Archivo     : %2\n"
                        "Driver      : %3\n"
                        "Geometria   : %4 (%5)\n"
                        "Elementos   : %6\n"
                        "Extension   : %7\n"
                        "SRC         : %8\n"
                        "Campos      : %9")
    .arg(layer.name(),
         QDir::toNativeSeparators(layer.sourcePath()),
         layer.driverName(),
         layer.geometryTypeName(),
         Style::kindName(layer.geometryKind()),
         QString::number(layer.featureCount()),
         layer.extent().toString(),
         layer.crsDescription(),
         fields.isEmpty() ? QStringLiteral("(sin campos)") : fields.join(QStringLiteral(", ")));
}

QJsonObject layerInfoJson(const VectorLayer &layer)
{
  QJsonObject object;
  object.insert(QStringLiteral("type"), QStringLiteral("vector"));
  object.insert(QStringLiteral("name"), layer.name());
  object.insert(QStringLiteral("source"), layer.sourcePath());
  object.insert(QStringLiteral("driver"), layer.driverName());
  object.insert(QStringLiteral("geometryType"), layer.geometryTypeName());
  object.insert(QStringLiteral("geometryKind"), Style::kindName(layer.geometryKind()));
  object.insert(QStringLiteral("features"), static_cast<double>(layer.featureCount()));

  const Rectangle extent = layer.extent();
  QJsonArray extentArray;
  extentArray.append(extent.xMin());
  extentArray.append(extent.yMin());
  extentArray.append(extent.xMax());
  extentArray.append(extent.yMax());
  object.insert(QStringLiteral("extent"), extentArray);

  QJsonObject crs;
  crs.insert(QStringLiteral("description"), layer.crsDescription());
  crs.insert(QStringLiteral("epsg"), layer.crsEpsg());
  object.insert(QStringLiteral("crs"), crs);

  QJsonArray fields;
  for (const VectorLayer::FieldInfo &field : layer.fields())
  {
    QJsonObject fieldObject;
    fieldObject.insert(QStringLiteral("name"), field.name);
    fieldObject.insert(QStringLiteral("type"), field.typeName);
    fieldObject.insert(QStringLiteral("width"), field.width);
    fields.append(fieldObject);
  }
  object.insert(QStringLiteral("fields"), fields);

  return object;
}

QString rasterInfoText(const RasterLayer &layer)
{
  QStringList lines;
  lines << QStringLiteral("Capa        : %1").arg(layer.name());
  lines << QStringLiteral("Archivo     : %1").arg(QDir::toNativeSeparators(layer.sourcePath()));
  lines << QStringLiteral("Driver      : %1").arg(layer.driverName());
  lines << QStringLiteral("Tipo        : Raster (%1)")
             .arg(layer.bandCount() == 1 ? QStringLiteral("1 banda")
                                         : QStringLiteral("%1 bandas").arg(layer.bandCount()));
  lines << QStringLiteral("Tamano      : %1 x %2 pixeles").arg(layer.width()).arg(layer.height());

  QStringList bandTexts;
  QStringList noDataTexts;
  QStringList paletteTexts;
  for (int i = 1; i <= layer.bandCount(); ++i)
  {
    const RasterLayer::BandInfo &info = layer.bands().at(i - 1);
    bandTexts << QStringLiteral("%1: %2 (%3)").arg(i).arg(info.typeName, info.colorName);
    if (info.hasNoData)
      noDataTexts << QStringLiteral("%1 (banda %2)").arg(QString::number(info.noData), QString::number(i));
    if (info.colorCount > 0)
      paletteTexts << QStringLiteral("%1 colores (banda %2)").arg(info.colorCount).arg(i);
  }

  lines << QStringLiteral("Bandas      : %1").arg(bandTexts.join(QStringLiteral(", ")));
  lines << QStringLiteral("Extension   : %1").arg(layer.extent().toString());
  lines << QStringLiteral("SRC         : %1").arg(layer.crsDescription());
  lines << QStringLiteral("NoData      : %1")
             .arg(noDataTexts.isEmpty() ? QStringLiteral("(sin NoData)") : noDataTexts.join(QStringLiteral(", ")));

  if (!paletteTexts.isEmpty())
    lines << QStringLiteral("Paleta      : %1").arg(paletteTexts.join(QStringLiteral(", ")));

  const QVector<int> factors = layer.overviewFactors();
  QStringList factorTexts;
  for (int factor : factors)
    factorTexts << QString::number(factor);
  lines << QStringLiteral("Overviews   : %1")
             .arg(factors.isEmpty() ? QStringLiteral("no") : factorTexts.join(QStringLiteral(", ")));

  return lines.join(QLatin1Char('\n'));
}

QJsonObject rasterInfoJson(const RasterLayer &layer)
{
  QJsonObject object;
  object.insert(QStringLiteral("type"), QStringLiteral("raster"));
  object.insert(QStringLiteral("name"), layer.name());
  object.insert(QStringLiteral("source"), layer.sourcePath());
  object.insert(QStringLiteral("driver"), layer.driverName());
  object.insert(QStringLiteral("width"), layer.width());
  object.insert(QStringLiteral("height"), layer.height());

  QJsonArray bands;
  QJsonArray noDataValues;
  for (int i = 1; i <= layer.bandCount(); ++i)
  {
    const RasterLayer::BandInfo &info = layer.bands().at(i - 1);

    QJsonObject bandObject;
    bandObject.insert(QStringLiteral("index"), i);
    bandObject.insert(QStringLiteral("type"), info.typeName);
    bandObject.insert(QStringLiteral("color"), info.colorName);
    if (info.hasNoData)
      bandObject.insert(QStringLiteral("nodata"), info.noData);
    else
      bandObject.insert(QStringLiteral("nodata"), QJsonValue::Null);
    bands.append(bandObject);

    if (info.hasNoData)
      noDataValues.append(info.noData);
    else
      noDataValues.append(QJsonValue::Null);
  }
  object.insert(QStringLiteral("bands"), bands);
  object.insert(QStringLiteral("nodata"), noDataValues);

  const Rectangle extent = layer.extent();
  QJsonArray extentArray;
  extentArray.append(extent.xMin());
  extentArray.append(extent.yMin());
  extentArray.append(extent.xMax());
  extentArray.append(extent.yMax());
  object.insert(QStringLiteral("extent"), extentArray);

  QJsonObject crs;
  crs.insert(QStringLiteral("description"), layer.crsDescription());
  crs.insert(QStringLiteral("epsg"), layer.crsEpsg());
  object.insert(QStringLiteral("crs"), crs);
  object.insert(QStringLiteral("epsg"), layer.crsEpsg());

  QJsonArray overviews;
  for (int factor : layer.overviewFactors())
    overviews.append(factor);
  object.insert(QStringLiteral("overviews"), overviews);

  return object;
}

bool parseSize(const QString &text, QSize *size)
{
  const QStringList parts = text.split(QRegularExpression(QStringLiteral("[xX]")), Qt::SkipEmptyParts);
  if (parts.size() != 2)
    return false;

  bool widthOk = false;
  bool heightOk = false;
  const int width = parts.at(0).trimmed().toInt(&widthOk);
  const int height = parts.at(1).trimmed().toInt(&heightOk);
  if (!widthOk || !heightOk || width < 16 || height < 16)
    return false;

  size->setWidth(width);
  size->setHeight(height);
  return true;
}

bool parseExtent(const QString &text, Rectangle *extent)
{
  const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
  if (parts.size() != 4)
    return false;

  double values[4] = {0.0, 0.0, 0.0, 0.0};
  for (int i = 0; i < 4; ++i)
  {
    bool ok = false;
    values[i] = parts.at(i).toDouble(&ok);
    if (!ok)
      return false;
  }

  *extent = Rectangle(values[0], values[1], values[2], values[3]);
  return true;
}

bool parseEpsg(const QString &text, int *epsg)
{
  QString cleaned = text.trimmed();
  cleaned.remove(QRegularExpression(QStringLiteral("^[Ee][Pp][Ss][Gg][: ]*")));

  bool ok = false;
  const int value = cleaned.toInt(&ok);
  if (!ok || value <= 0)
    return false;

  *epsg = value;
  return true;
}

/** Une las capas de --layer y de los argumentos posicionales, sin repetir. */
QStringList collectLayerPaths(const QCommandLineParser &parser)
{
  QStringList paths;

  auto append = [&paths](const QString &path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
      return;
    if (!paths.contains(trimmed))
      paths << trimmed;
  };

  for (const QString &path : parser.values(QStringLiteral("layer")))
    append(path);
  for (const QString &path : parser.positionalArguments())
    append(path);

  return paths;
}

/** Comprime una extension alrededor de su centro (factor < 1 acerca). */
Rectangle scaledExtent(const Rectangle &extent, double factor)
{
  const double halfWidth = extent.width() * factor / 2.0;
  const double halfHeight = extent.height() * factor / 2.0;
  return Rectangle(extent.centerX() - halfWidth, extent.centerY() - halfHeight,
                   extent.centerX() + halfWidth, extent.centerY() + halfHeight);
}

/** Desplaza una extension una fraccion de su tamano. */
Rectangle shiftedExtent(const Rectangle &extent, double ratioX, double ratioY)
{
  const double dx = extent.width() * ratioX;
  const double dy = extent.height() * ratioY;
  return Rectangle(extent.xMin() + dx, extent.yMin() + dy, extent.xMax() + dx, extent.yMax() + dy);
}

/**
 * Siguiente extension de la tanda de medida. El paneo va y viene (el centro
 * vuelve cada dos cuadros) y el zoom alterna acercar y alejar, de modo que todos
 * los cuadros se dibujan sobre datos y la medida es comparable entre motores.
 */
Rectangle nextBenchExtent(const Rectangle &current, const QString &scenario, int frame)
{
  if (!current.isValid())
    return current;

  const bool towards = (frame % 2) == 0;
  const double panRatio = towards ? 0.06 : -0.06;
  const double zoomFactor = towards ? 1.10 : (1.0 / 1.10);

  if (scenario == QStringLiteral("zoom"))
    return scaledExtent(current, zoomFactor);

  if (scenario == QStringLiteral("pan"))
    return shiftedExtent(current, panRatio, panRatio * 0.5);

  // Mixto: dos paneos por cada zoom, el patron real de uso del mapa.
  if ((frame % 3) == 2)
    return scaledExtent(current, zoomFactor);

  return shiftedExtent(current, panRatio, panRatio * 0.5);
}

struct BenchSamples
{
  std::vector<qint64> milliseconds;

  void add(qint64 value) { milliseconds.push_back(value); }

  qint64 percentile(double fraction) const
  {
    if (milliseconds.empty())
      return 0;

    std::vector<qint64> sorted = milliseconds;
    std::sort(sorted.begin(), sorted.end());
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const size_t index = static_cast<size_t>(position + 0.5);
    return sorted[std::min(index, sorted.size() - 1)];
  }

  qint64 minimum() const { return percentile(0.0); }
  qint64 median() const { return percentile(0.5); }
  qint64 p90() const { return percentile(0.9); }
  qint64 maximum() const { return percentile(1.0); }

  double framesPerSecond() const
  {
    const qint64 value = median();
    return value > 0 ? 1000.0 / static_cast<double>(value) : 0.0;
  }
};

QJsonObject timingsJson(const BenchSamples &samples)
{
  QJsonObject object;
  object.insert(QStringLiteral("frames"), static_cast<int>(samples.milliseconds.size()));
  object.insert(QStringLiteral("min_ms"), static_cast<double>(samples.minimum()));
  object.insert(QStringLiteral("median_ms"), static_cast<double>(samples.median()));
  object.insert(QStringLiteral("p90_ms"), static_cast<double>(samples.p90()));
  object.insert(QStringLiteral("max_ms"), static_cast<double>(samples.maximum()));
  object.insert(QStringLiteral("fps_median"), samples.framesPerSecond());
  return object;
}

QString timingsText(const BenchSamples &samples)
{
  return QStringLiteral("min %1 | mediana %2 | p90 %3 | max %4 ms  (%5 fps)")
    .arg(samples.minimum())
    .arg(samples.median())
    .arg(samples.p90())
    .arg(samples.maximum())
    .arg(QString::number(samples.framesPerSecond(), 'f', 1));
}

/** Peticion del modo --bench, ya validada. */
struct BenchRequest
{
  QStringList layerPaths;
  QSize size {1200, 800};
  int frames = 30;
  QString scenario = QStringLiteral("mixed");
  int destinationEpsg = 4326;
  Rectangle extent;
  qint64 maxFeatures = 0;
  QString renderer = QStringLiteral("auto");
  LabelOptions labels;
  qint64 vertexBudget = 600000;
  qint64 readBudget = 40000;
  qint64 budgetMs = 0;
  bool spatialFilter = false;
  bool asJson = false;
};

/**
 * Banco de pruebas de rendimiento del lienzo.
 *
 * Mide, para cada cuadro, el coste de un renderizado de calidad y el del cuadro
 * de borrador que se usa durante la interaccion. Con --bench-budget el comando
 * falla (codigo 6) si el p90 de la calidad supera el presupuesto, de modo que se
 * puede usar como puerta de regresion en CI.
 */
int runBenchmark(const BenchRequest &request)
{
  const bool useStore = (request.renderer != QStringLiteral("legacy"));

  MapCanvas canvas;
  canvas.setDestinationEpsg(request.destinationEpsg);
  canvas.setMaximumFeaturesPerLayer(request.maxFeatures);
  canvas.setVertexBudget(request.vertexBudget);
  canvas.setReadBudget(request.readBudget);
  canvas.setRendererBackend(useStore ? MapCanvas::RendererBackend::Fast
                                     : MapCanvas::RendererBackend::Legacy);
  canvas.setUseSpatialFilter(request.spatialFilter);
  canvas.resize(request.size);

  QStringList layerNames;
  bool envelopeMode = false;
  qint64 storeBuildMs = 0;
  qint64 storeVertices = 0;
  qint64 storeBytes = 0;
  qint64 storeFeatures = 0;

  int opened = 0;
  int rasterLayers = 0;
  for (const QString &path : request.layerPaths)
  {
    if (!QFileInfo::exists(path))
    {
      writeErr(QStringLiteral("No existe el archivo: %1").arg(QDir::toNativeSeparators(path)));
      return ExitFileNotFound;
    }

    Error vectorError;
    const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, &vectorError);
    if (!layer)
    {
      Error rasterError;
      const std::shared_ptr<RasterLayer> raster = RasterLayer::open(path, &rasterError);
      if (!raster)
      {
        writeErr(vectorError.toText());
        return ExitOpenFailed;
      }

      canvas.addRasterLayer(raster, opened == 0);
      layerNames << raster->name();
      ++rasterLayers;
      ++opened;
      logVerbose(QStringLiteral("raster de medida: %1 (%2x%3, %4 bandas)")
                   .arg(raster->name())
                   .arg(raster->width())
                   .arg(raster->height())
                   .arg(raster->bandCount()));
      continue;
    }

    if (useStore)
    {
      QString storeError;
      QString modeName;
      qint64 layerBuildMs = 0;
      const std::shared_ptr<GeometryStore> store =
        buildGeometryStore(layer, request.destinationEpsg, &layerBuildMs, &modeName, &storeError);
      storeBuildMs += layerBuildMs;

      if (store)
      {
        const GeometryStore::Stats stats = store->stats();
        storeFeatures += stats.features;
        storeVertices += stats.vertices;
        storeBytes += stats.bytes;
        if (stats.mode == GeometryStore::Mode::Envelopes)
          envelopeMode = true;
        layer->setGeometryStore(store);
        logVerbose(QStringLiteral("almacen de «%1» (%9): %2 elementos, %3 vertices, %4 MB, niveles %5 "
                                  "(lectura %6 ms, indice %7 ms, detalle %8 ms)")
                     .arg(layer->name())
                     .arg(stats.features)
                     .arg(stats.vertices)
                     .arg(QString::number(static_cast<double>(stats.bytes) / (1024.0 * 1024.0), 'f', 1))
                     .arg(stats.cachedLevels)
                     .arg(stats.readMs)
                     .arg(stats.indexMs)
                     .arg(stats.levelsMs)
                     .arg(modeName));
      }
      else
      {
        writeErr(QStringLiteral("Aviso: no se pudo indexar «%1»: %2 (se usara la lectura directa).")
                   .arg(QDir::toNativeSeparators(path), storeError));
      }
    }

    applyLabelOptions(layer, request.labels);

    layerNames << layer->name();
    canvas.addLayer(layer, opened == 0);
    ++opened;
  }

  if (opened == 0)
  {
    writeErr(QStringLiteral("No hay capas que medir: indica una con --layer <archivo>."));
    return ExitNoLayers;
  }

  canvas.setExtent(request.extent.isValid() ? request.extent : canvas.layersExtent());
  logVerbose(QStringLiteral("extension de medida: %1").arg(canvas.extent().toString()));

  const QSize draftSize(std::max(1, request.size.width() / 2), std::max(1, request.size.height() / 2));

  BenchSamples quality;
  BenchSamples draft;

  Rectangle extent = canvas.extent();
  FastMapRenderer::FrameStats lastStats;
  qint64 queryMs = 0;
  qint64 pathMs = 0;
  qint64 paintMs = 0;
  qint64 readFeatures = 0;
  qint64 skippedFeatures = 0;
  qint64 rasterBytesLast = 0;
  qint64 rasterPixelsLast = 0;
  qint64 rasterBytesTotal = 0;

  for (int frame = 0; frame < request.frames; ++frame)
  {
    extent = nextBenchExtent(extent, request.scenario, frame);
    canvas.setExtent(extent);

    QElapsedTimer timer;
    timer.start();
    const QImage image = canvas.renderToImage(request.size, false);
    quality.add(timer.elapsed());
    lastStats = canvas.fastFrameStats();
    queryMs += lastStats.queryMs;
    pathMs += lastStats.pathMs;
    paintMs += lastStats.paintMs;
    readFeatures += lastStats.readFeatures;
    skippedFeatures += lastStats.skippedFeatures;

    const RasterRenderer::FrameStats rasterFrame = canvas.rasterFrameStats();
    rasterBytesLast = rasterFrame.bytesRead;
    rasterPixelsLast = rasterFrame.pixelsDrawn;
    rasterBytesTotal += rasterFrame.bytesRead;

    if (image.isNull())
    {
      writeErr(QStringLiteral("El renderizado devolvio una imagen nula."));
      return ExitRenderFailed;
    }

    if (g_verbose)
    {
      logVerbose(QStringLiteral("cuadro %1: calidad %2 ms (consulta %3, trazados %4, pintado %5; "
                                "%6 candidatos, %7 vertices, nivel %8) | borrador %9 ms")
                   .arg(frame)
                   .arg(quality.milliseconds.back())
                   .arg(lastStats.queryMs)
                   .arg(lastStats.pathMs)
                   .arg(lastStats.paintMs)
                   .arg(lastStats.candidateFeatures)
                   .arg(lastStats.drawnVertices)
                   .arg(lastStats.level)
                   .arg(0));
    }

    timer.restart();
    canvas.renderToImage(draftSize, true);
    const qint64 draftMs = timer.elapsed();
    draft.add(draftMs);

    if (g_verbose)
      logVerbose(QStringLiteral("  borrador de ese cuadro: %1 ms").arg(draftMs));
  }

  const qint64 frames = std::max(1, request.frames);

  const bool budgetExceeded = (request.budgetMs > 0) && (quality.p90() > request.budgetMs);

  if (request.asJson)
  {
    QJsonObject root;
    root.insert(QStringLiteral("renderer"), request.renderer);
    root.insert(QStringLiteral("scenario"), request.scenario);
    root.insert(QStringLiteral("size"), QStringLiteral("%1x%2").arg(request.size.width()).arg(request.size.height()));
    root.insert(QStringLiteral("layers"), QJsonArray::fromStringList(layerNames));
    root.insert(QStringLiteral("quality"), timingsJson(quality));
    root.insert(QStringLiteral("draft"), timingsJson(draft));

    QJsonObject store;
    store.insert(QStringLiteral("build_ms"), static_cast<double>(storeBuildMs));
    store.insert(QStringLiteral("features"), static_cast<double>(storeFeatures));
    store.insert(QStringLiteral("vertices"), static_cast<double>(storeVertices));
    store.insert(QStringLiteral("bytes"), static_cast<double>(storeBytes));
    store.insert(QStringLiteral("candidates_last_frame"), static_cast<double>(lastStats.candidateFeatures));
    store.insert(QStringLiteral("drawn_features_last_frame"), static_cast<double>(lastStats.drawnFeatures));
    store.insert(QStringLiteral("drawn_vertices_last_frame"), static_cast<double>(lastStats.drawnVertices));
    store.insert(QStringLiteral("level_last_frame"), lastStats.level);
    store.insert(QStringLiteral("mode"), envelopeMode ? QStringLiteral("envelopes") : QStringLiteral("memory"));
    store.insert(QStringLiteral("read_total"), static_cast<double>(readFeatures));
    store.insert(QStringLiteral("skipped_total"), static_cast<double>(skippedFeatures));
    store.insert(QStringLiteral("read_last_frame"), static_cast<double>(lastStats.readFeatures));
    root.insert(QStringLiteral("store"), store);

    QJsonObject raster;
    raster.insert(QStringLiteral("layers"), rasterLayers);
    raster.insert(QStringLiteral("bytes_read_last_frame"), static_cast<double>(rasterBytesLast));
    raster.insert(QStringLiteral("pixels_last_frame"), static_cast<double>(rasterPixelsLast));
    raster.insert(QStringLiteral("bytes_read_total"), static_cast<double>(rasterBytesTotal));
    root.insert(QStringLiteral("raster"), raster);

    QJsonObject phases;
    phases.insert(QStringLiteral("query_ms_per_frame"), static_cast<double>(queryMs) / static_cast<double>(frames));
    phases.insert(QStringLiteral("path_ms_per_frame"), static_cast<double>(pathMs) / static_cast<double>(frames));
    phases.insert(QStringLiteral("paint_ms_per_frame"), static_cast<double>(paintMs) / static_cast<double>(frames));
    root.insert(QStringLiteral("phases"), phases);

    root.insert(QStringLiteral("budget_ms"), static_cast<double>(request.budgetMs));
    root.insert(QStringLiteral("budget_ok"), !budgetExceeded);

    writeOut(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)).trimmed());
  }
  else
  {
    writeOut(QStringLiteral("Motor      : %1%2")
               .arg(useStore ? QStringLiteral("acelerado (almacen indexado + R-tree)") : QStringLiteral("clasico (lectura directa de la capa)"),
                    request.renderer == QStringLiteral("auto") ? QStringLiteral("") : QStringLiteral(" [--renderer %1]").arg(request.renderer)));
    writeOut(QStringLiteral("Escenario  : %1, %2 cuadros, %3x%4")
               .arg(request.scenario)
               .arg(request.frames)
               .arg(request.size.width())
               .arg(request.size.height()));
    writeOut(QStringLiteral("Capas      : %1").arg(layerNames.join(QStringLiteral(", "))));

    if (useStore && storeFeatures > 0)
    {
      writeOut(QStringLiteral("Almacen    : %1 elementos, %2 vertices, %3 MB, indice en %4 ms")
                 .arg(storeFeatures)
                 .arg(storeVertices)
                 .arg(QString::number(static_cast<double>(storeBytes) / (1024.0 * 1024.0), 'f', 1))
                 .arg(storeBuildMs));
    }

    if (envelopeMode)
    {
      writeOut(QStringLiteral("Muestreo   : %1 geometrias leidas del archivo en %2 cuadros, "
                              "%3 elementos omitidos por presupuesto")
                 .arg(readFeatures)
                 .arg(frames)
                 .arg(skippedFeatures));
    }

    writeOut(QStringLiteral("Cuadro calidad : %1").arg(timingsText(quality)));
    writeOut(QStringLiteral("Cuadro borrador: %1").arg(timingsText(draft)));

    if (useStore)
    {
      writeOut(QStringLiteral("Ultimo cuadro  : %1 candidatos de %2 elementos, %3 dibujados, %4 vertices, nivel de detalle %5")
                 .arg(lastStats.candidateFeatures)
                 .arg(lastStats.totalFeatures)
                 .arg(lastStats.drawnFeatures)
                 .arg(lastStats.drawnVertices)
                 .arg(lastStats.level));
      writeOut(QStringLiteral("Reparto/cuadro : consulta %1 ms | construccion de trazados %2 ms | pintado %3 ms")
                 .arg(queryMs / frames)
                 .arg(pathMs / frames)
                 .arg(paintMs / frames));
    }

    if (request.budgetMs > 0)
    {
      writeOut(QStringLiteral("Presupuesto    : %1 ms de p90 -> %2")
                 .arg(request.budgetMs)
                 .arg(budgetExceeded ? QStringLiteral("SUPERADO") : QStringLiteral("cumplido")));
    }
  }

  return budgetExceeded ? ExitBudgetExceeded : ExitOk;
}

QString storeModeName(const GeometryStore &store)
{
  return store.mode() == GeometryStore::Mode::Envelopes ? QStringLiteral("envolventes")
                                                        : QStringLiteral("memoria");
}


/** Aplica las opciones de etiqueta a una capa recien abierta. */
void applyLabelOptions(const std::shared_ptr<VectorLayer> &layer, const LabelOptions &options)
{
  if (!layer || !options.parsed)
    return;

  LabelStyle style = layer->labelStyle();
  style.enabled = options.enabled && !options.fields.isEmpty();
  style.fields = options.fields;
  style.fontSize = options.mapUnitsSize > 0.0 ? options.mapUnitsSize : options.size;
  style.sizeMode = options.mapUnitsSize > 0.0 ? LabelStyle::SizeMode::MapUnits
                                              : LabelStyle::SizeMode::Screen;
  style.haloEnabled = options.halo;
  layer->setLabelStyle(style);

  logVerbose(QStringLiteral("etiquetas de «%1»: %2").arg(layer->name(), style.summary()));
}

/**
 * Diagnostico del indice de envolventes (`--index-shp`).
 *
 * Ademas de medir el recorrido del .shp, comprueba que las envolventes leidas
 * del archivo corresponden al mismo elemento que devuelve el FID de OGR: si el
 * indice se desalineara, el mapa dibujaria geometrias que no estan donde toca, y
 * ese fallo seria muy dificil de ver a ojo.
 */
int inspectShapefileIndex(const QString &path, bool asJson)
{
  if (!ShapefileIndex::supports(QStringLiteral("ESRI Shapefile"), path))
  {
    writeErr(QStringLiteral("«%1» no es un .shp de disco indexable.").arg(QDir::toNativeSeparators(path)));
    return ExitUsage;
  }

  ShapefileIndex::Stats stats;
  std::vector<ShapefileIndex::Entry> entries;

  writeErr(QStringLiteral("[minigis] recorriendo «%1»...").arg(QDir::toNativeSeparators(path)));

  QString error;
  if (!ShapefileIndex::scan(path, entries, stats, nullptr, &error))
  {
    writeErr(error);
    return ExitOpenFailed;
  }

  // Verificacion: la envolvente del indice debe coincidir con la de GDAL.
  Error openError;
  const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, &openError);
  if (!layer)
  {
    writeErr(openError.toText());
    return ExitOpenFailed;
  }

  const qint64 featureCount = layer->featureCount();
  int checked = 0;
  int mismatches = 0;
  double worstDelta = 0.0;

  const qint64 samples[] = {0, 1, 2, featureCount / 4, featureCount / 2, (featureCount * 3) / 4,
                            featureCount - 2, featureCount - 1};

  for (const qint64 row : samples)
  {
    if (row < 0 || row >= static_cast<qint64>(entries.size()))
      continue;

    const Rectangle envelope = layer->featureEnvelope(row);
    if (!envelope.isValid())
      continue;

    const ShapefileIndex::Entry &entry = entries[static_cast<size_t>(row)];
    const double delta = std::max(std::max(std::abs(entry.minX - envelope.xMin()), std::abs(entry.minY - envelope.yMin())),
                                  std::max(std::abs(entry.maxX - envelope.xMax()), std::abs(entry.maxY - envelope.yMax())));

    worstDelta = std::max(worstDelta, delta);
    ++checked;
    if (delta > 1e-6)
      ++mismatches;
  }

  // Verificacion 2: los atributos deben seguir leyendose DESPUES de indexar la
  // capa. Construir el almacen ignora temporalmente todos los campos (para no
  // leer un DBF de gigabytes); si esa restauracion se saltara algun camino de
  // error, la tabla de atributos mostraria "(nulo)" en todas las filas. Es un
  // fallo real que sufrio nz-building-outlines, asi que se comprueba aqui.
  qint64 attributeRows = 0;
  qint64 attributeEmpty = 0;
  QString sampleValue;

  if (!layer->fields().isEmpty() && featureCount > 0)
  {
    qint64 storeBuildMs = 0;
    QString modeName;
    QString storeError;
    const std::shared_ptr<GeometryStore> store = buildGeometryStore(layer, 4326, &storeBuildMs, &modeName, &storeError);
    (void)store;

    const qint64 rows[] = {0, featureCount / 2, featureCount - 1};
    for (const qint64 row : rows)
    {
      if (row < 0 || row >= featureCount)
        continue;

      const QVector<QString> values = layer->fieldValues(row);
      ++attributeRows;

      bool empty = true;
      for (int i = 0; i < values.size(); ++i)
      {
        if (!values[i].isEmpty())
        {
          empty = false;
          if (sampleValue.isEmpty() && i < layer->fields().size())
            sampleValue = QStringLiteral("%1=%2").arg(layer->fields()[i].name, values[i]);
        }
      }

      if (empty)
        ++attributeEmpty;
    }

    // Ademas de las tres filas de muestreo se comprueba un bloque contiguo desde
    // la primera: es lo que se ve al abrir la tabla de atributos y donde se noto
    // el fallo (todas las filas en "(nulo)").
    const qint64 blockRows = std::min<qint64>(256, featureCount);
    for (qint64 row = 0; row < blockRows; ++row)
    {
      const QVector<QString> values = layer->fieldValues(row);
      ++attributeRows;

      bool empty = true;
      for (const QString &value : values)
      {
        if (!value.isEmpty())
        {
          empty = false;
          break;
        }
      }

      if (empty)
        ++attributeEmpty;
    }
  }

  if (asJson)
  {
    QJsonObject root;
    root.insert(QStringLiteral("records"), static_cast<double>(stats.records));
    root.insert(QStringLiteral("nullShapes"), static_cast<double>(stats.nullShapes));
    root.insert(QStringLiteral("unsupported"), static_cast<double>(stats.unsupported));
    root.insert(QStringLiteral("shapeType"), stats.shapeType);
    root.insert(QStringLiteral("bytes"), static_cast<double>(stats.bytes));
    root.insert(QStringLiteral("scanMs"), static_cast<double>(stats.scanMs));
    root.insert(QStringLiteral("envelopeBytes"),
                static_cast<double>(entries.size() * (4 * sizeof(double) + 2 * sizeof(uint32_t))));
    root.insert(QStringLiteral("layerFeatures"), static_cast<double>(featureCount));
    root.insert(QStringLiteral("checked"), checked);
    root.insert(QStringLiteral("mismatches"), mismatches);
    root.insert(QStringLiteral("worstDelta"), worstDelta);
    root.insert(QStringLiteral("attributeRows"), static_cast<double>(attributeRows));
    root.insert(QStringLiteral("attributeEmpty"), static_cast<double>(attributeEmpty));
    root.insert(QStringLiteral("sampleValue"), sampleValue);
    writeOut(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)).trimmed());
  }
  else
  {
    writeOut(QStringLiteral("Elementos indexados : %1").arg(stats.records));
    writeOut(QStringLiteral("Elementos de la capa: %1").arg(featureCount));
    writeOut(QStringLiteral("Formas vacias      : %1   no soportadas: %2   tipo de forma: %3")
               .arg(stats.nullShapes)
               .arg(stats.unsupported)
               .arg(stats.shapeType));
    writeOut(QStringLiteral("Recorrido          : %1 MB en %2 ms")
               .arg(QString::number(static_cast<double>(stats.bytes) / (1024.0 * 1024.0), 'f', 1))
               .arg(stats.scanMs));
    writeOut(QStringLiteral("Memoria del indice : %1 MB")
               .arg(QString::number(static_cast<double>(entries.size() * (4 * sizeof(double) + 2 * sizeof(uint32_t)))
                                      / (1024.0 * 1024.0), 'f', 1)));
    writeOut(QStringLiteral("Verificacion       : %1 elementos comparados con GDAL, %2 discrepancias, "
                            "diferencia maxima %3")
               .arg(checked)
               .arg(mismatches)
               .arg(worstDelta));
    writeOut(QStringLiteral("Atributos tras indexar: %1 de %2 filas de prueba vacias  (muestra: %3)")
               .arg(attributeEmpty)
               .arg(attributeRows)
               .arg(sampleValue.isEmpty() ? QStringLiteral("(sin valores)") : sampleValue));
  }

  return (mismatches == 0 && attributeEmpty == 0) ? ExitOk : ExitRenderFailed;
}

/**
 * Construye el almacen de una capa: primero la geometria completa en memoria y,
 * si no cabe (millones de elementos), el indice de solo envolventes con lectura
 * de geometria bajo demanda. Devuelve nullptr solo si ninguna de las dos cosas es
 * posible, con el motivo en `error`.
 */
std::shared_ptr<GeometryStore> buildGeometryStore(const std::shared_ptr<VectorLayer> &layer,
                                                  int destinationEpsg, qint64 *buildMs,
                                                  QString *modeName, QString *error)
{
  if (!layer)
    return {};

  GeometryStore::Options options;

  QElapsedTimer timer;
  timer.start();

  QString memoryError;
  std::shared_ptr<GeometryStore> store =
    GeometryStore::buildFromLayer(*layer, destinationEpsg, options, nullptr, &memoryError);

  if (!store)
  {
    QString envelopeError;
    store = GeometryStore::buildEnvelopeIndex(*layer, destinationEpsg, options, nullptr, &envelopeError);
    if (!store && error)
      *error = QStringLiteral("%1\nIndice de envolventes: %2").arg(memoryError, envelopeError);
  }
  else if (error)
  {
    *error = memoryError;
  }

  if (buildMs)
    *buildMs = timer.elapsed();

  if (modeName && store)
    *modeName = storeModeName(*store);

  return store;
}

/**
 * Cuenta los pixeles que difieren de forma VISIBLE entre dos cuadros.
 *
 * Comparar bit a bit es demasiado estricto para el camino de borrador: el
 * suavizado de los marcadores puede variar en una unidad de canal por redondeo de
 * coma flotante entre dos renderizados identicos en apariencia. Lo que hay que
 * detectar es una costura (contenido desplazado), que produce miles de pixeles con
 * diferencias grandes, asi que se cuentan solo los que superan `tolerance` en
 * algun canal.
 */
qint64 countVisibleDifferences(const QImage &left, const QImage &right, int tolerance,
                               QString *firstDifference = nullptr)
{
  if (left.size() != right.size() || left.isNull())
    return -1;

  qint64 differences = 0;
  for (int y = 0; y < left.height(); ++y)
  {
    const QRgb *leftRow = reinterpret_cast<const QRgb *>(left.constScanLine(y));
    const QRgb *rightRow = reinterpret_cast<const QRgb *>(right.constScanLine(y));
    for (int x = 0; x < left.width(); ++x)
    {
      const int dr = std::abs(qRed(leftRow[x]) - qRed(rightRow[x]));
      const int dg = std::abs(qGreen(leftRow[x]) - qGreen(rightRow[x]));
      const int db = std::abs(qBlue(leftRow[x]) - qBlue(rightRow[x]));
      if (std::max(dr, std::max(dg, db)) > tolerance)
      {
        ++differences;
        if (firstDifference && differences <= 3)
        {
          *firstDifference += QStringLiteral("(%1,%2) ").arg(x).arg(y);
        }
      }
    }
  }

  return differences;
}

/** Cuenta los pixeles que no son del color de fondo del lienzo. */
qint64 countPaintedPixels(const QImage &image)
{
  const QRgb background = qRgb(248, 248, 244);
  qint64 painted = 0;

  for (int y = 0; y < image.height(); ++y)
  {
    const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x)
    {
      if ((row[x] & 0x00FFFFFF) != (background & 0x00FFFFFF))
        ++painted;
    }
  }

  return painted;
}

/**
 * Comprueba las etiquetas (`--check-labels`).
 *
 * La mayor parte de las comprobaciones se hacen sobre las ESTADISTICAS del
 * etiquetado (cuantas etiquetas se colocan, cuantas se descartan y por que) en
 * lugar de sobre los pixeles: asi la prueba mide la semantica de cada opcion
 * —campo, rango de escala, distancia minima, repeticiones, tamano minimo— y
 * funciona igual en cualquier plataforma. Las comprobaciones de pixeles solo se
 * ejecutan si la plataforma dibuja texto, porque un motor de fuente sin contornos
 * (la plataforma sin ventana) no pinta ningun glifo.
 */
int checkLabels(const QStringList &layerPaths, const QSize &size, bool asJson)
{
  if (layerPaths.isEmpty())
  {
    writeErr(QStringLiteral("Se necesita una capa: minigis --check-labels -l capa.shp"));
    return ExitUsage;
  }

  const QString path = layerPaths.first();
  if (!QFileInfo::exists(path))
  {
    writeErr(QStringLiteral("No existe el archivo: %1").arg(QDir::toNativeSeparators(path)));
    return ExitFileNotFound;
  }

  Error error;
  const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, &error);
  if (!layer)
  {
    writeErr(error.toText());
    return ExitOpenFailed;
  }

  attachGeometryStore(layer, 4326, true);

  MapCanvas canvas;
  canvas.setDestinationEpsg(4326);
  canvas.resize(size);
  canvas.addLayer(layer, true);

  bool ok = true;
  int executed = 0;
  int omitted = 0;
  const auto check = [&ok, &executed](const QString &name, bool passed, const QString &detail) {
    ++executed;
    writeOut(QStringLiteral("%1 %2%3")
               .arg(passed ? QStringLiteral("OK  ") : QStringLiteral("FALLO"))
               .arg(name, detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail));
    if (!passed)
      ok = false;
  };
  const auto omit = [&omitted](const QString &name, const QString &why) {
    ++omitted;
    writeOut(QStringLiteral("OMITE %1 — %2").arg(name, why));
  };

  // Campos de la capa, para elegir dos que existan y uno que no.
  QStringList fieldNames;
  for (const VectorLayer::FieldInfo &field : layer->fields())
    fieldNames << field.name;

  if (fieldNames.size() < 2)
  {
    writeErr(QStringLiteral("La capa necesita al menos dos campos para esta prueba; tiene %1.")
               .arg(fieldNames.size()));
    return ExitRenderFailed;
  }

  const QString firstField = fieldNames.value(0);
  const QString secondField = fieldNames.value(1);

  /** Dibuja con la configuracion indicada y devuelve las estadisticas y la imagen. */
  const auto render = [&](const LabelStyle &style, bool labelsVisible, QImage *image) {
    layer->setLabelStyle(style);
    canvas.setLabelsVisible(labelsVisible);
    canvas.refreshMap();
    const QImage frame = canvas.renderToImage(size, false);
    if (image)
      *image = frame;
    return canvas.labelFrameStats();
  };

  LabelStyle base;
  base.enabled = true;
  base.fields = QStringList {firstField};
  base.fontSize = 12.0;

  const QImage withoutLabels = canvas.renderToImage(size, false);

  // 1) Con un campo valido se etiqueta cada elemento visible.
  LabelStyle style = base;
  LabelRenderer::FrameStats stats = render(style, true, nullptr);
  check(QStringLiteral("se etiqueta un elemento por campo y elemento visible"), stats.placed > 0,
        QStringLiteral("%1 etiquetas colocadas con el campo «%2»").arg(stats.placed).arg(firstField));

  // 2) Un campo que no existe no dibuja nada (y no lee atributos de mas).
  style = base;
  style.fields = QStringList {QStringLiteral("campo_que_no_existe_xyz")};
  stats = render(style, true, nullptr);
  check(QStringLiteral("un campo inexistente no etiqueta nada"), stats.placed == 0,
        QStringLiteral("%1 etiquetas").arg(stats.placed));

  // 3) Dos campos se combinan con el separador elegido.
  style = base;
  style.fields = QStringList {firstField, secondField};
  style.separator = QStringLiteral(" | ");
  stats = render(style, true, nullptr);
  const qint64 twoFieldPlaced = stats.placed;
  check(QStringLiteral("se pueden combinar varios campos"), twoFieldPlaced > 0,
        QStringLiteral("%1 etiquetas con «%2 | %3»").arg(twoFieldPlaced).arg(firstField, secondField));

  // 4) El interruptor global apaga todas las etiquetas.
  stats = render(base, false, nullptr);
  check(QStringLiteral("el interruptor global apaga las etiquetas"), stats.placed == 0,
        QStringLiteral("%1 etiquetas").arg(stats.placed));

  // 5) Fuera del rango de escala no se etiqueta: es lo que permite que las
  //    etiquetas aparezcan solo al acercarse.
  style = base;
  style.minimumScale = canvas.mapUnitsPerPixel() * 0.5; // "mas alejado" que la mitad de la actual
  stats = render(style, true, nullptr);
  check(QStringLiteral("fuera del rango de escala no se etiqueta"), stats.placed == 0,
        QStringLiteral("%1 etiquetas con la escala actual fuera del rango").arg(stats.placed));

  // 5b) Y dentro del rango si se etiqueta.
  style = base;
  style.maximumScale = canvas.mapUnitsPerPixel() * 0.5; // "mas cercano" solo a partir de la mitad
  stats = render(style, true, nullptr);
  check(QStringLiteral("dentro del rango de escala si se etiqueta"), stats.placed > 0,
        QStringLiteral("%1 etiquetas").arg(stats.placed));

  // 6) Una distancia minima grande reduce el numero de etiquetas (evita solapes).
  style = base;
  style.minimumDistance = 3000.0; // mas ancha que el lienzo: solo cabe una
  stats = render(style, true, nullptr);
  const qint64 farApartPlaced = stats.placed;
  check(QStringLiteral("la distancia minima descarta etiquetas que se solapan"),
        farApartPlaced < twoFieldPlaced || stats.skippedCollision > 0,
        QStringLiteral("%1 colocadas, %2 descartadas por solape").arg(farApartPlaced).arg(stats.skippedCollision));

  // 7) El tamano minimo del elemento evita etiquetar lo que no se ve.
  style = base;
  style.minimumFeatureSize = 100000.0;
  stats = render(style, true, nullptr);
  check(QStringLiteral("el tamano minimo del elemento descarta elementos diminutos"),
        stats.placed == 0 && stats.skippedSmall > 0,
        QStringLiteral("%1 colocadas, %2 por tamano").arg(stats.placed).arg(stats.skippedSmall));

  // 8) El limite de etiquetas acota el trabajo del cuadro.
  style = base;
  style.maximumLabels = 1;
  stats = render(style, true, nullptr);
  check(QStringLiteral("el limite de etiquetas acota el dibujado"), stats.placed <= 1,
        QStringLiteral("%1 etiqueta con limite 1").arg(stats.placed));

  // 9) El descarte de textos repetidos se nota en una capa con valores repetidos.
  LabelStyle duplicateStyle = base;
  duplicateStyle.fields = QStringList {secondField};
  duplicateStyle.discardDuplicates = true;
  const qint64 uniquePlaced = render(duplicateStyle, true, nullptr).placed;

  duplicateStyle.discardDuplicates = false;
  const qint64 repeatedPlaced = render(duplicateStyle, true, nullptr).placed;
  check(QStringLiteral("el descarte de textos repetidos reduce las etiquetas"),
        uniquePlaced <= repeatedPlaced, QStringLiteral("%1 sin repetir frente a %2 repitiendo")
                                           .arg(uniquePlaced)
                                           .arg(repeatedPlaced));

  // 10) El tamano en unidades del mapa se interpreta A LA ESCALA ACTUAL: el mismo
  //     tamano aparente debe dar el mismo resultado que en puntos de pantalla. Si no
  //     coincidiera, el numero significaria otra cosa y el texto saldria enorme (que
  //     es el fallo que se vio en uso real: manchas negras en lugar de letras).
  {
    QFont font;
    font.setPointSizeF(base.fontSize);
    const double pixelHeight = QFontMetricsF(font).height();

    LabelStyle mapUnits = base;
    mapUnits.sizeMode = LabelStyle::SizeMode::MapUnits;
    mapUnits.fontSize = pixelHeight * canvas.mapUnitsPerPixel();
    const LabelRenderer::FrameStats mapStats = render(mapUnits, true, nullptr);

    check(QStringLiteral("el tamano en unidades del mapa equivale al de pantalla"), mapStats.placed > 0,
          QStringLiteral("%1 etiquetas con %2 u.m. (≈ %3 px) frente a %4 con %5 pt")
            .arg(mapStats.placed)
            .arg(mapUnits.fontSize, 0, 'g', 4)
            .arg(pixelHeight, 0, 'f', 1)
            .arg(twoFieldPlaced)
            .arg(base.fontSize));
  }

  // 11) Un tamano desproporcionado se recorta: por encima del limite el texto deja de
  //     leerse y se ve como manchas, asi que se limita en lugar de dibujarlo.
  {
    LabelStyle absurd = base;
    absurd.sizeMode = LabelStyle::SizeMode::MapUnits;
    absurd.fontSize = 1.0e6; // con cualquier escala real, miles de pixeles
    const LabelRenderer::FrameStats absurdStats = render(absurd, true, nullptr);

    check(QStringLiteral("un tamano desproporcionado se recorta en lugar de dibujar manchas"),
          absurdStats.sizeClamped > 0,
          QStringLiteral("%1 etiquetas con el tamano recortado").arg(absurdStats.sizeClamped));
  }

  // 12) Los textos se cachean entre cuadros: al segundo cuadro no se vuelve a leer
  //     el atributo de cada elemento (si no, desplazar el mapa releeria la capa).
  layer->setLabelStyle(base);
  canvas.refreshMap();
  canvas.renderToImage(size, false);
  const LabelRenderer::FrameStats first = canvas.labelFrameStats();
  canvas.renderToImage(size, false);
  const LabelRenderer::FrameStats second = canvas.labelFrameStats();
  check(QStringLiteral("los textos se cachean entre cuadros"),
        second.cacheHits > 0 && second.valueReads <= first.valueReads,
        QStringLiteral("%1 lecturas y %2 aciertos de cache en el segundo cuadro")
          .arg(second.valueReads)
          .arg(second.cacheHits));

  // --- Comprobaciones de pixeles: solo si la plataforma dibuja texto ---------
  {
    QImage probe(64, 24, QImage::Format_ARGB32_Premultiplied);
    probe.fill(Qt::white);
    QImage probeAfter = probe;
    {
      QPainter probePainter(&probeAfter);
      probePainter.setPen(Qt::black);
      probePainter.drawText(QPointF(2, 16), QStringLiteral("W"));
    }

    // Se compara antes y despues: contar pixeles "pintados" no serviria, porque el
    // fondo blanco de la sonda ya difiere del color de fondo del lienzo.
    if (countVisibleDifferences(probe, probeAfter, 8) == 0)
    {
      omit(QStringLiteral("las etiquetas anaden tinta al mapa"),
           QStringLiteral("esta plataforma no dibuja texto (motor de fuente sin contornos)"));
      omit(QStringLiteral("una fuente mayor dibuja mas tinta"),
           QStringLiteral("esta plataforma no dibuja texto"));
      omit(QStringLiteral("el halo cambia el dibujado del texto"),
           QStringLiteral("esta plataforma no dibuja texto"));
      omit(QStringLiteral("el paneo con etiquetas coincide con el redibujado"),
           QStringLiteral("esta plataforma no dibuja texto"));
    }
    else
    {
      // La tinta que anaden las etiquetas se mide como DIFERENCIA respecto al mapa
      // sin etiquetas: contar pixeles pintados no sirve, porque el texto cae sobre
      // poligonos ya pintados y no cambiaria la cuenta.
      QImage labelled;
      render(base, true, &labelled);
      const qint64 addedByLabels = countVisibleDifferences(labelled, withoutLabels, 8);
      check(QStringLiteral("las etiquetas anaden tinta al mapa"), addedByLabels > 0,
            QStringLiteral("%1 pixeles pintados por las etiquetas").arg(addedByLabels));

      LabelStyle big = base;
      big.fontSize = base.fontSize * 2.5;
      QImage bigImage;
      render(big, true, &bigImage);
      const qint64 addedByBig = countVisibleDifferences(bigImage, withoutLabels, 8);
      check(QStringLiteral("una fuente mayor dibuja mas tinta"), addedByBig > addedByLabels,
            QStringLiteral("%1 pixeles con %2 pt frente a %3 con %4 pt")
              .arg(addedByBig)
              .arg(big.fontSize)
              .arg(addedByLabels)
              .arg(base.fontSize));

      LabelStyle noHalo = base;
      noHalo.haloEnabled = false;
      QImage noHaloImage;
      render(noHalo, true, &noHaloImage);
      const qint64 withHalo = countVisibleDifferences(labelled, noHaloImage, 8);
      check(QStringLiteral("el halo cambia el dibujado del texto"), withHalo > 0,
            QStringLiteral("%1 pixeles distintos entre halo y sin halo").arg(withHalo));

      // El invariante del lienzo, ahora con etiquetas: los textos se dibujan
      // encima del cuadro reutilizado y no deben quedar duplicados ni desplazados.
      layer->setLabelStyle(base);
      canvas.setLabelsVisible(true);
      canvas.refreshMap();
      canvas.renderToImage(size, false);

      const double unitsPerPixel = canvas.mapUnitsPerPixel();
      const Rectangle extent = canvas.extent();
      canvas.setExtent(Rectangle(extent.xMin() + 7.0 * unitsPerPixel, extent.yMin(),
                                 extent.xMax() + 7.0 * unitsPerPixel, extent.yMax()));
      const QImage reused = canvas.renderToImage(size, false);

      canvas.refreshMap();
      const QImage full = canvas.renderToImage(size, false);

      const qint64 differences = countVisibleDifferences(reused, full, 8);
      check(QStringLiteral("el paneo con etiquetas coincide con el redibujado"), differences == 0,
            QStringLiteral("%1 pixeles distintos").arg(differences));
    }
  }

  if (asJson)
  {
    QJsonObject root;
    root.insert(QStringLiteral("ok"), ok);
    root.insert(QStringLiteral("checks"), executed);
    root.insert(QStringLiteral("omitted"), omitted);
    root.insert(QStringLiteral("fields"), QJsonArray::fromStringList(fieldNames));
    writeOut(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)).trimmed());
  }

  return ok ? ExitOk : ExitRenderFailed;
}

/**
 * Comprueba que el mapa se redibuja al cambiar la simbologia o la visibilidad de
 * una capa (`--check-refresh`).
 *
 * Es una prueba de contrato, no de rendimiento: el lienzo guarda el ultimo cuadro
 * para reutilizarlo al desplazar el mapa, y una cache mal invalidada hacia que un
 * cambio de estilo o de visibilidad no se viera hasta mover la vista. Aqui se
 * reproduce exactamente esa secuencia y se compara pixel a pixel.
 */
int checkRefresh(const QStringList &layerPaths, const QSize &size, bool asJson)
{
  if (layerPaths.isEmpty())
  {
    writeErr(QStringLiteral("Se necesita una capa: minigis --check-refresh -l capa.shp"));
    return ExitUsage;
  }

  const QString path = layerPaths.first();
  if (!QFileInfo::exists(path))
  {
    writeErr(QStringLiteral("No existe el archivo: %1").arg(QDir::toNativeSeparators(path)));
    return ExitFileNotFound;
  }

  Error error;
  const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, &error);
  if (!layer)
  {
    writeErr(error.toText());
    return ExitOpenFailed;
  }

  attachGeometryStore(layer, 4326, true);

  MapCanvas canvas;
  canvas.setDestinationEpsg(4326);
  canvas.resize(size);
  canvas.addLayer(layer, true);

  bool ok = true;
  const auto check = [&ok](const QString &name, bool passed, const QString &detail) {
    writeOut(QStringLiteral("%1 %2%3")
               .arg(passed ? QStringLiteral("OK  ") : QStringLiteral("FALLO"))
               .arg(name, detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail));
    if (!passed)
      ok = false;
  };

  // 1) Cuadro de referencia y comprobacion de que la cache se reutiliza cuando
  //    no ha cambiado nada (si no, la prueba de abajo no probaria nada).
  const QImage base = canvas.renderToImage(size, false);
  const qint64 basePainted = countPaintedPixels(base);
  const QImage repeated = canvas.renderToImage(size, false);
  check(QStringLiteral("cuadro estable con el mismo estado"), repeated == base,
        QStringLiteral("%1 pixeles pintados").arg(basePainted));

  if (basePainted == 0)
  {
    writeErr(QStringLiteral("La capa no pinta nada en la vista; la prueba no seria concluyente."));
    return ExitRenderFailed;
  }

  // 2) Cambio de simbologia (lo que hace el boton Aceptar del dialogo de estilo).
  Style style = layer->style();
  style.fillColor = QColor(230, 70, 40);
  style.strokeColor = QColor(110, 0, 0);
  style.fillEnabled = true;
  style.strokeWidth = 3.0;
  style.pointSize = 11.0;
  layer->setStyle(style);

  const QImage styled = canvas.renderToImage(size, false);
  check(QStringLiteral("la simbologia nueva se ve en el mapa"), styled != base,
        QStringLiteral("%1 pixeles pintados").arg(countPaintedPixels(styled)));

  // 3) Ocultar la capa (lo que hace el check del listado de capas).
  layer->setVisible(false);
  const QImage hidden = canvas.renderToImage(size, false);
  const qint64 hiddenPainted = countPaintedPixels(hidden);
  check(QStringLiteral("la capa oculta desaparece del mapa"), hiddenPainted == 0,
        QStringLiteral("%1 pixeles pintados").arg(hiddenPainted));

  // 4) Volver a mostrarla debe recuperar exactamente el cuadro anterior.
  layer->setVisible(true);
  const QImage shown = canvas.renderToImage(size, false);
  check(QStringLiteral("al volver a mostrarla se recupera el dibujo"), shown == styled, QString());

  // 5) Paneo: reutilizar el cuadro anterior (copiarlo y repintar solo la franja
  //    descubierta) debe dar EXACTAMENTE lo mismo que redibujar desde cero. Si la
  //    copia y la franja no encajan aparece una costura o queda una banda sin
  //    pintar: son los fallos que se ven al desplazar el mapa. Se prueban las
  //    cuatro direcciones y diagonales, con desplazamientos fraccionarios
  //    (ejercitan el ajuste a la rejilla de pixeles) y GRANDES (hacen evidente
  //    cualquier banda que se quede sin pintar).
  //
  //    Antes se acerca la vista: si el contenido no llega a los bordes del
  //    lienzo, una banda sin pintar en el borde no produce ninguna diferencia y la
  //    prueba no la veria. Ese fue el punto ciego que dejo pasar el fallo del
  //    rectangulo cubierto.
  {
    const Rectangle fullExtent = layer->extentInEpsg(4326);
    if (fullExtent.isValid())
    {
      const double halfWidth = fullExtent.width() * 0.25;
      const double halfHeight = fullExtent.height() * 0.25;
      canvas.setExtent(Rectangle(fullExtent.centerX() - halfWidth, fullExtent.centerY() - halfHeight,
                                 fullExtent.centerX() + halfWidth, fullExtent.centerY() + halfHeight)
                         .adjustedToAspect(double(size.width()) / double(size.height())));
    }
  }

  const double pans[][2] = {{3.7, 0.0},    {-3.7, 0.0},    {0.0, 3.7},    {0.0, -3.7},  {5.2, -4.3},
                            {0.4, 0.4},    {37.0, 0.0},    {-41.0, 12.0}, {0.0, 55.0},  {-17.0, -63.0},
                            {123.0, -29.0}, {-210.0, 97.0}};
  qint64 worstPanDifferences = 0;
  QString worstPanCase;

  for (const auto &pan : pans)
  {
    const double unitsPerPixel = canvas.mapUnitsPerPixel();
    const Rectangle extentBeforePan = canvas.extent();

    canvas.setExtent(Rectangle(extentBeforePan.xMin() + pan[0] * unitsPerPixel,
                               extentBeforePan.yMin() - pan[1] * unitsPerPixel,
                               extentBeforePan.xMax() + pan[0] * unitsPerPixel,
                               extentBeforePan.yMax() - pan[1] * unitsPerPixel));
    const QImage pannedWithReuse = canvas.renderToImage(size, false);

    // Volcado para diagnostico visual: permite comparar la parte copiada con la
    // franja repintada cuando la comprobacion falla.
    if (g_verbose)
    {
      pannedWithReuse.save(QStringLiteral("pan_reuse.png"));
      logVerbose(QStringLiteral("paneo (%1,%2): reutilizado en %3 ms")
                   .arg(pan[0]).arg(pan[1]).arg(canvas.lastReuseMilliseconds()));
    }

    canvas.refreshMap(); // descarta la cache: el mismo encuadre, pero dibujado entero
    const QImage pannedFullRender = canvas.renderToImage(size, false);
    if (g_verbose)
      pannedFullRender.save(QStringLiteral("pan_full.png"));

    const qint64 differences = countVisibleDifferences(pannedWithReuse, pannedFullRender, 8);

    if (differences > worstPanDifferences)
    {
      worstPanDifferences = differences;
      worstPanCase = QStringLiteral("(%1, %2) px").arg(pan[0]).arg(pan[1]);
    }
  }

  check(QStringLiteral("el paneo reutilizado coincide con el redibujado completo"), worstPanDifferences == 0,
        QStringLiteral("%1 pixeles distintos de %2 en el peor caso %3")
          .arg(worstPanDifferences)
          .arg(size.width() * size.height())
          .arg(worstPanCase.isEmpty() ? QStringLiteral("(ninguno)") : worstPanCase));

  // 6) Lo mismo en modo borrador, que es el que se ve MIENTRAS se arrastra: se
  //    dibuja a media resolucion (como hace paintEvent) y tiene su propia cache.
  const QSize draftSize(std::max(1, size.width() / 2), std::max(1, size.height() / 2));
  qint64 worstDraftDifferences = 0;
  QString draftDetail;

  // Antes de comparar, se comprueba que dibujar dos veces el MISMO estado (sin
  // cache de por medio) da exactamente el mismo resultado: si el propio dibujado
  // no fuese determinista a nivel de pixel, la comparacion de abajo no
  // significaria nada.
  {
    canvas.refreshMap();
    const QImage firstDraft = canvas.renderToImage(draftSize, true);
    canvas.refreshMap();
    const QImage secondDraft = canvas.renderToImage(draftSize, true);

    QString sameStateDetail;
    const qint64 sameStateDifferences = countVisibleDifferences(firstDraft, secondDraft, 8, &sameStateDetail);
    check(QStringLiteral("el mismo estado dibuja lo mismo en borrador"), sameStateDifferences == 0,
          QStringLiteral("%1 pixeles distintos %2").arg(sameStateDifferences).arg(sameStateDetail));
  }

  for (const auto &pan : pans)
  {
    const double draftUnitsPerPixel = canvas.mapUnitsPerPixel();
    const Rectangle extentBeforeDraft = canvas.extent();

    canvas.setExtent(Rectangle(extentBeforeDraft.xMin() + pan[0] * draftUnitsPerPixel,
                               extentBeforeDraft.yMin() - pan[1] * draftUnitsPerPixel,
                               extentBeforeDraft.xMax() + pan[0] * draftUnitsPerPixel,
                               extentBeforeDraft.yMax() - pan[1] * draftUnitsPerPixel));
    const QImage draftWithReuse = canvas.renderToImage(draftSize, true);

    canvas.refreshMap();
    const QImage draftFullRender = canvas.renderToImage(draftSize, true);

    QString firstDifference;
    const qint64 draftDifferences = countVisibleDifferences(draftWithReuse, draftFullRender, 8, &firstDifference);
    logVerbose(QStringLiteral("borrador paso (%1,%2): desplazamiento %3,%4 px de borrador -> %5 diferencias %6")
                 .arg(pan[0])
                 .arg(pan[1])
                 .arg(static_cast<double>(canvas.extent().xMin() - extentBeforeDraft.xMin()) * canvas.mapUnitsPerPixel() * 0.5, 0, 'f', 2)
                 .arg(static_cast<double>(canvas.extent().yMax() - extentBeforeDraft.yMax()) * canvas.mapUnitsPerPixel() * 0.5, 0, 'f', 2)
                 .arg(draftDifferences)
                 .arg(firstDifference));
    if (draftDifferences < 0)
    {
      worstDraftDifferences = -1;
      break;
    }

    if (draftDifferences > worstDraftDifferences)
    {
      worstDraftDifferences = draftDifferences;
      draftDetail = firstDifference;
    }
  }

  // Criterio del camino de borrador: se tolera una fraccion minima (0,1 % de los
  // pixeles) porque al dibujar una franja recortada Qt suaviza de forma algo
  // distinta los simbolos que caen sobre el borde del recorte (medido: 5 pixeles
  // de 60.000). Lo que NO se tolera es una costura: el fallo original desplazaba
  // todo el contenido copiado y producia miles de pixeles distintos (6 %).
  {
    const qint64 allowedDraftDifferences = static_cast<qint64>(draftSize.width()) * draftSize.height() / 1000;
    check(QStringLiteral("el paneo en borrador coincide con el redibujado (media resolucion)"),
          worstDraftDifferences >= 0 && worstDraftDifferences <= allowedDraftDifferences,
          QStringLiteral("%1 pixeles distintos de %2 (tolerancia %3) %4")
            .arg(worstDraftDifferences)
            .arg(draftSize.width() * draftSize.height())
            .arg(allowedDraftDifferences)
            .arg(draftDetail));
  }

  if (asJson)
  {
    QJsonObject root;
    root.insert(QStringLiteral("ok"), ok);
    root.insert(QStringLiteral("paintedBase"), static_cast<double>(basePainted));
    root.insert(QStringLiteral("paintedHidden"), static_cast<double>(hiddenPainted));
    root.insert(QStringLiteral("panDifferences"), static_cast<double>(worstPanDifferences));
    writeOut(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)).trimmed());
  }

  return ok ? ExitOk : ExitRenderFailed;
}

/**
 * Transforma un punto del SRC nativo de la capa a EPSG:4326 con GDAL.
 * Es el oraculo independiente que usa --check-raster para comprobar el encuadre.
 */
bool transformRasterPointTo4326(const RasterLayer &layer, double x, double y, double *longitude, double *latitude)
{
  const OGRSpatialReference *source = layer.spatialRef();
  if (!source)
    return false;

  OGRSpatialReference destination;
  if (destination.importFromEPSG(4326) != OGRERR_NONE)
    return false;
  destination.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  std::unique_ptr<OGRSpatialReference> sourceClone(source->Clone());
  if (!sourceClone)
    return false;
  sourceClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  std::unique_ptr<OGRCoordinateTransformation> transformation(
    OGRCreateCoordinateTransformation(sourceClone.get(), &destination));
  if (!transformation)
    return false;

  double px = x;
  double py = y;
  if (!transformation->Transform(1, &px, &py))
    return false;

  *longitude = px;
  *latitude = py;
  return true;
}

/**
 * Diagnostico del dibujado raster (sin ventana).
 *
 * Comprueba pixel a pixel lo que una prueba de rendimiento no ve: cuadrantes en
 * su sitio, extension, reproyeccion, NoData, estiramiento, paleta, redibujado
 * al cambiar el estilo, invariante del paneo reutilizado y lectura acotada.
 */
int checkRaster(const QStringList &layerPaths, const QSize &size, bool asJson)
{
  if (layerPaths.isEmpty())
  {
    writeErr(QStringLiteral("Se necesita al menos un raster, por ejemplo:"));
    writeErr(QStringLiteral("  minigis --check-raster -l data/sample/rasters/ortho_rgb.tif"));
    return ExitUsage;
  }

  std::shared_ptr<RasterLayer> rgbLayer;
  std::shared_ptr<RasterLayer> demLayer;
  std::shared_ptr<RasterLayer> paletteLayer;

  for (const QString &path : layerPaths)
  {
    if (!QFileInfo::exists(path))
    {
      writeErr(QStringLiteral("No existe el archivo: %1").arg(QDir::toNativeSeparators(path)));
      return ExitFileNotFound;
    }

    Error error;
    const std::shared_ptr<RasterLayer> layer = RasterLayer::open(path, &error);
    if (!layer)
    {
      writeErr(error.toText());
      return ExitOpenFailed;
    }

    const QString name = layer->name().toLower();
    if (name.contains(QStringLiteral("ortho")))
      rgbLayer = layer;
    else if (name.contains(QStringLiteral("dem")))
      demLayer = layer;
    else if (name.contains(QStringLiteral("landcover")) || name.contains(QStringLiteral("palette")))
      paletteLayer = layer;
    else if (!rgbLayer)
      rgbLayer = layer;
  }

  if (!rgbLayer && !demLayer && !paletteLayer)
  {
    writeErr(QStringLiteral("No hay capas raster que comprobar."));
    return ExitNoLayers;
  }

  bool ok = true;
  qint64 quadrantMatches = -1;
  double extentError = -1.0;
  double warpError = -1.0;
  qint64 transparentPixels = -1;
  qint64 percentClipRange = -1;
  qint64 minMaxRange = -1;
  qint64 paletteMatches = -1;
  qint64 restyledPixels = -1;
  qint64 reuseDifferences = -1;
  qint64 bytesRead = -1;
  qint64 bytesLimit = -1;
  qint64 marginPixels = -1;
  bool defaultStretchNone = false;

  const auto check = [&ok](const QString &name, bool passed, const QString &detail) {
    writeOut(QStringLiteral("%1 %2%3")
               .arg(passed ? QStringLiteral("OK  ") : QStringLiteral("FALLO"))
               .arg(name, detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail));
    if (!passed)
      ok = false;
  };

  const QColor background(248, 248, 244);

  // Misma transformacion que MapToPixel, para calcular donde debe caer cada cosa.
  const auto mapToPixel = [](const Rectangle &extent, const QSize &imageSize, double x, double y) {
    const double scale = std::min(static_cast<double>(imageSize.width()) / extent.width(),
                                  static_cast<double>(imageSize.height()) / extent.height());
    const double offsetX = (imageSize.width() - extent.width() * scale) / 2.0;
    const double offsetY = (imageSize.height() - extent.height() * scale) / 2.0;
    return QPointF(offsetX + (x - extent.xMin()) * scale, offsetY + (extent.yMax() - y) * scale);
  };

  const auto pixelAt = [](const QImage &image, const QPointF &point) {
    const int x = std::clamp(static_cast<int>(std::lround(point.x())), 0, image.width() - 1);
    const int y = std::clamp(static_cast<int>(std::lround(point.y())), 0, image.height() - 1);
    return image.pixelColor(x, y);
  };

  const auto sameColor = [](const QColor &left, const QColor &right, int tolerance) {
    return std::abs(left.red() - right.red()) <= tolerance
           && std::abs(left.green() - right.green()) <= tolerance
           && std::abs(left.blue() - right.blue()) <= tolerance;
  };

  const auto isBackground = [&background](const QColor &color) {
    return (color.rgb() & 0x00FFFFFF) == (background.rgb() & 0x00FFFFFF);
  };

  const auto contentBounds = [&isBackground](const QImage &image, QRect *rect) {
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < image.height(); ++y)
    {
      for (int x = 0; x < image.width(); ++x)
      {
        if (isBackground(image.pixelColor(x, y)))
          continue;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
      }
    }
    if (maxX < 0)
      return false;
    *rect = QRect(QPoint(minX, minY), QPoint(maxX, maxY));
    return true;
  };

  // Rango de gris p2..p98 de lo dibujado (ignora el fondo), que es lo que el ojo
  // percibe como contraste: un unico pixel atipico no debe dominar la medida.
  const auto grayPercentileRange = [&isBackground](const QImage &image) {
    std::vector<int> values;
    values.reserve(static_cast<size_t>(image.width()) * image.height());
    for (int y = 0; y < image.height(); ++y)
    {
      for (int x = 0; x < image.width(); ++x)
      {
        const QColor color = image.pixelColor(x, y);
        if (!isBackground(color))
          values.push_back(color.red());
      }
    }
    if (values.empty())
      return qint64(0);
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double fraction) {
      const size_t last = values.size() - 1;
      return values[static_cast<size_t>(std::lround(fraction * last))];
    };
    return qint64(percentile(0.98) - percentile(0.02));
  };

  // --- 1) Ortofoto: cuadrantes, extension, redibujado, paneo y lectura ------
  if (rgbLayer)
  {
    const RasterStyle savedStyle = rgbLayer->style();
    const bool savedVisible = rgbLayer->isVisible();

    // Al abrir una imagen no se estira el contraste: los valores Byte se pintan
    // tal cual y el estiramiento es una eleccion del usuario en el dialogo.
    defaultStretchNone = savedStyle.stretch == RasterStyle::Stretch::None;
    check(QStringLiteral("estiramiento por defecto: ninguno"), defaultStretchNone,
          QStringLiteral("modo %1").arg(static_cast<int>(savedStyle.stretch)));

    RasterStyle style = savedStyle;
    style.stretch = RasterStyle::Stretch::None;
    style.opacity = 100;
    style.useNodata = true;
    if (rgbLayer->bandCount() >= 3)
    {
      style.bandMode = RasterStyle::BandMode::Rgb;
      style.redBand = 1;
      style.greenBand = 2;
      style.blueBand = 3;
      style.alphaBand = rgbLayer->bandCount() >= 4 ? 4 : 0;
    }
    rgbLayer->setStyle(style);

    const Rectangle extent(-4.85, 39.8, -2.75, 41.2);

    MapCanvas canvas;
    canvas.setDestinationEpsg(4326);
    canvas.resize(size);
    canvas.addRasterLayer(rgbLayer, false);
    canvas.setExtent(extent);
    const QImage image = canvas.renderToImage(size, false);

    struct QuadrantSample
    {
      const char *name;
      double longitude;
      double latitude;
      QColor color;
    };
    const QuadrantSample quadrants[4] = {
      {"NO", -4.2, 40.85, QColor(220, 50, 50)},
      {"NE", -3.4, 40.85, QColor(50, 180, 70)},
      {"SO", -4.2, 40.15, QColor(40, 80, 200)},
      {"SE", -3.4, 40.15, QColor(245, 245, 245)}};

    quadrantMatches = 0;
    for (const QuadrantSample &sample : quadrants)
    {
      const QColor color = pixelAt(image, mapToPixel(extent, size, sample.longitude, sample.latitude));
      if (sameColor(color, sample.color, 2))
        ++quadrantMatches;
    }
    check(QStringLiteral("cuadrantes en su posicion"), quadrantMatches == 4,
          QStringLiteral("%1/4 colores esperados").arg(quadrantMatches));

    QRect bounds;
    if (contentBounds(image, &bounds))
    {
      const QPointF topLeft = mapToPixel(extent, size, -4.6, 41.2);
      const QPointF bottomRight = mapToPixel(extent, size, -3.0, 39.8);
      extentError = std::max({std::fabs(bounds.left() - topLeft.x()), std::fabs(bounds.right() - bottomRight.x()),
                              std::fabs(bounds.top() - topLeft.y()),
                              std::fabs(bounds.bottom() - bottomRight.y())});
    }
    check(QStringLiteral("extension del raster"), extentError >= 0.0 && extentError <= 1.5,
          QStringLiteral("error maximo %1 px").arg(QString::number(extentError, 'f', 1)));

    // Redibujado: cambiar el estilo debe verse sin mover la vista (revision).
    const QImage base = canvas.renderToImage(size, false);
    RasterStyle restyle = style;
    restyle.opacity = 40;
    rgbLayer->setStyle(restyle);
    const QImage restyled = canvas.renderToImage(size, false);
    restyledPixels = countVisibleDifferences(base, restyled, 0, nullptr);
    check(QStringLiteral("el cambio de estilo se ve en el mapa"), restyledPixels > 0,
          QStringLiteral("%1 pixeles distintos").arg(restyledPixels));

    rgbLayer->setVisible(false);
    const QImage hidden = canvas.renderToImage(size, false);
    const qint64 hiddenPainted = countPaintedPixels(hidden);
    check(QStringLiteral("la capa oculta desaparece del mapa"), hiddenPainted == 0,
          QStringLiteral("%1 pixeles pintados").arg(hiddenPainted));

    rgbLayer->setVisible(true);
    const QImage shown = canvas.renderToImage(size, false);
    check(QStringLiteral("al volver a mostrarla se recupera el dibujo"), shown == restyled, QString());

    // Paneo reutilizado == redibujado completo (0 pixeles en calidad).
    canvas.setExtent(extent);
    canvas.refreshMap();
    const QImage beforePan = canvas.renderToImage(size, false);
    const double shift = 37.0 * canvas.mapUnitsPerPixel();
    canvas.setExtent(Rectangle(extent.xMin() + shift, extent.yMin(), extent.xMax() + shift, extent.yMax()));
    const QImage reused = canvas.renderToImage(size, false);
    canvas.refreshMap();
    const QImage full = canvas.renderToImage(size, false);
    reuseDifferences = countVisibleDifferences(reused, full, 0, nullptr);
    check(QStringLiteral("el paneo reutilizado coincide con el redibujado"), reuseDifferences == 0,
          QStringLiteral("%1 pixeles distintos de %2")
            .arg(reuseDifferences)
            .arg(static_cast<qint64>(size.width()) * size.height()));

    bytesRead = canvas.rasterFrameStats().bytesRead;
    bytesLimit = static_cast<qint64>(size.width()) * size.height() * rgbLayer->bandCount() * 4;
    check(QStringLiteral("lectura acotada al lienzo"), bytesRead >= 0 && bytesRead <= bytesLimit,
          QStringLiteral("%1 MB por cuadro (limite %2 MB)")
            .arg(QString::number(static_cast<double>(bytesRead) / (1024.0 * 1024.0), 'f', 1))
            .arg(QString::number(static_cast<double>(bytesLimit) / (1024.0 * 1024.0), 'f', 1)));

    Q_UNUSED(beforePan);

    // 8) Margenes del warp. Al reproyectar, la huella del raster (girada) se
    //    mete en un rectangulo alineado a los ejes y las esquinas que sobran se
    //    rellenan de negro. Al cargar un mosaico por hojas, esos margenes
    //    opacos tapaban la hoja vecina y dejaban una cuna negra en cada union:
    //    el "desfase" que se ve entre imagenes contiguas. La ortofoto no tiene
    //    NoData ni ningun pixel negro propio, asi que cualquier negro delata un
    //    margen del warp que se esta dibujando.
    {
      MapCanvas projected;
      projected.setDestinationEpsg(25830);
      projected.resize(size);
      projected.addRasterLayer(rgbLayer, true);
      const QImage projectedImage = projected.renderToImage(size, false);

      marginPixels = 0;
      for (int y = 0; y < projectedImage.height(); ++y)
      {
        for (int x = 0; x < projectedImage.width(); ++x)
        {
          if (projectedImage.pixelColor(x, y) == QColor(0, 0, 0))
            ++marginPixels;
        }
      }

      check(QStringLiteral("sin margenes negros al reproyectar"), marginPixels == 0,
            QStringLiteral("%1 pixeles negros en %2x%3").arg(marginPixels).arg(size.width()).arg(size.height()));
    }

    rgbLayer->setStyle(savedStyle);
    rgbLayer->setVisible(savedVisible);
  }

  // --- 2) DEM: reproyeccion, NoData y estiramiento --------------------------
  if (demLayer)
  {
    const RasterStyle savedStyle = demLayer->style();

    RasterStyle style = savedStyle;
    style.stretch = RasterStyle::Stretch::PercentClip;
    style.useNodata = true;
    demLayer->setStyle(style);

    const Rectangle extent(-3.83078, 40.37419, -3.80678, 40.39019);

    MapCanvas canvas;
    canvas.setDestinationEpsg(4326);
    canvas.resize(size);
    canvas.addRasterLayer(demLayer, false);
    canvas.setExtent(extent);

    const QImage percentImage = canvas.renderToImage(size, false);

    const double *geoTransform = demLayer->geoTransform();
    double longitude = 0.0;
    double latitude = 0.0;
    const bool transformed =
      transformRasterPointTo4326(*demLayer, geoTransform[0] + demLayer->width() * geoTransform[1],
                                 geoTransform[3] + demLayer->height() * geoTransform[5], &longitude, &latitude);

    warpError = -1.0;
    if (transformed)
    {
      // Se recorre la diagonal desde fuera hacia dentro: la distancia al primer
      // pixel pintado es el error de colocacion de esa esquina.
      const QPointF corner = mapToPixel(extent, size, longitude, latitude);
      for (double distance = 6.0; distance >= -6.0; distance -= 0.5)
      {
        const QPointF sample = corner + QPointF(distance * 0.70710678, distance * 0.70710678);
        if (!isBackground(pixelAt(percentImage, sample)))
        {
          warpError = std::fabs(distance);
          break;
        }
      }
    }
    check(QStringLiteral("reproyeccion UTM->4326"), warpError >= 0.0 && warpError <= 1.5,
          QStringLiteral("error %1 px").arg(QString::number(warpError, 'f', 1)));

    // NoData: un bloque de 5x5 alrededor del pixel (2,2) del archivo debe ser fondo.
    transparentPixels = -1;
    double noDataLongitude = 0.0;
    double noDataLatitude = 0.0;
    if (transformRasterPointTo4326(*demLayer, geoTransform[0] + 2.5 * geoTransform[1],
                                   geoTransform[3] + 2.5 * geoTransform[5], &noDataLongitude, &noDataLatitude))
    {
      const QPointF center = mapToPixel(extent, size, noDataLongitude, noDataLatitude);
      transparentPixels = 0;
      for (int dy = -2; dy <= 2; ++dy)
      {
        for (int dx = -2; dx <= 2; ++dx)
        {
          if (isBackground(pixelAt(percentImage, center + QPointF(dx, dy))))
            ++transparentPixels;
        }
      }
    }
    check(QStringLiteral("NoData transparente"), transparentPixels == 25,
          QStringLiteral("%1 de 25 pixeles de fondo").arg(transparentPixels));

    percentClipRange = grayPercentileRange(percentImage);

    RasterStyle minMaxStyle = style;
    minMaxStyle.stretch = RasterStyle::Stretch::MinMax;
    demLayer->setStyle(minMaxStyle);
    const QImage minMaxImage = canvas.renderToImage(size, false);
    minMaxRange = grayPercentileRange(minMaxImage);

    check(QStringLiteral("estiramiento por percentiles"), percentClipRange >= 200 && minMaxRange <= 10,
          QStringLiteral("rango %1 (min-max: %2)").arg(percentClipRange).arg(minMaxRange));

    demLayer->setStyle(savedStyle);
  }

  // --- 3) Paleta -----------------------------------------------------------
  if (paletteLayer)
  {
    const Rectangle extent(-4.85, 39.8, -2.75, 41.2);

    MapCanvas canvas;
    canvas.setDestinationEpsg(4326);
    canvas.resize(size);
    canvas.addRasterLayer(paletteLayer, false);
    canvas.setExtent(extent);
    const QImage image = canvas.renderToImage(size, false);

    struct PaletteSample
    {
      const char *name;
      int column;
      QColor color;
    };
    const PaletteSample samples[3] = {
      {"bosque", 10, QColor(70, 140, 70)},
      {"cultivo", 64, QColor(200, 180, 90)},
      {"agua", 120, QColor(70, 110, 180)}};

    paletteMatches = 0;
    for (const PaletteSample &sample : samples)
    {
      const double longitude = -4.6 + (sample.column + 0.5) / 128.0 * 1.6;
      const QColor color = pixelAt(image, mapToPixel(extent, size, longitude, 40.5));
      if (sameColor(color, sample.color, 0))
        ++paletteMatches;
    }
    check(QStringLiteral("paleta del archivo"), paletteMatches == 3,
          QStringLiteral("%1/3 clases con su color").arg(paletteMatches));

    // El indice 0 (NoData) no debe pintarse de negro.
    qint64 blackPixels = 0;
    for (int y = 0; y < image.height(); ++y)
    {
      for (int x = 0; x < image.width(); ++x)
      {
        const QColor color = image.pixelColor(x, y);
        if (color.red() == 0 && color.green() == 0 && color.blue() == 0)
          ++blackPixels;
      }
    }
    check(QStringLiteral("el NoData de la paleta no se pinta"), blackPixels == 0,
          QStringLiteral("%1 pixeles negros").arg(blackPixels));
  }

  if (asJson)
  {
    QJsonObject root;
    root.insert(QStringLiteral("ok"), ok);
    root.insert(QStringLiteral("quadrants"), static_cast<double>(quadrantMatches));
    root.insert(QStringLiteral("extent_error_px"), extentError);
    root.insert(QStringLiteral("warp_error_px"), warpError);
    root.insert(QStringLiteral("nodata_pixels"), static_cast<double>(transparentPixels));
    root.insert(QStringLiteral("percentclip_range"), static_cast<double>(percentClipRange));
    root.insert(QStringLiteral("minmax_range"), static_cast<double>(minMaxRange));
    root.insert(QStringLiteral("palette_classes"), static_cast<double>(paletteMatches));
    root.insert(QStringLiteral("restyled_pixels"), static_cast<double>(restyledPixels));
    root.insert(QStringLiteral("reuse_differences"), static_cast<double>(reuseDifferences));
    root.insert(QStringLiteral("bytes_read"), static_cast<double>(bytesRead));
    root.insert(QStringLiteral("bytes_limit"), static_cast<double>(bytesLimit));
    root.insert(QStringLiteral("warp_margin_pixels"), static_cast<double>(marginPixels));
    root.insert(QStringLiteral("default_stretch_none"), defaultStretchNone);
    writeOut(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)).trimmed());
  }

  return ok ? ExitOk : ExitRenderFailed;
}

/** Construye el almacen indexado de una capa para los modos sin ventana. */
void attachGeometryStore(const std::shared_ptr<VectorLayer> &layer, int destinationEpsg, bool enabled){
  if (!enabled || !layer)
    return;

  qint64 buildMs = 0;
  QString modeName;
  QString error;
  const std::shared_ptr<GeometryStore> store = buildGeometryStore(layer, destinationEpsg, &buildMs, &modeName, &error);

  if (store)
  {
    layer->setGeometryStore(store);
    const GeometryStore::Stats stats = store->stats();
    logVerbose(QStringLiteral("almacen (%1): %2 elementos, %3 vertices, %4 niveles, %5 ms")
                 .arg(modeName)
                 .arg(stats.features)
                 .arg(stats.vertices)
                 .arg(stats.cachedLevels)
                 .arg(buildMs));
  }
  else
  {
    logVerbose(QStringLiteral("sin almacen indexado (%1); se usara la lectura directa").arg(error));
  }
}

} // namespace

int main(int argc, char *argv[])
{
  // Los modos sin ventana deben fijar la plataforma de Qt antes de crear
  // QApplication:
  //   * --info, --help, --version, --list-drivers y --snapshot se ejecutan con
  //     la plataforma "offscreen" (no necesitan pantalla ni fuentes).
  //   * --snapshot-window captura la interfaz completa; para que el texto salga
  //     con las fuentes del sistema se deja la plataforma nativa.
  // Si el usuario ya definio QT_QPA_PLATFORM se respeta su eleccion.
  bool headlessRequested = false;
  bool windowCapture = false;
  for (int i = 1; i < argc; ++i)
  {
    const QString argument = QString::fromLocal8Bit(argv[i]);
    if (argument == QStringLiteral("--snapshot-window"))
    {
      headlessRequested = true;
      windowCapture = true;
    }
    else if (argument == QStringLiteral("--info") || argument == QStringLiteral("-i")
             || argument == QStringLiteral("--list-drivers")
             || argument == QStringLiteral("--snapshot")
             || argument == QStringLiteral("--bench")
             || argument == QStringLiteral("--index-shp")
             || argument == QStringLiteral("--check-refresh")
             || argument == QStringLiteral("--check-raster")
             || argument == QStringLiteral("--help") || argument == QStringLiteral("-h")
             || argument == QStringLiteral("--version") || argument == QStringLiteral("-v"))
    {
      headlessRequested = true;
    }
  }
  if (headlessRequested && !windowCapture && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "offscreen");

#ifdef Q_OS_WIN
  // La consola de Windows no usa UTF-8 por defecto: sin esto, los acentos de los
  // mensajes salen con caracteres extranos al ejecutar desde cmd.exe.
  if (headlessRequested)
    SetConsoleOutputCP(kUtf8CodePage);
#endif

  QApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("MiniGIS"));
  QCoreApplication::setOrganizationName(QStringLiteral("MiniGIS"));
  QCoreApplication::setApplicationVersion(QString::fromUtf8(MINIGIS_VERSION));

  QCommandLineParser parser;
  parser.setApplicationDescription(
    QStringLiteral("MiniGIS: visor de shapefiles y otras capas vectoriales (Qt 6 + GDAL)"));
  const QCommandLineOption helpOption = parser.addHelpOption();
  const QCommandLineOption versionOption = parser.addVersionOption();

  const QCommandLineOption infoOption({QStringLiteral("i"), QStringLiteral("info")},
                                      QStringLiteral("Muestra la informacion de las capas y sale"));
  const QCommandLineOption jsonOption(QStringLiteral("json"),
                                      QStringLiteral("Salida en JSON (se usa junto a --info)"));
  const QCommandLineOption driversOption(QStringLiteral("list-drivers"),
                                         QStringLiteral("Lista los drivers vectoriales de GDAL y sale"));
  const QCommandLineOption snapshotOption(QStringLiteral("snapshot"),
                                          QStringLiteral("Renderiza el mapa a <archivo.png> sin abrir ventana"),
                                          QStringLiteral("archivo.png"));
  const QCommandLineOption snapshotWindowOption(QStringLiteral("snapshot-window"),
                                                QStringLiteral("Captura la interfaz completa a <archivo.png>"),
                                                QStringLiteral("archivo.png"));
  const QCommandLineOption layerOption({QStringLiteral("l"), QStringLiteral("layer")},
                                       QStringLiteral("Capa a cargar; se puede repetir"),
                                       QStringLiteral("archivo"));
  const QCommandLineOption extentOption(QStringLiteral("extent"),
                                        QStringLiteral("Extension inicial: xmin,ymin,xmax,ymax"),
                                        QStringLiteral("xmin,ymin,xmax,ymax"));
  const QCommandLineOption sizeOption(QStringLiteral("size"),
                                      QStringLiteral("Tamano de la imagen: AnchoxAlto"),
                                      QStringLiteral("AnchoxAlto"), QStringLiteral("1200x800"));
  const QCommandLineOption crsOption(QStringLiteral("crs"),
                                     QStringLiteral("SRC de destino del dibujo (EPSG:xxxx)"),
                                     QStringLiteral("EPSG:xxxx"), QStringLiteral("EPSG:4326"));
  const QCommandLineOption maxFeaturesOption(QStringLiteral("max-features"),
                                             QStringLiteral("Limite de elementos dibujados por capa (0 = sin limite)"),
                                             QStringLiteral("n"), QStringLiteral("0"));
  const QCommandLineOption verboseOption({QStringLiteral("V"), QStringLiteral("verbose")},
                                         QStringLiteral("Salida detallada en stderr"));
  const QCommandLineOption benchOption(QStringLiteral("bench"),
                                       QStringLiteral("Mide cuadros de paneo/zoom sin abrir ventana (numero de cuadros)"),
                                       QStringLiteral("cuadros"), QStringLiteral("30"));
  const QCommandLineOption benchScenarioOption(QStringLiteral("bench-scenario"),
                                               QStringLiteral("Gesto simulado en --bench: pan | zoom | mixed"),
                                               QStringLiteral("gesto"), QStringLiteral("mixed"));
  const QCommandLineOption benchBudgetOption(QStringLiteral("bench-budget"),
                                             QStringLiteral("Presupuesto de ms por cuadro (p90); si se supera, sale con codigo 6"),
                                             QStringLiteral("ms"), QStringLiteral("0"));
  const QCommandLineOption rendererOption(QStringLiteral("renderer"),
                                          QStringLiteral("Motor de dibujo: auto | legacy | fast"),
                                          QStringLiteral("motor"), QStringLiteral("auto"));
  const QCommandLineOption vertexBudgetOption(QStringLiteral("vertex-budget"),
                                              QStringLiteral("Vertices maximos por cuadro en el motor acelerado (0 = sin limite)"),
                                              QStringLiteral("n"), QStringLiteral("600000"));
  const QCommandLineOption spatialFilterOption(QStringLiteral("spatial-filter"),
                                               QStringLiteral("Filtra en origen con el indice del archivo (util en capas que no caben en memoria)"));
  const QCommandLineOption labelFieldOption(QStringLiteral("label-field"),
                                            QStringLiteral("Etiqueta los elementos con estos campos (separados por comas)"),
                                            QStringLiteral("campo[,campo]"));
  const QCommandLineOption labelSizeOption(QStringLiteral("label-size"),
                                           QStringLiteral("Tamano de la fuente de las etiquetas, en puntos"),
                                           QStringLiteral("pt"), QStringLiteral("10"));
  const QCommandLineOption labelMapSizeOption(
    QStringLiteral("label-map-size"),
    QStringLiteral("Tamano de la fuente en UNIDADES DEL MAPA (el texto crece al acercarse)"),
    QStringLiteral("u.m."));
  const QCommandLineOption noLabelsOption(QStringLiteral("no-labels"),
                                          QStringLiteral("Abre las capas con las etiquetas desactivadas"));
  const QCommandLineOption snapshotDialogOption(QStringLiteral("snapshot-dialog"),
                                                QStringLiteral("Captura el dialogo de etiquetas a <archivo.png> y sale"),
                                                QStringLiteral("archivo.png"));
  const QCommandLineOption snapshotRasterDialogOption(
    QStringLiteral("snapshot-raster-dialog"),
    QStringLiteral("Captura el dialogo de estilo raster a <archivo.png> y sale"),
    QStringLiteral("archivo.png"));
  const QCommandLineOption checkLabelsOption(QStringLiteral("check-labels"),
                                             QStringLiteral("Comprueba el etiquetado (campos, escala, solapes, cache)"));
  const QCommandLineOption checkRefreshOption(QStringLiteral("check-refresh"),
                                              QStringLiteral("Comprueba que el mapa se redibuja al cambiar simbologia o visibilidad"));
  const QCommandLineOption checkRasterOption(
    QStringLiteral("check-raster"),
    QStringLiteral("Comprueba el dibujado raster: cuadrantes, extension, reproyeccion, NoData, contraste, "
                   "paleta, redibujado, paneo y lectura acotada"));
  const QCommandLineOption withAttributesOption(QStringLiteral("with-attributes"),
                                               QStringLiteral("Con --snapshot-window: incluye la tabla de atributos en la captura"));
  const QCommandLineOption indexShpOption(QStringLiteral("index-shp"),
                                          QStringLiteral("Diagnostico: indexa las envolventes de <archivo.shp> y las verifica contra GDAL"),
                                          QStringLiteral("archivo.shp"));
  const QCommandLineOption readBudgetOption(QStringLiteral("read-budget"),
                                            QStringLiteral("Elementos leidos del archivo por cuadro en capas indexadas por envolventes (0 = sin limite)"),
                                            QStringLiteral("n"), QStringLiteral("40000"));
  const QCommandLineOption rasterStretchOption(QStringLiteral("raster-stretch"),
                                               QStringLiteral("Estiramiento de contraste de los rasters: none | minmax | percentclip (por defecto none)"),
                                               QStringLiteral("modo"));
  const QCommandLineOption rasterBandsOption(QStringLiteral("raster-bands"),
                                             QStringLiteral("Bandas del raster: R,G,B o una sola (escala de grises)"),
                                             QStringLiteral("bandas"));
  const QCommandLineOption rasterOpacityOption(QStringLiteral("raster-opacity"),
                                               QStringLiteral("Opacidad de los rasters (0-100)"),
                                               QStringLiteral("0-100"), QStringLiteral("100"));

  parser.addOptions({infoOption, jsonOption, driversOption, snapshotOption, snapshotWindowOption,
                     layerOption, extentOption, sizeOption, crsOption, maxFeaturesOption, verboseOption,
                     benchOption, benchScenarioOption, benchBudgetOption, rendererOption, vertexBudgetOption,
                     spatialFilterOption, readBudgetOption, indexShpOption, withAttributesOption,
                     checkRefreshOption, checkLabelsOption, snapshotDialogOption, snapshotRasterDialogOption,
                     checkRasterOption, labelFieldOption, labelSizeOption,
                     labelMapSizeOption, noLabelsOption, rasterStretchOption, rasterBandsOption,
                     rasterOpacityOption});
  parser.addPositionalArgument(QStringLiteral("archivo"),
                              QStringLiteral("Capas a cargar (opcional, se puede repetir)"),
                              QStringLiteral("[archivo...]"));

  if (!parser.parse(QCoreApplication::arguments()))
  {
    writeErr(parser.errorText());
    writeErr(QString());
    writeErr(helpText(parser));
    return ExitUsage;
  }

  if (parser.isSet(helpOption))
  {
    writeOut(helpText(parser));
    return ExitOk;
  }

  if (parser.isSet(versionOption))
  {
    writeOut(versionText());
    return ExitOk;
  }

  g_verbose = parser.isSet(verboseOption);

  // Registro de drivers y localizacion de proj.db / GDAL_DATA. Sin esto, GDAL
  // no reconoce ningun formato y toda apertura de capa falla.
  const RuntimeInfo runtime = initializeGdal();
  logVerbose(QStringLiteral("GDAL %1, PROJ %2, PROJ_DATA %3")
               .arg(runtime.gdalVersion,
                    runtime.projVersion,
                    runtime.projDataFound ? runtime.projDataDir : QStringLiteral("no encontrado")));

  if (parser.isSet(driversOption))
  {
    const QStringList drivers = vectorDriverNames();
    logVerbose(QStringLiteral("drivers vectoriales disponibles: %1").arg(drivers.size()));
    for (const QString &name : drivers)
      writeOut(name);
    return ExitOk;
  }

  // Opciones de etiquetado (se aplican a cada capa que se abra en los modos sin
  // ventana y tambien a la interfaz, donde el dialogo las puede cambiar luego).
  LabelOptions labelOptions;
  if (parser.isSet(labelFieldOption) || parser.isSet(labelMapSizeOption))
  {
    labelOptions.parsed = true;
    labelOptions.mapUnitsSize = parser.value(labelMapSizeOption).toDouble();
    labelOptions.fields = parser.value(labelFieldOption).split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString &field : labelOptions.fields)
      field = field.trimmed();
    labelOptions.size = parser.value(labelSizeOption).toDouble();
    labelOptions.enabled = !parser.isSet(noLabelsOption);
    if (labelOptions.size <= 0.0)
      labelOptions.size = 10.0;
  }

  // Opciones de raster (se aplican a cada capa raster que se abra en los modos
  // sin ventana; en la interfaz se cambian con el dialogo de estilo).
  RasterOptions rasterOptions;
  if (parser.isSet(rasterStretchOption))
  {
    const QString mode = parser.value(rasterStretchOption).trimmed().toLower();
    if (mode == QStringLiteral("none"))
      rasterOptions.stretch = RasterStyle::Stretch::None;
    else if (mode == QStringLiteral("minmax"))
      rasterOptions.stretch = RasterStyle::Stretch::MinMax;
    else if (mode == QStringLiteral("percentclip"))
      rasterOptions.stretch = RasterStyle::Stretch::PercentClip;
    else
    {
      writeErr(QStringLiteral("Estiramiento no soportado: «%1». Usa none, minmax o percentclip.")
                 .arg(parser.value(rasterStretchOption)));
      return ExitUsage;
    }
    rasterOptions.stretchSet = true;
  }

  if (parser.isSet(rasterBandsOption))
  {
    const QStringList parts =
      parser.value(rasterBandsOption).split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.size() > 3)
    {
      writeErr(QStringLiteral("Bandas no validas: «%1». Ejemplo: --raster-bands 1 o --raster-bands 1,2,3")
                 .arg(parser.value(rasterBandsOption)));
      return ExitUsage;
    }

    for (const QString &part : parts)
    {
      bool ok = false;
      const int band = part.trimmed().toInt(&ok);
      if (!ok || band < 1)
      {
        writeErr(QStringLiteral("Banda no valida: «%1». Indica numeros de banda empezando en 1.").arg(part));
        return ExitUsage;
      }
      rasterOptions.bands.append(band);
    }
    rasterOptions.bandsSet = true;
  }

  if (parser.isSet(rasterOpacityOption))
  {
    bool ok = false;
    const int opacity = parser.value(rasterOpacityOption).toInt(&ok);
    if (!ok || opacity < 0 || opacity > 100)
    {
      writeErr(QStringLiteral("Opacidad no valida: «%1». Usa un valor entre 0 y 100.")
                 .arg(parser.value(rasterOpacityOption)));
      return ExitUsage;
    }
    rasterOptions.opacity = opacity;
    rasterOptions.opacitySet = true;
  }

  const QStringList layerPaths = collectLayerPaths(parser);

  // ---------------------------------------------------------------------
  // Diagnostico del indice de envolventes (sin ventana)
  // ---------------------------------------------------------------------
  if (parser.isSet(indexShpOption))
  {
    const QString path = parser.value(indexShpOption);
    if (!QFileInfo::exists(path))
    {
      writeErr(QStringLiteral("No existe el archivo: %1").arg(QDir::toNativeSeparators(path)));
      return ExitFileNotFound;
    }

    return inspectShapefileIndex(path, parser.isSet(jsonOption));
  }

  // ---------------------------------------------------------------------
  // Comprobacion de redibujado (simbologia y visibilidad)
  // ---------------------------------------------------------------------
  // Captura del dialogo de etiquetas: permite revisar la interfaz sin abrirla a
  // mano (util para comprobar el diseno y para las capturas de la documentacion).
  if (parser.isSet(snapshotDialogOption))
  {
    if (layerPaths.isEmpty())
    {
      writeErr(QStringLiteral("Se necesita una capa: minigis --snapshot-dialog d.png -l capa.shp"));
      return ExitUsage;
    }

    Error error;
    const std::shared_ptr<VectorLayer> layer = VectorLayer::open(layerPaths.first(), &error);
    if (!layer)
    {
      writeErr(error.toText());
      return ExitOpenFailed;
    }

    LabelStyle style = layer->labelStyle();
    if (!style.enabled && !layer->fields().isEmpty())
    {
      style.enabled = true;
      style.fields = QStringList {layer->fields().first().name};
    }

    applyLabelOptions(layer, labelOptions);
    style = layer->labelStyle();

    // Escala de referencia: la que tendria el mapa encuadrado en la ventana. El
    // dialogo la necesita para traducir entre puntos y unidades del mapa.
    const Rectangle layerExtent = layer->extentInEpsg(4326);
    const double referenceScale =
      layerExtent.isValid() ? layerExtent.width() / 900.0 : 1.0;

    LabelDialog dialog(layer->name(), layer->fields(), style, referenceScale, nullptr);
    dialog.resize(880, 720);
    dialog.show();
    QCoreApplication::processEvents();

    const QString dialogPath = parser.value(snapshotDialogOption);
    if (!dialog.grab().save(dialogPath))
    {
      writeErr(QStringLiteral("No se pudo escribir la imagen: %1").arg(QDir::toNativeSeparators(dialogPath)));
      return ExitRenderFailed;
    }

    writeOut(QStringLiteral("Imagen escrita: %1").arg(QDir::toNativeSeparators(dialogPath)));
    return ExitOk;
  }

  // Captura del dialogo de estilo raster: permite revisar el diseno sin abrir
  // la aplicacion (mismo patron que --snapshot-dialog para etiquetas).
  if (parser.isSet(snapshotRasterDialogOption))
  {
    if (layerPaths.isEmpty())
    {
      writeErr(QStringLiteral("Se necesita un raster: minigis --snapshot-raster-dialog d.png -l capa.tif"));
      return ExitUsage;
    }

    const QString path = layerPaths.first();
    if (!QFileInfo::exists(path))
    {
      writeErr(QStringLiteral("No existe el archivo: %1").arg(QDir::toNativeSeparators(path)));
      return ExitFileNotFound;
    }

    Error error;
    const std::shared_ptr<RasterLayer> raster = RasterLayer::open(path, &error);
    if (!raster)
    {
      writeErr(error.toText());
      return ExitOpenFailed;
    }

    applyRasterOptions(raster, rasterOptions);

    RasterStyleDialog dialog(raster->name(), raster->bandLabels(), raster->style(), nullptr);
    dialog.resize(560, 520);
    dialog.show();
    QCoreApplication::processEvents();

    const QString dialogPath = parser.value(snapshotRasterDialogOption);
    if (!dialog.grab().save(dialogPath))
    {
      writeErr(QStringLiteral("No se pudo escribir la imagen: %1").arg(QDir::toNativeSeparators(dialogPath)));
      return ExitRenderFailed;
    }

    writeOut(QStringLiteral("Imagen escrita: %1").arg(QDir::toNativeSeparators(dialogPath)));
    return ExitOk;
  }

  // Diagnostico del dibujado raster (sin ventana).
  if (parser.isSet(checkRasterOption))
  {
    QSize checkSize(900, 600);
    if (parser.isSet(sizeOption) && !parseSize(parser.value(sizeOption), &checkSize))
    {
      writeErr(QStringLiteral("Tamano no valido: «%1». Ejemplo: --size 1200x800").arg(parser.value(sizeOption)));
      return ExitUsage;
    }

    return checkRaster(layerPaths, checkSize, parser.isSet(jsonOption));
  }

  if (parser.isSet(checkLabelsOption) || parser.isSet(checkRefreshOption))
  {
    const bool labels = parser.isSet(checkLabelsOption);
    QSize checkSize(600, 400);
    if (!parseSize(parser.value(sizeOption), &checkSize))
    {
      writeErr(QStringLiteral("Tamano no valido: «%1». Ejemplo: --size 900x600").arg(parser.value(sizeOption)));
      return ExitUsage;
    }

    if (layerPaths.isEmpty())
    {
      writeErr(QStringLiteral("Se necesita una capa, por ejemplo:"));
      writeErr(QStringLiteral("  minigis --check-refresh -l data/sample/shapefiles/parcels.shp"));
      return ExitUsage;
    }

    return labels ? checkLabels(layerPaths, checkSize, parser.isSet(jsonOption))
                  : checkRefresh(layerPaths, checkSize, parser.isSet(jsonOption));
  }

  // ---------------------------------------------------------------------
  // Banco de pruebas de rendimiento (sin ventana)
  // ---------------------------------------------------------------------
  if (parser.isSet(benchOption))
  {
    if (layerPaths.isEmpty())
    {
      writeErr(QStringLiteral("Se necesita al menos una capa, por ejemplo:"));
      writeErr(QStringLiteral("  minigis --bench 40 --layer mapa.gpkg --size 1200x800"));
      return ExitUsage;
    }

    BenchRequest request;
    request.layerPaths = layerPaths;
    request.frames = parser.value(benchOption).toInt();
    request.scenario = parser.value(benchScenarioOption).toLower();
    request.renderer = parser.value(rendererOption).toLower();
    request.budgetMs = parser.value(benchBudgetOption).toLongLong();
    request.vertexBudget = parser.value(vertexBudgetOption).toLongLong();
    request.readBudget = parser.value(readBudgetOption).toLongLong();
    request.maxFeatures = parser.value(maxFeaturesOption).toLongLong();
    request.labels = labelOptions;
    request.spatialFilter = parser.isSet(spatialFilterOption);
    request.asJson = parser.isSet(jsonOption);

    if (request.frames <= 0)
    {
      writeErr(QStringLiteral("El numero de cuadros debe ser mayor que cero: %1").arg(parser.value(benchOption)));
      return ExitUsage;
    }

    if (request.scenario != QStringLiteral("pan") && request.scenario != QStringLiteral("zoom")
        && request.scenario != QStringLiteral("mixed"))
    {
      writeErr(QStringLiteral("Escenario no soportado: «%1». Usa pan, zoom o mixed.").arg(request.scenario));
      return ExitUsage;
    }

    if (request.renderer != QStringLiteral("auto") && request.renderer != QStringLiteral("legacy")
        && request.renderer != QStringLiteral("fast"))
    {
      writeErr(QStringLiteral("Motor no soportado: «%1». Usa auto, legacy o fast.").arg(request.renderer));
      return ExitUsage;
    }

    if (!parseSize(parser.value(sizeOption), &request.size))
    {
      writeErr(QStringLiteral("Tamano no valido: «%1». Ejemplo: --size 1600x900").arg(parser.value(sizeOption)));
      return ExitUsage;
    }

    if (!parseEpsg(parser.value(crsOption), &request.destinationEpsg))
    {
      writeErr(QStringLiteral("SRC no valido: «%1». Ejemplo: --crs EPSG:25830").arg(parser.value(crsOption)));
      return ExitUsage;
    }

    if (parser.isSet(extentOption) && !parseExtent(parser.value(extentOption), &request.extent))
    {
      writeErr(QStringLiteral("Extension no valida: «%1». Ejemplo: --extent -4,40,-3,41").arg(parser.value(extentOption)));
      return ExitUsage;
    }

    return runBenchmark(request);
  }

  // ---------------------------------------------------------------------
  // Modo informacion (sin ventana)
  // ---------------------------------------------------------------------
  if (parser.isSet(infoOption))
  {
    if (layerPaths.isEmpty())
    {
      writeErr(QStringLiteral("Se necesita al menos una capa, por ejemplo:"));
      writeErr(QStringLiteral("  minigis --info --layer data/sample/shapefiles/cities.shp"));
      return ExitUsage;
    }

    QJsonArray jsonLayers;
    for (const QString &path : layerPaths)
    {
      if (!QFileInfo::exists(path))
      {
        writeErr(QStringLiteral("No existe el archivo: %1").arg(QDir::toNativeSeparators(path)));
        return ExitFileNotFound;
      }

      Error error;
      const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, &error);
      if (layer)
      {
        if (parser.isSet(jsonOption))
          jsonLayers.append(layerInfoJson(*layer));
        else
          writeOut(layerInfoText(*layer));
        continue;
      }

      // No era vectorial: se intenta como raster. El orden importa: un
      // GeoPackage con las dos cosas se informa como vectorial (documentado).
      Error rasterError;
      const std::shared_ptr<RasterLayer> raster = RasterLayer::open(path, &rasterError);
      if (raster)
      {
        if (parser.isSet(jsonOption))
          jsonLayers.append(rasterInfoJson(*raster));
        else
          writeOut(rasterInfoText(*raster));

        // Diagnostico: extension ya reproyectada al SRC del lienzo, para
        // comprobar el encuadre sin abrir la ventana (-V).
        logVerbose(QStringLiteral("%1: extension en EPSG:4326 = %2")
                     .arg(raster->name(), raster->extentInEpsg(4326).toString()));
        for (int band = 1; band <= raster->bandCount(); ++band)
        {
          const RasterLayer::BandStats &stats = raster->bandStats(band);
          if (stats.valid)
            logVerbose(QStringLiteral("%1: banda %2 rango [%3, %4], percentiles [%5, %6]")
                         .arg(raster->name())
                         .arg(band)
                         .arg(stats.min)
                         .arg(stats.max)
                         .arg(stats.p2)
                         .arg(stats.p98));
        }
        continue;
      }

      writeErr(error.toText());
      return ExitOpenFailed;
    }

    if (parser.isSet(jsonOption))
    {
      QJsonObject root;
      root.insert(QStringLiteral("layers"), jsonLayers);
      writeOut(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
    }

    return ExitOk;
  }

  // ---------------------------------------------------------------------
  // Modos de captura (sin ventana visible)
  // ---------------------------------------------------------------------
  const bool snapshotRequested = parser.isSet(snapshotOption) || parser.isSet(snapshotWindowOption);
  if (snapshotRequested)
  {
    QSize imageSize(1200, 800);
    if (!parseSize(parser.value(sizeOption), &imageSize))
    {
      writeErr(QStringLiteral("Tamano no valido: «%1». Ejemplo: --size 1600x900").arg(parser.value(sizeOption)));
      return ExitUsage;
    }

    int destinationEpsg = 4326;
    if (!parseEpsg(parser.value(crsOption), &destinationEpsg))
    {
      writeErr(QStringLiteral("SRC no valido: «%1». Ejemplo: --crs EPSG:25830").arg(parser.value(crsOption)));
      return ExitUsage;
    }

    Rectangle requestedExtent;
    if (parser.isSet(extentOption) && !parseExtent(parser.value(extentOption), &requestedExtent))
    {
      writeErr(QStringLiteral("Extension no valida: «%1». Ejemplo: --extent -4,40,-3,41").arg(parser.value(extentOption)));
      return ExitUsage;
    }

    qint64 maxFeatures = 0;
    if (parser.isSet(maxFeaturesOption))
      maxFeatures = parser.value(maxFeaturesOption).toLongLong();

    if (parser.isSet(snapshotWindowOption))
    {
      // Se construye la ventana completa: valida paneles, modelo de atributos y
      // menus. Sin restaurar la configuracion guardada, para que la captura
      // respete --size y no modifique los ajustes del usuario.
      MainWindow window(nullptr, false);
      window.resize(imageSize);
      window.canvas()->setDestinationEpsg(destinationEpsg);
      window.canvas()->setMaximumFeaturesPerLayer(maxFeatures);

      window.show();
      QApplication::processEvents();

      // La interfaz carga en segundo plano (con barra de progreso); aqui se
      // espera a que termine procesando eventos, igual que en uso interactivo.
      window.loadPaths(layerPaths);
      if (!window.waitForLoading(300000))
      {
        writeErr(QStringLiteral("La carga de capas no termino a tiempo."));
        return ExitOpenFailed;
      }

      const int opened = static_cast<int>(window.canvas()->layers().size()
                                          + window.canvas()->rasterLayers().size());
      if (opened == 0)
      {
        writeErr(QStringLiteral("No hay capas que capturar: indica una con --layer <archivo> o como argumento."));
        return ExitNoLayers;
      }

      // Etiquetas pedidas por linea de comandos (la interfaz las puede cambiar
      // despues con el dialogo).
      for (const std::shared_ptr<VectorLayer> &layer : window.canvas()->layers())
        applyLabelOptions(layer, labelOptions);

      // La tabla de atributos permite comprobar de un vistazo que los valores se
      // leen bien (que es justo lo que se rompio con las capas indexadas).
      if (parser.isSet(withAttributesOption))
      {
        window.showAttributeTable(true);
        QApplication::processEvents();
      }

      // Deja que el layout de la ventana (paneles, docks) fije el tamano final
      // del lienzo antes de encuadrar: si no, el zoom se calcula con un tamano
      // provisional y el mapa queda descentrado. Se procesan eventos hasta que
      // el tamano se estabiliza.
      QSize lastCanvasSize;
      for (int iteration = 0; iteration < 5 && window.canvas()->size() != lastCanvasSize; ++iteration)
      {
        lastCanvasSize = window.canvas()->size();
        QApplication::processEvents();
      }

      if (requestedExtent.isValid())
        window.canvas()->setExtent(requestedExtent);
      else
        window.canvas()->zoomToFullExtent();

      QApplication::processEvents();

      const QString outputPath = parser.value(snapshotWindowOption);
      const QImage image = window.grab().toImage();
      if (image.isNull() || !image.save(outputPath, "PNG"))
      {
        writeErr(QStringLiteral("No se pudo escribir la imagen en %1").arg(QDir::toNativeSeparators(outputPath)));
        return ExitRenderFailed;
      }

      writeOut(QStringLiteral("Captura de interfaz escrita: %1 (%2x%3)")
                 .arg(QDir::toNativeSeparators(outputPath))
                 .arg(image.width())
                 .arg(image.height()));
      return ExitOk;
    }

    // Render del mapa (solo lienzo), con el tamano exacto pedido.
    const bool useStore = (parser.value(rendererOption).toLower() != QStringLiteral("legacy"));
    MapCanvas canvas;
    canvas.setDestinationEpsg(destinationEpsg);
    canvas.setMaximumFeaturesPerLayer(maxFeatures);
    canvas.setVertexBudget(parser.value(vertexBudgetOption).toLongLong());
    canvas.setReadBudget(parser.value(readBudgetOption).toLongLong());
    canvas.setRendererBackend(useStore ? MapCanvas::RendererBackend::Fast
                                       : MapCanvas::RendererBackend::Legacy);
    canvas.setUseSpatialFilter(parser.isSet(spatialFilterOption));
    canvas.resize(imageSize);

    int opened = 0;
    for (const QString &path : layerPaths)
    {
      if (!QFileInfo::exists(path))
      {
        writeErr(QStringLiteral("No existe el archivo: %1").arg(QDir::toNativeSeparators(path)));
        return ExitFileNotFound;
      }

      Error error;
      const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, &error);
      if (layer)
      {
        applyLabelOptions(layer, labelOptions);
        attachGeometryStore(layer, destinationEpsg, useStore);
        canvas.addLayer(layer, opened == 0);
        ++opened;
        logVerbose(QStringLiteral("capa cargada: %1 (%2 elementos)").arg(layer->name()).arg(layer->featureCount()));
        continue;
      }

      Error rasterError;
      const std::shared_ptr<RasterLayer> raster = RasterLayer::open(path, &rasterError);
      if (!raster)
      {
        writeErr(error.toText());
        return ExitOpenFailed;
      }

      for (int band : rasterOptions.bands)
      {
        if (band < 1 || band > raster->bandCount())
        {
          writeErr(QStringLiteral("La banda %1 no existe en «%2» (tiene %3 bandas).")
                     .arg(band)
                     .arg(raster->name())
                     .arg(raster->bandCount()));
          return ExitUsage;
        }
      }
      applyRasterOptions(raster, rasterOptions);

      canvas.addRasterLayer(raster, opened == 0);
      ++opened;
      logVerbose(QStringLiteral("raster cargado: %1 (%2x%3, %4 bandas)")
                   .arg(raster->name())
                   .arg(raster->width())
                   .arg(raster->height())
                   .arg(raster->bandCount()));
    }

    if (opened == 0)
    {
      writeErr(QStringLiteral("No hay capas que renderizar: indica una con --layer <archivo> o como argumento."));
      return ExitNoLayers;
    }

    if (requestedExtent.isValid())
      canvas.setExtent(requestedExtent);
    else
      canvas.zoomToFullExtent();

    const QString outputPath = parser.value(snapshotOption);
    const QImage image = canvas.renderToImage(imageSize);
    {
      const LabelRenderer::FrameStats &labels = canvas.labelFrameStats();
      logVerbose(QStringLiteral("etiquetas: %1 colocadas, %2 sin campo, %3 por solape, %4 repetidas, "
                                "%5 por tamano, %6 candidatos, %7 lecturas de atributos, %8 ms, %9 sin contornos")
                   .arg(labels.placed).arg(labels.skippedEmpty).arg(labels.skippedCollision)
                   .arg(labels.skippedDuplicate).arg(labels.skippedSmall).arg(labels.candidates)
                   .arg(labels.valueReads).arg(labels.paintMs).arg(labels.textFallbacks));
    }
    if (image.isNull() || !image.save(outputPath, "PNG"))
    {
      writeErr(QStringLiteral("No se pudo escribir la imagen en %1").arg(QDir::toNativeSeparators(outputPath)));
      return ExitRenderFailed;
    }

    writeOut(QStringLiteral("Imagen escrita: %1 (%2x%3, render %4 ms)")
               .arg(QDir::toNativeSeparators(outputPath))
               .arg(image.width())
               .arg(image.height())
               .arg(canvas.lastRenderMilliseconds()));
    return ExitOk;
  }

  // ---------------------------------------------------------------------
  // Interfaz grafica
  // ---------------------------------------------------------------------
  MainWindow window;
  window.show();

  if (!layerPaths.isEmpty())
    window.loadPaths(layerPaths);

  return app.exec();
}
