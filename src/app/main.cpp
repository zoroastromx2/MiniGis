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
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSize>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <memory>

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
#include "minigismainwindow.h"
#include "minigismapcanvas.h"
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
  for (const QString &path : request.layerPaths)
  {
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
  }

  return mismatches == 0 ? ExitOk : ExitRenderFailed;
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

/** Construye el almacen indexado de una capa para los modos sin ventana. */
void attachGeometryStore(const std::shared_ptr<VectorLayer> &layer, int destinationEpsg, bool enabled)
{
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
  const QCommandLineOption indexShpOption(QStringLiteral("index-shp"),
                                          QStringLiteral("Diagnostico: indexa las envolventes de <archivo.shp> y las verifica contra GDAL"),
                                          QStringLiteral("archivo.shp"));
  const QCommandLineOption readBudgetOption(QStringLiteral("read-budget"),
                                            QStringLiteral("Elementos leidos del archivo por cuadro en capas indexadas por envolventes (0 = sin limite)"),
                                            QStringLiteral("n"), QStringLiteral("40000"));

  parser.addOptions({infoOption, jsonOption, driversOption, snapshotOption, snapshotWindowOption,
                     layerOption, extentOption, sizeOption, crsOption, maxFeaturesOption, verboseOption,
                     benchOption, benchScenarioOption, benchBudgetOption, rendererOption, vertexBudgetOption,
                     spatialFilterOption, readBudgetOption, indexShpOption});
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
      if (!layer)
      {
        writeErr(error.toText());
        return ExitOpenFailed;
      }

      if (parser.isSet(jsonOption))
        jsonLayers.append(layerInfoJson(*layer));
      else
        writeOut(layerInfoText(*layer));
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

      const int opened = static_cast<int>(window.canvas()->layers().size());
      if (opened == 0)
      {
        writeErr(QStringLiteral("No hay capas que capturar: indica una con --layer <archivo> o como argumento."));
        return ExitNoLayers;
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
      if (!layer)
      {
        writeErr(error.toText());
        return ExitOpenFailed;
      }

      attachGeometryStore(layer, destinationEpsg, useStore);
      canvas.addLayer(layer, opened == 0);
      ++opened;
      logVerbose(QStringLiteral("capa cargada: %1 (%2 elementos)").arg(layer->name()).arg(layer->featureCount()));
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
