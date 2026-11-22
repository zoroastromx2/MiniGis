#include "minigismosaicreplica.h"

#include "minigisrasterlayer.h"
#include "minigismosaiclayer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <memory>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_utils.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>

namespace minigis
{
namespace
{

/** Version del formato de la cache: cambiarla invalida las replicas viejas. */
constexpr int kReplicaVersion = 1;
/** Niveles de overview de la replica (2, 4, 8 y 16). */
const int kOverviewLevels[] = {2, 4, 8, 16};
constexpr int kOverviewCount = 4;
/** Filas que se copian de golpe al materializar la replica (~4 MB por banda). */
constexpr int kCopyBlockRows = 128;

struct ProgressContext
{
  const std::function<bool(int, const QString &)> *progress = nullptr;
  int basePercent = 0;
  int spanPercent = 100;
  bool cancelled = false;
};

/** Callback de GDAL: informa y deja cancelar devolviendo FALSE. */
int CPL_STDCALL replicaProgress(double complete, const char *message, void *data)
{
  auto *context = static_cast<ProgressContext *>(data);
  if (!context || context->cancelled)
    return FALSE;

  const int percent =
    context->basePercent + static_cast<int>(std::clamp(complete, 0.0, 1.0) * context->spanPercent);
  if (context->progress && *context->progress)
  {
    if (!(*context->progress)(std::clamp(percent, 0, 100), QString::fromUtf8(message ? message : "")))
    {
      context->cancelled = true;
      return FALSE;
    }
  }
  return TRUE;
}

std::unique_ptr<OGRCoordinateTransformation> transformationFrom(GDALDatasetH dataset,
                                                                const OGRSpatialReference &destination)
{
  const OGRSpatialReference *source = reinterpret_cast<const OGRSpatialReference *>(GDALGetSpatialRef(dataset));
  if (!source)
    return nullptr;

  std::unique_ptr<OGRSpatialReference> sourceClone(source->Clone());
  if (!sourceClone)
    return nullptr;
  sourceClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference destinationCopy(destination);
  destinationCopy.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  return std::unique_ptr<OGRCoordinateTransformation>(
    OGRCreateCoordinateTransformation(sourceClone.get(), &destinationCopy));
}

/** Extension de un dataset en el SRC indicado (puntos intermedios incluidos). */
bool transformedExtent(GDALDatasetH dataset, const OGRSpatialReference &destination, int steps,
                       Rectangle *extent)
{
  const std::unique_ptr<OGRCoordinateTransformation> transformation =
    transformationFrom(dataset, destination);
  if (!transformation)
    return false;

  const double width = GDALGetRasterXSize(dataset);
  const double height = GDALGetRasterYSize(dataset);
  double geoTransform[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  if (GDALGetGeoTransform(dataset, geoTransform) != CE_None)
    return false;

  const auto append = [&](double pixelX, double pixelY) {
    double x = geoTransform[0] + pixelX * geoTransform[1] + pixelY * geoTransform[2];
    double y = geoTransform[3] + pixelX * geoTransform[4] + pixelY * geoTransform[5];
    if (transformation->Transform(1, &x, &y) && std::isfinite(x) && std::isfinite(y))
      extent->extend(x, y);
  };

  for (int i = 0; i < steps; ++i)
  {
    const double t = static_cast<double>(i) / steps;
    append(width * t, 0.0);
    append(width, height * t);
    append(width * (1.0 - t), height);
    append(0.0, height * (1.0 - t));
  }

  return extent->isValid();
}

/** Tamano en el SRC destino de un pixel del origen, medido en el centro. */
bool sourcePixelSizeInDestination(GDALDatasetH dataset, const OGRSpatialReference &destination, double *size)
{
  const std::unique_ptr<OGRCoordinateTransformation> transformation =
    transformationFrom(dataset, destination);
  if (!transformation)
    return false;

  const double width = GDALGetRasterXSize(dataset);
  const double height = GDALGetRasterYSize(dataset);
  double geoTransform[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  if (GDALGetGeoTransform(dataset, geoTransform) != CE_None)
    return false;

  double x0 = geoTransform[0] + (width / 2.0) * geoTransform[1] + (height / 2.0) * geoTransform[2];
  double y0 = geoTransform[3] + (width / 2.0) * geoTransform[4] + (height / 2.0) * geoTransform[5];
  // Solo el paso horizontal del pixel (gt[1]): medir la diagonal daria un pixel
  // de replica ~1,4 veces mayor de lo pedido.
  double x1 = x0 + geoTransform[1];
  double y1 = y0;
  if (!transformation->Transform(1, &x0, &y0) || !transformation->Transform(1, &x1, &y1))
    return false;

  const double dx = x1 - x0;
  const double dy = y1 - y0;
  const double distance = std::sqrt(dx * dx + dy * dy);
  if (!(distance > 0.0) || !std::isfinite(distance))
    return false;

  *size = distance;
  return true;
}

QString sidecarPath(const QString &directory, const QString &key)
{
  return QDir(directory).filePath(key + QStringLiteral(".json"));
}

QString replicaPath(const QString &directory, const QString &key)
{
  return QDir(directory).filePath(key + QStringLiteral(".tif"));
}

QString isoNow()
{
  return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

/** Comprueba el archivo lateral: misma version, SRC, factor y mismas hojas. */
bool sidecarIsValid(const QString &jsonPath, const QString &tifPath, const QStringList &paths,
                    int destinationEpsg, int factor, MosaicReplicaState *state)
{
  QFile file(jsonPath);
  if (!file.open(QIODevice::ReadOnly))
    return false;

  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  file.close();
  if (!document.isObject())
    return false;

  const QJsonObject object = document.object();
  if (object.value(QStringLiteral("version")).toInt() != kReplicaVersion)
    return false;
  if (object.value(QStringLiteral("epsg")).toInt() != destinationEpsg)
    return false;
  if (object.value(QStringLiteral("factor")).toInt() != factor)
    return false;
  if (!QFileInfo::exists(tifPath))
    return false;

  const QJsonArray storedTiles = object.value(QStringLiteral("tiles")).toArray();
  if (storedTiles.size() != paths.size())
    return false;

  for (int index = 0; index < storedTiles.size(); ++index)
  {
    const QJsonObject stored = storedTiles.at(index).toObject();
    const QFileInfo info(paths.at(index));
    if (stored.value(QStringLiteral("path")).toString() != info.absoluteFilePath())
      return false;
    if (static_cast<qint64>(stored.value(QStringLiteral("size")).toDouble()) != info.size())
      return false;
    if (static_cast<qint64>(stored.value(QStringLiteral("mtime")).toDouble())
        != info.lastModified().toMSecsSinceEpoch())
      return false;
  }

  if (state)
  {
    state->available = true;
    state->reused = true;
    state->path = tifPath;
    state->pixels = QSize(object.value(QStringLiteral("width")).toInt(),
                          object.value(QStringLiteral("height")).toInt());
    state->pixelSize = object.value(QStringLiteral("pixel_size")).toDouble();
    state->bytes = static_cast<qint64>(object.value(QStringLiteral("bytes")).toDouble());
    state->levels = object.value(QStringLiteral("levels")).toInt();
  }

  return true;
}

void writeSidecar(const QString &jsonPath, const QString &key, const MosaicReplicaState &state,
                  const QStringList &paths, int destinationEpsg, int factor)
{
  QJsonArray storedTiles;
  for (const QString &path : paths)
  {
    const QFileInfo info(path);
    QJsonObject entry;
    entry.insert(QStringLiteral("path"), info.absoluteFilePath());
    entry.insert(QStringLiteral("size"), static_cast<double>(info.size()));
    entry.insert(QStringLiteral("mtime"), static_cast<double>(info.lastModified().toMSecsSinceEpoch()));
    storedTiles.append(entry);
  }

  QJsonObject object;
  object.insert(QStringLiteral("version"), kReplicaVersion);
  object.insert(QStringLiteral("key"), key);
  object.insert(QStringLiteral("epsg"), destinationEpsg);
  object.insert(QStringLiteral("factor"), factor);
  object.insert(QStringLiteral("width"), state.pixels.width());
  object.insert(QStringLiteral("height"), state.pixels.height());
  object.insert(QStringLiteral("pixel_size"), state.pixelSize);
  object.insert(QStringLiteral("bytes"), static_cast<double>(state.bytes));
  object.insert(QStringLiteral("levels"), state.levels);
  object.insert(QStringLiteral("created"), isoNow());
  object.insert(QStringLiteral("last_used"), isoNow());
  object.insert(QStringLiteral("tiles"), storedTiles);

  QFile file(jsonPath);
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

/** Marca la replica como usada ahora (para el borrado por antiguedad). */
void touchSidecar(const QString &jsonPath)
{
  QFile file(jsonPath);
  if (!file.open(QIODevice::ReadOnly))
    return;
  QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  file.close();
  if (!document.isObject())
    return;

  QJsonObject object = document.object();
  object.insert(QStringLiteral("last_used"), isoNow());
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

QString MosaicReplica::cacheDirectory(const MosaicReplicaOptions &options)
{
  QString directory = options.cacheDir.trimmed();
  if (directory.isEmpty())
    directory = qEnvironmentVariable("MINIGIS_MOSAIC_CACHE_DIR").trimmed();
  if (directory.isEmpty())
  {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    directory = QDir(base.isEmpty() ? QDir::tempPath() : base).filePath(QStringLiteral("mosaicos"));
  }

  // Si la carpeta elegida no se puede escribir (perfil de solo lectura, disco
  // lleno), se usa la carpeta temporal: el mosaico funciona igual, solo se
  // pierde la reutilizacion entre sesiones.
  const auto writable = [](const QString &path) {
    if (path.isEmpty() || !QDir().mkpath(path))
      return false;
    QTemporaryFile probe(QDir(path).filePath(QStringLiteral("minigis_probe_XXXXXX")));
    return probe.open();
  };

  if (writable(directory))
    return directory;

  const QString fallback = QDir(QDir::tempPath()).filePath(QStringLiteral("minigis-mosaicos"));
  if (writable(fallback))
    return fallback;

  return {};
}

QString MosaicReplica::cacheKey(const QStringList &paths, int destinationEpsg,
                                const MosaicReplicaOptions &options)
{
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArrayLiteral("minigis-mosaico-"));
  hash.addData(QByteArray::number(kReplicaVersion));
  hash.addData(QByteArray::number(destinationEpsg));
  hash.addData(QByteArray::number(options.factor));

  std::vector<QByteArray> descriptions;
  descriptions.reserve(static_cast<size_t>(paths.size()));
  for (const QString &path : paths)
  {
    const QFileInfo info(path);
    descriptions.push_back(info.absoluteFilePath().toUtf8() + '|' + QByteArray::number(info.size()) + '|'
                           + QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
  }

  // Orden estable: la misma seleccion en otro orden es el mismo mosaico.
  std::sort(descriptions.begin(), descriptions.end());
  for (const QByteArray &description : descriptions)
  {
    hash.addData(description);
    hash.addData("|");
  }

  return QString::fromLatin1(hash.result().toHex().left(16));
}

MosaicReplicaState MosaicReplica::prepare(const RasterMosaic &mosaic, int destinationEpsg,
                                          const MosaicReplicaOptions &options,
                                          const std::function<bool(int, const QString &)> &progress)
{
  QStringList paths;
  paths.reserve(mosaic.tileCount());
  for (const std::shared_ptr<RasterLayer> &tile : mosaic.tiles())
  {
    if (tile)
      paths << tile->sourcePath();
  }
  return preparePaths(paths, destinationEpsg, options, progress);
}

MosaicReplicaState MosaicReplica::preparePaths(const QStringList &paths, int destinationEpsg,
                                               const MosaicReplicaOptions &options,
                                               const std::function<bool(int, const QString &)> &progress)
{
  MosaicReplicaState state;
  const auto report = [&progress](int percent, const QString &message) {
    return progress ? progress(percent, message) : true;
  };

  if (!options.enabled)
  {
    state.message = QStringLiteral("Replica desactivada (--no-mosaic-replica).");
    return state;
  }

  if (paths.isEmpty())
  {
    state.message = QStringLiteral("El mosaico no tiene hojas.");
    return state;
  }

  const QString directory = cacheDirectory(options);
  if (directory.isEmpty())
  {
    state.message = QStringLiteral("No hay una carpeta escribible para la cache de replicas.");
    return state;
  }

  const QString key = cacheKey(paths, destinationEpsg, options);
  const QString tifPath = replicaPath(directory, key);
  const QString jsonPath = sidecarPath(directory, key);

  // 1) Reutilizar si la replica ya existe y las hojas no han cambiado.
  if (sidecarIsValid(jsonPath, tifPath, paths, destinationEpsg, options.factor, &state))
  {
    touchSidecar(jsonPath);
    report(100, QStringLiteral("Replica reutilizada (%1 MB)").arg(state.bytes / (1024.0 * 1024.0), 0, 'f', 1));
    return state;
  }

  // 2) Construir. Todo va a un archivo temporal que solo se renombra al final:
  //    una replica a medias nunca se reutiliza.
  if (!report(0, QStringLiteral("Preparando el mosaico (%1 hojas)...").arg(paths.size())))
  {
    state.message = QStringLiteral("Cancelado.");
    return state;
  }

  const QString tmpTif = tifPath + QStringLiteral(".tmp");
  const QByteArray vrtPath = QDir(directory).filePath(key + QStringLiteral(".vrt")).toUtf8();
  GDALDatasetH vrt = nullptr;
  GDALDatasetH warped = nullptr;
  GDALDatasetH destination = nullptr;
  char *destinationWkt = nullptr;
  bool ok = false;

  const auto cleanup = [&]() {
    if (warped)
    {
      GDALClose(warped);
      warped = nullptr;
    }
    if (destination)
    {
      GDALClose(destination);
      destination = nullptr;
    }
    if (vrt)
    {
      GDALClose(vrt);
      vrt = nullptr;
    }
    QFile::remove(tmpTif);
    QFile::remove(QString::fromUtf8(vrtPath));
    if (destinationWkt)
    {
      CPLFree(destinationWkt);
      destinationWkt = nullptr;
    }
  };

  do
  {
    OGRSpatialReference destinationSrs;
    if (destinationSrs.importFromEPSG(destinationEpsg) != OGRERR_NONE)
    {
      state.message = QStringLiteral("SRC de destino no valido (EPSG:%1).").arg(destinationEpsg);
      break;
    }
    destinationSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    if (destinationSrs.exportToWkt(&destinationWkt) != OGRERR_NONE || !destinationWkt)
    {
      state.message = QStringLiteral("No se pudo exportar el SRC de destino.");
      break;
    }

    // VRT de las hojas (por ruta): es el origen de la reproyeccion y no necesita
    // datasets abiertos, de modo que puede construirse sin tocar los que dibuja
    // la interfaz.
    char **sourceNames = nullptr;
    for (const QString &path : paths)
    {
      const QByteArray utf8 = path.toUtf8();
      sourceNames = CSLAddString(sourceNames, utf8.constData());
    }

    char **vrtArguments = nullptr;
    vrtArguments = CSLAddString(vrtArguments, "-resolution");
    vrtArguments = CSLAddString(vrtArguments, "highest");
    GDALBuildVRTOptions *vrtOptions = GDALBuildVRTOptionsNew(vrtArguments, nullptr);
    CSLDestroy(vrtArguments);

    ProgressContext vrtContext;
    vrtContext.progress = &progress;
    vrtContext.basePercent = 0;
    vrtContext.spanPercent = 10;
    GDALBuildVRTOptionsSetProgress(vrtOptions, replicaProgress, &vrtContext);

    int usageError = 0;
    GDALDatasetH builtVrt = GDALBuildVRT(vrtPath.constData(), paths.size(), nullptr, sourceNames, vrtOptions,
                                         &usageError);
    GDALBuildVRTOptionsFree(vrtOptions);
    CSLDestroy(sourceNames);

    if (!builtVrt)
    {
      state.message = QStringLiteral("No se pudo construir el mosaico intermedio (VRT).");
      break;
    }

    // Se cierra el VRT que devuelve GDALBuildVRT y se reabre desde el archivo:
    // asi el resto del proceso lee las hojas por ruta, como haria gdalwarp.
    GDALClose(builtVrt);
    vrt = GDALOpenEx(vrtPath.constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!vrt)
    {
      state.message = QStringLiteral("No se pudo reabrir el mosaico intermedio (VRT).");
      break;
    }

    if (vrtContext.cancelled)
    {
      state.message = QStringLiteral("Cancelado.");
      break;
    }

    // Rejilla de la replica: 1/factor del pixel de origen, acotada por lado.
    Rectangle bounds;
    if (!transformedExtent(vrt, destinationSrs, 16, &bounds))
    {
      state.message = QStringLiteral("No se pudo calcular la extension del mosaico reproyectado.");
      break;
    }

    double pixelSize = 0.0;
    if (!sourcePixelSizeInDestination(vrt, destinationSrs, &pixelSize))
    {
      state.message = QStringLiteral("No se pudo calcular el tamano de pixel de la replica.");
      break;
    }

    pixelSize *= std::max(1, options.factor);
    QSize pixels(std::max(1, static_cast<int>(std::ceil(bounds.width() / pixelSize))),
                 std::max(1, static_cast<int>(std::ceil(bounds.height() / pixelSize))));
    const int longestSide = std::max(pixels.width(), pixels.height());
    if (longestSide > options.maxSide)
    {
      pixelSize *= static_cast<double>(longestSide) / options.maxSide;
      pixels = QSize(std::max(1, static_cast<int>(std::ceil(bounds.width() / pixelSize))),
                     std::max(1, static_cast<int>(std::ceil(bounds.height() / pixelSize))));
    }

    if (!report(12, QStringLiteral("Creando la replica (%1x%2)...").arg(pixels.width()).arg(pixels.height())))
    {
      state.message = QStringLiteral("Cancelado.");
      break;
    }

    if (GDALGetDriverByName("GTiff") == nullptr)
    {
      state.message = QStringLiteral("GDAL no tiene el driver GTiff.");
      break;
    }

    // Tres bandas: la compresion JPEG del GeoTIFF no admite una cuarta. Los
    // huecos de la cobertura (la union de las hojas no es un rectangulo) se
    // marcan con NoData=0 y el mosaico los dibuja transparentes.
    const int sourceBands = std::min(3, GDALGetRasterCount(vrt));

    char **createOptions = nullptr;
    createOptions = CSLSetNameValue(createOptions, "TILED", "YES");
    if (sourceBands == 3)
    {
      createOptions = CSLSetNameValue(createOptions, "COMPRESS", "JPEG");
      createOptions = CSLSetNameValue(createOptions, "JPEG_QUALITY", "80");
    }
    else
    {
      createOptions = CSLSetNameValue(createOptions, "COMPRESS", "DEFLATE");
      createOptions = CSLSetNameValue(createOptions, "PREDICTOR", "2");
    }
    createOptions = CSLSetNameValue(createOptions, "BIGTIFF", "IF_SAFER");
    createOptions = CSLSetNameValue(createOptions, "NUM_THREADS", "ALL_CPUS");

    destination = GDALCreate(GDALGetDriverByName("GTiff"), tmpTif.toUtf8().constData(), pixels.width(),
                             pixels.height(), sourceBands, GDT_Byte, createOptions);
    CSLDestroy(createOptions);

    if (!destination)
    {
      state.message = QStringLiteral("No se pudo crear el GeoTIFF de la replica.");
      break;
    }

    double geoTransform[6] = {bounds.xMin(), pixelSize, 0.0, bounds.yMax(), 0.0, -pixelSize};
    GDALSetGeoTransform(destination, geoTransform);
    GDALSetProjection(destination, destinationWkt);

    const GDALColorInterp interpretations[3] = {GCI_RedBand, GCI_GreenBand, GCI_BlueBand};
    for (int band = 1; band <= sourceBands; ++band)
      GDALSetRasterColorInterpretation(GDALGetRasterBand(destination, band), interpretations[band - 1]);

    // El relleno fuera de la cobertura es 0: se declara NoData para que el
    // mosaico lo dibuje transparente en vez de negro.
    GDALSetRasterNoDataValue(GDALGetRasterBand(destination, 1), 0.0);

    // Vista reproyectada con la rejilla EXACTA de la replica: es el mismo camino
    // que usa el renderizador para dibujar (GDALCreateWarpedVRT), no el
    // GDALChunkAndWarpImage de gdalwarp, que en este equipo resultaba inestable.
    const char *sourceWkt = GDALGetProjectionRef(vrt);
    if (!sourceWkt || !*sourceWkt)
    {
      state.message = QStringLiteral("El mosaico intermedio no tiene SRC.");
      break;
    }

    GDALWarpOptions *warpOptions = GDALCreateWarpOptions();
    warpOptions->eResampleAlg = GRA_Bilinear;
    warpOptions->hSrcDS = vrt;
    GDALWarpInitDefaultBandMapping(warpOptions, sourceBands);
    // La materializacion es el trabajo pesado de la replica: se aprovechan los
    // hilos del equipo (medido: el warp multihilo de GDAL es varias veces mas
    // rapido que el secuencial).
    warpOptions->papszWarpOptions =
      CSLSetNameValue(warpOptions->papszWarpOptions, "NUM_THREADS", "ALL_CPUS");
    // Sin esto el warp lee las hojas a resolucion completa aunque la replica sea
    // 1/8: son 64 veces mas pixeles decodificados (medido: 2 min frente a 20 s).
    warpOptions->papszWarpOptions =
      CSLSetNameValue(warpOptions->papszWarpOptions, "USE_SRC_OVERVIEWS", "YES");

    char **transformerOptions = nullptr;
    transformerOptions = CSLSetNameValue(transformerOptions, "SRC_SRS", sourceWkt);
    transformerOptions = CSLSetNameValue(transformerOptions, "DST_SRS", destinationWkt);
    warpOptions->pfnTransformer = GDALGenImgProjTransform;
    warpOptions->pTransformerArg = GDALCreateGenImgProjTransformer2(vrt, nullptr, transformerOptions);
    CSLDestroy(transformerOptions);

    if (!warpOptions->pTransformerArg)
    {
      GDALDestroyWarpOptions(warpOptions);
      state.message = QStringLiteral("No se pudo crear el transformador de reproyeccion.");
      break;
    }

    // Inicializa el transformador como hace GDALAutoCreateWarpedVRT antes de
    // fijar la rejilla propia (la misma secuencia que RasterLayer::viewForEpsg).
    double suggestedGeoTransform[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    double suggestedExtent[4] = {0.0, 0.0, 0.0, 0.0};
    int suggestedWidth = 0;
    int suggestedHeight = 0;
    GDALSuggestedWarpOutput2(vrt, warpOptions->pfnTransformer, warpOptions->pTransformerArg,
                             suggestedGeoTransform, &suggestedWidth, &suggestedHeight, suggestedExtent, 0);

    GDALSetGenImgProjTransformerDstGeoTransform(warpOptions->pTransformerArg, geoTransform);
    warpOptions->pTransformerArg =
      GDALCreateApproxTransformer(warpOptions->pfnTransformer, warpOptions->pTransformerArg, 0.125);
    warpOptions->pfnTransformer = GDALApproxTransform;
    GDALApproxTransformerOwnsSubtransformer(warpOptions->pTransformerArg, TRUE);

    warped = GDALCreateWarpedVRT(vrt, pixels.width(), pixels.height(), geoTransform, warpOptions);
    GDALDestroyWarpOptions(warpOptions);

    if (!warped)
    {
      const QString gdalMessage = QString::fromUtf8(CPLGetLastErrorMsg()).trimmed();
      state.message = QStringLiteral("No se pudo preparar la reproyeccion de la replica.%1")
                        .arg(gdalMessage.isEmpty() ? QString() : QStringLiteral(" GDAL: ") + gdalMessage);
      break;
    }

    // Materializacion por bloques de filas, con progreso y cancelacion.
    const int blockRows = std::max(1, std::min(kCopyBlockRows, pixels.height()));
    std::vector<unsigned char> buffer(static_cast<size_t>(pixels.width()) * static_cast<size_t>(blockRows));
    bool readFailed = false;
    bool cancelled = false;
    for (int y = 0; y < pixels.height() && !readFailed && !cancelled; y += blockRows)
    {
      const int rows = std::min(blockRows, pixels.height() - y);
      for (int band = 1; band <= sourceBands && !readFailed; ++band)
      {
        GDALRasterBandH sourceBand = GDALGetRasterBand(warped, band);
        GDALRasterBandH destinationBand = GDALGetRasterBand(destination, band);
        if (!sourceBand || !destinationBand)
        {
          readFailed = true;
          break;
        }

        if (GDALRasterIO(sourceBand, GF_Read, 0, y, pixels.width(), rows, buffer.data(), pixels.width(), rows,
                         GDT_Byte, 0, 0)
            != CE_None)
        {
          readFailed = true;
          break;
        }

        if (GDALRasterIO(destinationBand, GF_Write, 0, y, pixels.width(), rows, buffer.data(), pixels.width(),
                         rows, GDT_Byte, 0, 0)
            != CE_None)
        {
          readFailed = true;
          break;
        }
      }

      const int percent = 15 + static_cast<int>(65.0 * (y + rows) / pixels.height());
      if (!report(percent, QStringLiteral("Reproyectando la replica (%1%)").arg(percent)))
        cancelled = true;
    }

    if (cancelled)
    {
      state.message = QStringLiteral("Cancelado.");
      break;
    }

    if (readFailed)
    {
      state.message = QStringLiteral("GDAL no pudo reproyectar el mosaico a la replica.");
      break;
    }

    GDALFlushCache(destination);
    GDALClose(destination);
    destination = nullptr;
    GDALClose(warped);
    warped = nullptr;
    GDALClose(vrt);
    vrt = nullptr;
    QFile::remove(QString::fromUtf8(vrtPath));

    report(82, QStringLiteral("Moviendo la replica a la cache..."));

    if (QFile::exists(tifPath) && !QFile::remove(tifPath))
    {
      state.message = QStringLiteral("No se pudo reemplazar la replica anterior en la cache.");
      break;
    }

    if (!QFile::rename(tmpTif, tifPath))
    {
      state.message = QStringLiteral("No se pudo mover la replica a su sitio en la cache.");
      break;
    }

    // Overviews internas: sin ellas la replica solo aceleraria la vista de pais.
    GDALDatasetH overviewDataset = GDALOpenEx(tifPath.toUtf8().constData(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                                              nullptr, nullptr, nullptr);
    if (!overviewDataset)
    {
      state.message = QStringLiteral("No se pudo reabrir la replica para calcular sus niveles.");
      break;
    }

    ProgressContext overviewContext;
    overviewContext.progress = &progress;
    overviewContext.basePercent = 82;
    overviewContext.spanPercent = 16;
    const bool overviewsOk =
      GDALBuildOverviews(overviewDataset, "AVERAGE", kOverviewCount, const_cast<int *>(kOverviewLevels), 0,
                         nullptr, replicaProgress, &overviewContext)
      == CE_None;
    GDALFlushCache(overviewDataset);
    GDALClose(overviewDataset);

    if (overviewContext.cancelled)
    {
      state.message = QStringLiteral("Cancelado.");
      break;
    }

    if (!overviewsOk)
    {
      state.message = QStringLiteral("No se pudieron calcular los niveles de detalle de la replica.");
      break;
    }

    state.available = true;
    state.rebuilt = true;
    state.path = tifPath;
    state.pixels = pixels;
    state.pixelSize = pixelSize;
    state.bytes = QFileInfo(tifPath).size();
    state.levels = kOverviewCount;

    writeSidecar(jsonPath, key, state, paths, destinationEpsg, options.factor);
    trimCache(directory, options.maxCacheBytes, key);

    report(100, QStringLiteral("Replica lista (%1 MB, %2x%3)")
                 .arg(state.bytes / (1024.0 * 1024.0), 0, 'f', 1)
                 .arg(pixels.width())
                 .arg(pixels.height()));
    ok = true;
  } while (false);

  if (!ok)
    cleanup();

  if (destinationWkt)
    CPLFree(destinationWkt);
  return state;
}

qint64 MosaicReplica::trimCache(const QString &directory, qint64 maxBytes, const QString &keepKey)
{
  QDir dir(directory);
  if (!dir.exists() || maxBytes <= 0)
    return 0;

  struct Entry
  {
    QString key;
    qint64 bytes = 0;
    QDateTime lastUsed;
  };

  std::vector<Entry> entries;
  qint64 total = 0;
  for (const QFileInfo &info : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files))
  {
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
      continue;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();

    Entry entry;
    entry.key = info.completeBaseName();
    entry.lastUsed = QDateTime::fromString(document.object().value(QStringLiteral("last_used")).toString(),
                                           Qt::ISODate);
    entry.bytes = QFileInfo(replicaPath(directory, entry.key)).size();
    total += entry.bytes;
    entries.push_back(entry);
  }

  std::sort(entries.begin(), entries.end(),
            [](const Entry &left, const Entry &right) { return left.lastUsed < right.lastUsed; });

  qint64 removed = 0;
  for (const Entry &entry : entries)
  {
    if (total <= maxBytes)
      break;
    if (entry.key == keepKey)
      continue;

    QFile::remove(sidecarPath(directory, entry.key));
    QFile::remove(replicaPath(directory, entry.key));
    total -= entry.bytes;
    removed += entry.bytes;
  }

  return removed;
}

} // namespace minigis
