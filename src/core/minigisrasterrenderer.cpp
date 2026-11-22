#include "minigisrasterrenderer.h"

#include "minigismosaiclayer.h"
#include "minigisrasterlayer.h"
#include "minigisrasterstyle.h"
#include "minigismaprenderer.h"

#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QRgb>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <gdal.h>

namespace minigis
{
namespace
{

/** Minimo y maximo teoricos de un tipo de GDAL, para normalizar a 0..255. */
void typeRange(GDALDataType type, double *minimum, double *maximum)
{
  switch (type)
  {
    case GDT_Byte:
      *minimum = 0.0;
      *maximum = 255.0;
      break;
    case GDT_UInt16:
      *minimum = 0.0;
      *maximum = 65535.0;
      break;
    case GDT_Int16:
      *minimum = -32768.0;
      *maximum = 32767.0;
      break;
    case GDT_UInt32:
      *minimum = 0.0;
      *maximum = 4294967295.0;
      break;
    case GDT_Int32:
      *minimum = -2147483648.0;
      *maximum = 2147483647.0;
      break;
    case GDT_Float32:
    case GDT_Float64:
      *minimum = 0.0;
      *maximum = 1.0;
      break;
    default:
      *minimum = 0.0;
      *maximum = 255.0;
      break;
  }
}

/** Lee un muestreo del buffer con el tipo nativo de la banda. */
double sampleAt(const unsigned char *buffer, GDALDataType type, qint64 index)
{
  switch (type)
  {
    case GDT_Byte:
      return buffer[index];
    case GDT_UInt16:
    {
      quint16 value = 0;
      std::memcpy(&value, buffer + index * 2, sizeof(value));
      return value;
    }
    case GDT_Int16:
    {
      qint16 value = 0;
      std::memcpy(&value, buffer + index * 2, sizeof(value));
      return value;
    }
    case GDT_UInt32:
    {
      quint32 value = 0;
      std::memcpy(&value, buffer + index * 4, sizeof(value));
      return value;
    }
    case GDT_Int32:
    {
      qint32 value = 0;
      std::memcpy(&value, buffer + index * 4, sizeof(value));
      return value;
    }
    case GDT_Float32:
    {
      float value = 0.0f;
      std::memcpy(&value, buffer + index * 4, sizeof(value));
      return static_cast<double>(value);
    }
    case GDT_Float64:
    {
      double value = 0.0;
      std::memcpy(&value, buffer + index * 8, sizeof(value));
      return value;
    }
    default:
      return 0.0;
  }
}

bool valueIsNoData(double value, const RasterLayer::BandInfo &band, bool useNoData)
{
  if (!useNoData || !band.hasNoData)
    return false;
  if (!std::isfinite(value) || !std::isfinite(band.noData))
    return std::isnan(value) && std::isnan(band.noData);
  return std::fabs(value - band.noData) <= 1e-6 * std::max(1.0, std::fabs(band.noData));
}

/** Rango de valores que se mapea a 0..255 segun el modo de estiramiento. */
void stretchRange(const RasterLayer::BandStats &stats, const RasterStyle &style, GDALDataType type,
                  double *low, double *high)
{
  switch (style.stretch)
  {
    case RasterStyle::Stretch::None:
      typeRange(type, low, high);
      return;

    case RasterStyle::Stretch::MinMax:
      if (style.useCustomRange && style.maxValue > style.minValue)
      {
        *low = style.minValue;
        *high = style.maxValue;
        return;
      }
      if (stats.valid && stats.max > stats.min)
      {
        *low = stats.min;
        *high = stats.max;
        return;
      }
      typeRange(type, low, high);
      return;

    case RasterStyle::Stretch::PercentClip:
      if (stats.valid && stats.p98 > stats.p2)
      {
        *low = stats.p2;
        *high = stats.p98;
        return;
      }
      typeRange(type, low, high);
      return;
  }

  typeRange(type, low, high);
}

unsigned char valueToByte(double value, double low, double high)
{
  if (!(high > low) || !std::isfinite(value))
    return 0;
  const double normalized = (value - low) / (high - low);
  return static_cast<unsigned char>(std::lround(std::clamp(normalized, 0.0, 1.0) * 255.0));
}

/** Tabla de 256 entradas para una banda Byte con el estiramiento configurado. */
std::array<unsigned char, 256> buildByteLut(const RasterLayer::BandStats &stats, const RasterStyle &style)
{
  std::array<unsigned char, 256> lut {};
  double low = 0.0;
  double high = 255.0;
  stretchRange(stats, style, GDT_Byte, &low, &high);
  for (int value = 0; value < 256; ++value)
    lut[static_cast<size_t>(value)] = valueToByte(value, low, high);
  return lut;
}

/**
 * Colores de una banda paletizada. Devuelve false si la banda no tiene tabla de
 * color. El alfa de cada entrada se respeta (indice 0 = transparente en los
 * datos de ejemplo).
 */
bool paletteFor(GDALRasterBandH band, std::array<QRgb, 256> *colors, std::array<unsigned char, 256> *alphas)
{
  GDALColorTableH table = GDALGetRasterColorTable(band);
  if (!table)
    return false;

  const int count = std::min(256, GDALGetColorEntryCount(table));
  for (int i = 0; i < 256; ++i)
  {
    (*colors)[static_cast<size_t>(i)] = qRgba(0, 0, 0, 0);
    (*alphas)[static_cast<size_t>(i)] = 0;
  }

  for (int i = 0; i < count; ++i)
  {
    const GDALColorEntry *entry = GDALGetColorEntry(table, i);
    if (!entry)
      continue;
    (*colors)[static_cast<size_t>(i)] =
      qRgba(static_cast<int>(entry->c1), static_cast<int>(entry->c2), static_cast<int>(entry->c3), 255);
    (*alphas)[static_cast<size_t>(i)] =
      static_cast<unsigned char>(std::clamp(static_cast<int>(entry->c4), 0, 255));
  }

  return true;
}

} // namespace

void RasterRenderer::resetStats()
{
  mStats = FrameStats();
}

void RasterRenderer::paint(QPainter &painter, const std::vector<std::shared_ptr<RasterLayer>> &layers,
                           const Rectangle &extent, const QSize &size, const Options &options, int destinationEpsg)
{
  resetStats();

  for (const std::shared_ptr<RasterLayer> &layer : layers)
  {
    if (!layer)
      continue;
    paintLayer(painter, *layer, extent, size, options, destinationEpsg);
  }
}

void RasterRenderer::paintLayer(QPainter &painter, const RasterLayer &layer, const Rectangle &extent,
                                const QSize &size, const Options &options, int destinationEpsg,
                                const RasterStyle *styleOverride)
{
  if (!layer.isVisible() || !layer.extent().isValid() || !extent.isValid() || size.isEmpty())
    return;

  const MapToPixel transform(extent, size);
  if (!transform.isValid())
    return;

  // Tamano de pixel del lienzo: fija la rejilla de los VRT reproyectados para
  // que todas las hojas de un mosaico compartan reticula (ver RasterLayer). En
  // borrador se reutiliza la rejilla que ya tengan: el gesto no recrea warps.
  double warpPixelSize = 0.0;
  if (!options.draft)
  {
    warpPixelSize = options.warpPixelSize > 0.0 ? options.warpPixelSize : 1.0 / transform.pixelsPerMapUnit();
  }

  bool created = false;
  const RasterLayer::View *view = layer.viewForEpsg(destinationEpsg, warpPixelSize, &created);
  if (!view || view->width <= 0 || view->height <= 0)
    return;
  if (created)
    ++mStats.warpsCreated;

  // Extension de la vista (ya en el SRC del lienzo) a pixeles de pantalla.
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  const double pixelCorners[4][2] = {{0.0, 0.0},
                                     {static_cast<double>(view->width), 0.0},
                                     {0.0, static_cast<double>(view->height)},
                                     {static_cast<double>(view->width), static_cast<double>(view->height)}};
  for (const auto &pixel : pixelCorners)
  {
    const double x = view->geoTransform[0] + pixel[0] * view->geoTransform[1] + pixel[1] * view->geoTransform[2];
    const double y = view->geoTransform[3] + pixel[0] * view->geoTransform[4] + pixel[1] * view->geoTransform[5];
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minY = std::min(minY, y);
    maxY = std::max(maxY, y);
  }

  const QPointF topLeft = transform.transform(minX, maxY);
  const QPointF bottomRight = transform.transform(maxX, minY);

  QRectF target(topLeft, bottomRight);
  target = target.normalized();
  target = target.intersected(QRectF(0.0, 0.0, size.width(), size.height()));
  if (target.isEmpty())
    return;

  QRectF destination;
  const RasterStyle &style = styleOverride ? *styleOverride : layer.style();
  const QImage image =
    compose(layer, style, target, transform, destinationEpsg, warpPixelSize, options.draft, mStats, &destination);
  if (image.isNull() || destination.isEmpty())
    return;

  painter.save();
  painter.setOpacity(std::clamp(style.opacity, 0, 100) / 100.0);
  painter.setRenderHint(QPainter::SmoothPixmapTransform,
                        style.resampling == RasterStyle::Resampling::Bilinear);

  // Recorte a la huella real de la hoja: el VRT reproyectado es un
  // rectangulo que la contiene y sus margenes (negros o NoData) taparian las
  // hojas vecinas. Dos hojas contiguas comparten el borde, asi que el recorte
  // reparte los pixeles de la union sin dejar hueco ni solape.
  if (view->footprint.size() >= 3)
  {
    QPainterPath clip;
    clip.moveTo(transform.transform(view->footprint.front().x(), view->footprint.front().y()));
    for (size_t i = 1; i < view->footprint.size(); ++i)
      clip.lineTo(transform.transform(view->footprint[i].x(), view->footprint[i].y()));
    clip.closeSubpath();

    // La huella se engorda un pixel: dos hojas contiguas recortan dos poligonos
    // que comparten borde y, con el recorte exacto, la union de ambos puede
    // dejar una linea de un pixel del fondo del lienzo en la costura (medido).
    // Con el engorde una tapa a la otra; a opacidad 100 no se nota.
    QPainterPathStroker stroker;
    stroker.setWidth(1.5);
    clip = clip.united(stroker.createStroke(clip));

    painter.setClipPath(clip, Qt::IntersectClip);
  }

  painter.drawImage(destination, image);
  painter.restore();

  ++mStats.layersDrawn;
  ++mStats.tilesRead;
  mStats.pixelsDrawn += static_cast<qint64>(image.width()) * image.height();
}

void RasterRenderer::paintMosaic(QPainter &painter, const RasterMosaic &mosaic, const Rectangle &extent,
                                 const QSize &size, const Options &options, int destinationEpsg)
{
  if (!mosaic.isVisible() || mosaic.tileCount() == 0 || !extent.isValid() || size.isEmpty())
    return;

  const MapToPixel transform(extent, size);
  if (!transform.isValid())
    return;

  // La replica es un raster mas, ya en el SRC del lienzo: se dibuja sin warp y
  // GDAL elige el nivel de overview. Se usa mientras no haya que ampliarla mas
  // de 2x; por debajo de eso compensa leer las hojas (mas nitidas).
  const RasterStyle &style = mosaic.style();
  const double canvasPixel = 1.0 / transform.pixelsPerMapUnit();
  if (const std::shared_ptr<RasterLayer> replica = mosaic.replicaLayer())
  {
    const double replicaPixel = mosaic.replicaPixelSize();
    if (replicaPixel > 0.0 && replicaPixel <= 2.0 * canvasPixel)
    {
      RasterStyle replicaStyle = style;
      // El mosaico no tiene banda alfa; la replica si (los huecos de la
      // cobertura quedan transparentes en vez de negros).
      if (replica->bandCount() >= 4 && replicaStyle.alphaBand == 0)
        replicaStyle.alphaBand = 4;

      paintLayer(painter, *replica, extent, size, options, destinationEpsg, &replicaStyle);
      return;
    }
  }

  // Rejilla compartida por todas las hojas y cuantizada a potencias de 2: un
  // paso de zoom deja de rehacer el warp de cada hoja. La rejilla sigue siendo
  // la misma para todas, asi que las hojas contiguas siguen encajando.
  Options tileOptions = options;
  if (!options.draft)
  {
    const double exact = 1.0 / transform.pixelsPerMapUnit();
    tileOptions.warpPixelSize = std::exp2(std::round(std::log2(exact)));
  }

  const std::vector<Rectangle> &tileExtents = mosaic.tileExtents(destinationEpsg);
  const int budget = std::max(0, options.tileBudget);

  int visible = 0;
  int read = 0;

  for (int index = 0; index < mosaic.tileCount(); ++index)
  {
    // Descarte ANTES de pedir la vista: crear un warp por hoja fuera de la vista
    // fue el coste medido de cargar 508 imagenes como capas sueltas.
    if (index < static_cast<int>(tileExtents.size()))
    {
      const Rectangle &tileExtent = tileExtents[static_cast<size_t>(index)];
      if (!tileExtent.isValid() || !tileExtent.intersects(extent))
        continue;
    }

    ++visible;

    // Presupuesto de la fase de carga: el cuadro no puede esperar a las 508.
    if (budget > 0 && read >= budget)
      continue;

    const std::shared_ptr<RasterLayer> tile = mosaic.tile(index);
    if (!tile)
      continue;

    paintLayer(painter, *tile, extent, size, tileOptions, destinationEpsg, &style);
    ++read;
  }

  mStats.tilesVisible += visible;
}

QImage RasterRenderer::compose(const RasterLayer &layer, const RasterStyle &style, const QRectF &target,
                               const MapToPixel &transform, int destinationEpsg, double warpPixelSize,
                               bool draft, FrameStats &stats, QRectF *destination) const
{
  bool created = false;
  const RasterLayer::View *view = layer.viewForEpsg(destinationEpsg, draft ? 0.0 : warpPixelSize, &created);
  if (created)
    ++stats.warpsCreated;
  if (!view || !view->dataset)
    return {};

  const double *geoTransform = view->geoTransform;

  // Una geotransformacion con rotacion no se puede dibujar con un rectangulo
  // alineado a los ejes; se omite (los datos de trabajo son norte-arriba).
  if (std::fabs(geoTransform[2]) > 1e-12 || std::fabs(geoTransform[4]) > 1e-12)
    return {};

  const double determinant = geoTransform[1] * geoTransform[5] - geoTransform[2] * geoTransform[4];
  if (std::fabs(determinant) < 1e-15)
    return {};

  // Rectangulo de pixeles de origen que corresponde a lo que se ve en pantalla.
  const QPointF mapCorners[4] = {
    transform.toMap(target.topLeft()), transform.toMap(target.topRight()),
    transform.toMap(target.bottomRight()), transform.toMap(target.bottomLeft())};

  double minPixelX = std::numeric_limits<double>::max();
  double minPixelY = std::numeric_limits<double>::max();
  double maxPixelX = std::numeric_limits<double>::lowest();
  double maxPixelY = std::numeric_limits<double>::lowest();

  for (const QPointF &corner : mapCorners)
  {
    const double dx = corner.x() - geoTransform[0];
    const double dy = corner.y() - geoTransform[3];
    const double pixelX = (dx * geoTransform[5] - dy * geoTransform[2]) / determinant;
    const double pixelY = (-dx * geoTransform[4] + dy * geoTransform[1]) / determinant;

    minPixelX = std::min(minPixelX, pixelX);
    maxPixelX = std::max(maxPixelX, pixelX);
    minPixelY = std::min(minPixelY, pixelY);
    maxPixelY = std::max(maxPixelY, pixelY);
  }

  const int sourceX = std::max(0, static_cast<int>(std::floor(minPixelX)));
  const int sourceY = std::max(0, static_cast<int>(std::floor(minPixelY)));
  const int sourceRight = std::min(view->width, static_cast<int>(std::ceil(maxPixelX)));
  const int sourceBottom = std::min(view->height, static_cast<int>(std::ceil(maxPixelY)));

  if (sourceRight <= sourceX || sourceBottom <= sourceY)
    return {};

  // La ventana de origen se ajusta a pixeles ENTEROS, asi que en pantalla ocupa
  // un rectangulo algo mayor que `target` (hasta un pixel de origen por lado).
  // Hay que dibujar la imagen en ese rectangulo y no en `target`: estirarla
  // hasta el recorte visible desplazaba el contenido hasta un pixel y hacia que
  // dos hojas contiguas no encajaran (el desfase entre imagenes).
  const double left = geoTransform[0] + sourceX * geoTransform[1];
  const double top = geoTransform[3] + sourceY * geoTransform[5];
  const double right = geoTransform[0] + sourceRight * geoTransform[1];
  const double bottom = geoTransform[3] + sourceBottom * geoTransform[5];
  *destination = QRectF(transform.transform(left, top), transform.transform(right, bottom)).normalized();
  if (destination->isEmpty())
    return {};

  const QSize targetSize =
    draft ? QSize(std::max(1, static_cast<int>(destination->width()) / 2),
                  std::max(1, static_cast<int>(destination->height()) / 2))
          : QSize(std::max(1, static_cast<int>(std::lround(destination->width()))),
                  std::max(1, static_cast<int>(std::lround(destination->height()))));

  const bool grayscale = style.bandMode == RasterStyle::BandMode::Grayscale;
  const int channelCount = grayscale ? 1 : 3;
  const int bandNumbers[3] = {style.redBand, style.greenBand, style.blueBand};
  for (int i = 0; i < channelCount; ++i)
  {
    if (bandNumbers[i] < 1 || bandNumbers[i] > layer.bandCount())
      return {};
  }

  // Lectura de las bandas de color, una llamada por banda. Se probo a leer las
  // tres de una vez con GDALDatasetRasterIO y no compensa: en un VRT reproyectado
  // GDAL hace un warp por banda igualmente, y en la vista nativa el ahorro no
  // paga el riesgo de un camino distinto por driver. Lo que si se explota es el
  // descarte y el warp por nivel (ver paintMosaic).
  const int readWidth = sourceRight - sourceX;
  const int readHeight = sourceBottom - sourceY;

  std::vector<std::vector<unsigned char>> buffers;
  std::vector<GDALDataType> types;
  buffers.reserve(static_cast<size_t>(channelCount));

  for (int i = 0; i < channelCount; ++i)
  {
    GDALRasterBandH band = GDALGetRasterBand(view->dataset, bandNumbers[i]);
    if (!band)
      return {};

    const GDALDataType type = GDALGetRasterDataType(band);
    const int bytesPerSample = GDALGetDataTypeSizeBytes(type);
    if (bytesPerSample <= 0)
      return {};

    const qint64 bufferSize = static_cast<qint64>(targetSize.width()) * targetSize.height() * bytesPerSample;
    buffers.emplace_back(static_cast<size_t>(bufferSize));

    if (GDALRasterIO(band, GF_Read, sourceX, sourceY, readWidth, readHeight, buffers.back().data(),
                     targetSize.width(), targetSize.height(), type, 0, 0)
        != CE_None)
      return {};

    types.push_back(type);
    stats.bytesRead += bufferSize;
  }

  // Alfa opcional de la propia capa (se lee como Byte; GDAL la convierte si
  // hace falta). Los margenes del VRT reproyectado se recortan en `paint` con
  // la huella, no con una banda alfa: un alfa por capa no reparte los pixeles
  // de la union entre dos hojas y dejaria una linea de fondo en la costura.
  std::vector<unsigned char> alphaBuffer;
  const bool hasAlpha = style.alphaBand > 0 && style.alphaBand <= layer.bandCount()
                        && GDALGetRasterBand(view->dataset, style.alphaBand) != nullptr;
  if (hasAlpha)
  {
    alphaBuffer.resize(static_cast<size_t>(targetSize.width()) * targetSize.height());
    if (GDALRasterIO(GDALGetRasterBand(view->dataset, style.alphaBand), GF_Read, sourceX, sourceY, readWidth,
                     readHeight, alphaBuffer.data(), targetSize.width(), targetSize.height(), GDT_Byte, 0, 0)
        != CE_None)
      alphaBuffer.clear();
    stats.bytesRead += static_cast<qint64>(alphaBuffer.size());
  }

  const int width = targetSize.width();
  const int height = targetSize.height();
  const RasterLayer::BandInfo &redInfo = layer.bands().at(bandNumbers[0] - 1);

  QImage image(width, height, QImage::Format_ARGB32_Premultiplied);

  // Paleta del archivo: el indice se traduce con la tabla de color, sin
  // estiramiento (los datos ya son categorias, no intensidades).
  std::array<QRgb, 256> palette {};
  std::array<unsigned char, 256> paletteAlpha {};
  const bool usePalette = grayscale && paletteFor(GDALGetRasterBand(view->dataset, bandNumbers[0]), &palette,
                                                  &paletteAlpha);

  // Camino rapido: bandas Byte con tabla de 256 entradas.
  if (std::all_of(types.cbegin(), types.cend(), [](GDALDataType type) { return type == GDT_Byte; }))
  {
    std::array<std::array<unsigned char, 256>, 3> luts {};
    for (int c = 0; c < channelCount; ++c)
      luts[static_cast<size_t>(c)] = buildByteLut(layer.bandStats(bandNumbers[c]), style);

    for (int y = 0; y < height; ++y)
    {
      auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
      for (int x = 0; x < width; ++x)
      {
        const qint64 index = static_cast<qint64>(y) * width + x;
        const int r0 = static_cast<int>(buffers[0][static_cast<size_t>(index)]);
        int a = hasAlpha && !alphaBuffer.empty() ? alphaBuffer[static_cast<size_t>(index)] : 255;
        QRgb color = 0;

        if (usePalette)
        {
          const size_t paletteIndex = static_cast<size_t>(r0);
          color = palette[paletteIndex];
          a = std::min(a, static_cast<int>(paletteAlpha[paletteIndex]));
        }
        else
        {
          const int r = luts[0][static_cast<size_t>(r0)];
          const int g = grayscale ? r : luts[1][static_cast<size_t>(buffers[1][static_cast<size_t>(index)])];
          const int b = grayscale ? r : luts[2][static_cast<size_t>(buffers[2][static_cast<size_t>(index)])];
          color = qRgba(r, g, b, 255);
        }

        if (valueIsNoData(r0, redInfo, style.useNodata))
          a = 0;

        line[x] = a == 0 ? qRgba(0, 0, 0, 0) : qPremultiply(qRgba(qRed(color), qGreen(color), qBlue(color), a));
      }
    }

    return image;
  }

  // Camino generico: cada muestra se convierte con el rango de estiramiento de
  // su banda (tipos de 16/32 bits o coma flotante).
  double low[3] = {0.0, 0.0, 0.0};
  double high[3] = {255.0, 255.0, 255.0};
  for (int c = 0; c < channelCount; ++c)
    stretchRange(layer.bandStats(bandNumbers[c]), style, types[c], &low[c], &high[c]);

  for (int y = 0; y < height; ++y)
  {
    auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < width; ++x)
    {
      const qint64 index = static_cast<qint64>(y) * width + x;

      const double raw = sampleAt(buffers[0].data(), types[0], index);
      const unsigned char r = valueToByte(raw, low[0], high[0]);
      const unsigned char g = grayscale ? r : valueToByte(sampleAt(buffers[1].data(), types[1], index), low[1], high[1]);
      const unsigned char b = grayscale ? r : valueToByte(sampleAt(buffers[2].data(), types[2], index), low[2], high[2]);

      int a = hasAlpha && !alphaBuffer.empty() ? alphaBuffer[index] : 255;
      if (valueIsNoData(raw, redInfo, style.useNodata))
        a = 0;

      line[x] = a == 0 ? qRgba(0, 0, 0, 0) : qPremultiply(qRgba(r, g, b, a));
    }
  }

  return image;
}

} // namespace minigis
