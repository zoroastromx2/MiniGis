#include "minigisrasterrenderer.h"

#include "minigisrasterlayer.h"
#include "minigisrasterstyle.h"
#include "minigismaprenderer.h"

#include <QPainter>
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

void RasterRenderer::paint(QPainter &painter, const std::vector<std::shared_ptr<RasterLayer>> &layers,
                           const Rectangle &extent, const QSize &size, const Options &options, int destinationEpsg)
{
  mStats = FrameStats();
  if (layers.empty() || !extent.isValid() || size.isEmpty())
    return;

  const MapToPixel transform(extent, size);
  if (!transform.isValid())
    return;

  for (const std::shared_ptr<RasterLayer> &layer : layers)
  {
    if (!layer || !layer->isVisible() || !layer->extent().isValid())
      continue;

    const RasterLayer::View *view = layer->viewForEpsg(destinationEpsg);
    if (!view || view->width <= 0 || view->height <= 0)
      continue;

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
      continue;

    const QImage image = compose(*layer, target, transform, destinationEpsg, options.draft, mStats);
    if (image.isNull())
      continue;

    const RasterStyle &style = layer->style();
    painter.save();
    painter.setOpacity(std::clamp(style.opacity, 0, 100) / 100.0);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          style.resampling == RasterStyle::Resampling::Bilinear);
    painter.drawImage(target, image);
    painter.restore();

    ++mStats.layersDrawn;
    mStats.pixelsDrawn += static_cast<qint64>(image.width()) * image.height();
  }
}

QImage RasterRenderer::compose(const RasterLayer &layer, const QRectF &target, const MapToPixel &transform,
                               int destinationEpsg, bool draft, FrameStats &stats) const
{
  const RasterLayer::View *view = layer.viewForEpsg(destinationEpsg);
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

  const QSize targetSize = draft ? QSize(std::max(1, static_cast<int>(target.width()) / 2),
                                         std::max(1, static_cast<int>(target.height()) / 2))
                                 : QSize(std::max(1, static_cast<int>(std::lround(target.width()))),
                                         std::max(1, static_cast<int>(std::lround(target.height()))));

  const RasterStyle &style = layer.style();
  const bool grayscale = style.bandMode == RasterStyle::BandMode::Grayscale;
  const int channelCount = grayscale ? 1 : 3;
  const int bandNumbers[3] = {style.redBand, style.greenBand, style.blueBand};
  for (int i = 0; i < channelCount; ++i)
  {
    if (bandNumbers[i] < 1 || bandNumbers[i] > layer.bandCount())
      return {};
  }

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

    if (GDALRasterIO(band, GF_Read, sourceX, sourceY, sourceRight - sourceX, sourceBottom - sourceY,
                     buffers.back().data(), targetSize.width(), targetSize.height(), type, 0, 0)
        != CE_None)
      return {};

    types.push_back(type);
    stats.bytesRead += bufferSize;
  }

  // Banda alfa opcional (se lee como Byte; GDAL la convierte si hace falta).
  std::vector<unsigned char> alphaBuffer;
  const bool hasAlpha = style.alphaBand > 0 && style.alphaBand <= layer.bandCount()
                        && GDALGetRasterBand(view->dataset, style.alphaBand) != nullptr;
  if (hasAlpha)
  {
    alphaBuffer.resize(static_cast<size_t>(targetSize.width()) * targetSize.height());
    if (GDALRasterIO(GDALGetRasterBand(view->dataset, style.alphaBand), GF_Read, sourceX, sourceY,
                     sourceRight - sourceX, sourceBottom - sourceY, alphaBuffer.data(), targetSize.width(),
                     targetSize.height(), GDT_Byte, 0, 0)
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
        const int r0 = buffers[0][index];
        int a = hasAlpha && !alphaBuffer.empty() ? alphaBuffer[index] : 255;
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
          const int g = grayscale ? r : luts[1][static_cast<size_t>(buffers[1][index])];
          const int b = grayscale ? r : luts[2][static_cast<size_t>(buffers[2][index])];
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
