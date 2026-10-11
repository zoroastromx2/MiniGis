/**
 * MiniGIS - punto de entrada.
 *
 * Modos de ejecucion (documentados tambien en --help):
 *   minigis [archivos...]                     interfaz grafica
 *   minigis --info [--json] --layer x.shp     informacion de las capas, sin ventana
 *   minigis --snapshot out.png --layer x.shp  render del mapa a PNG, sin ventana
 *   minigis --snapshot-window ui.png          captura de la interfaz completa
 *   minigis --list-drivers                    drivers vectoriales de GDAL
 *
 * Contrato del ejecutable (lo usan scripts y las pruebas de CTest):
 *   0 exito | 1 uso incorrecto | 2 archivo no encontrado | 3 GDAL no pudo abrir
 *   4 no se pudo escribir la imagen | 5 no hay capas para renderizar
 */

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSize>
#include <QStringList>
#include <QTextStream>

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
#include "minigismainwindow.h"
#include "minigismapcanvas.h"
#include "minigisrectangle.h"
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
  ExitNoLayers = 5
};

bool g_verbose = false;

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
                          "\nCodigos de salida:\n"
                          "  0 exito            1 uso incorrecto       2 archivo no encontrado\n"
                          "  3 GDAL no pudo abrir la capa             4 no se pudo escribir la imagen\n"
                          "  5 no hay capas que renderizar\n");
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

  parser.addOptions({infoOption, jsonOption, driversOption, snapshotOption, snapshotWindowOption,
                     layerOption, extentOption, sizeOption, crsOption, maxFeaturesOption, verboseOption});
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

        window.canvas()->addLayer(layer, opened == 0);
        ++opened;
        logVerbose(QStringLiteral("capa cargada: %1 (%2 elementos)").arg(layer->name()).arg(layer->featureCount()));
      }

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
    MapCanvas canvas;
    canvas.setDestinationEpsg(destinationEpsg);
    canvas.setMaximumFeaturesPerLayer(maxFeatures);
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
