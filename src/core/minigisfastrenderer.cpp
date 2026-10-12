#include "minigisfastrenderer.h"

#include "minigismaprenderer.h"
#include "minigisstyle.h"

#include <QBrush>
#include <QElapsedTimer>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace minigis
{
namespace
{

/** Tipos de anillo almacenados en GeometryStore::ringKinds(). */
constexpr uint8_t kKindPoint = 1;
constexpr uint8_t kKindLine = 2;
constexpr uint8_t kKindPolygon = 3;

/**
 * Pluma del contorno.
 *
 * Con `hairline` se devuelve una pluma cosmetica (grosor 0 = un pixel de
 * dispositivo) que Qt dibuja sin pasar por el contorneador: es la diferencia
 * entre 216 ms y 1.049 ms por cuadro con 12.000 poligonos. Para grosores de mas
 * de un pixel se usa el contorno normal con union a inglete, que evita generar
 * un arco en cada vertice.
 */
QPen strokePen(const Style &style, bool hairline)
{
  if (style.strokeWidth <= 0.0)
    return QPen(Qt::NoPen);

  if (hairline)
    return QPen(style.strokeColor, 0.0);

  return QPen(style.strokeColor, style.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
}

/**
 * Elementos por trazado.
 *
 * Medido sobre 12.000 poligonos (186.000 vertices): el coste del rellenado con
 * suavizado de Qt crece muy por encima de lo lineal con el numero de subpaths de
 * un mismo QPainterPath. Un unico trazado con 12.000 subpaths costaba ~7.700 ms;
 * en lotes de 256, ~1.000 ms; en lotes de 4, ~52 ms; en lotes de 1, ~63 ms. Con
 * pocos elementos, en cambio, el lote minimo es el mejor (16 ms con lotes de 1
 * frente a 53 ms con lotes de 16 en una vista con 180 elementos), porque el
 * coste fijo por trazado pesa mas que el numero de subpaths.
 *
 * De ahi la regla: lote de 1 cuando la vista tiene pocos elementos y de 4 cuando
 * tiene muchos, que es el mejor compromiso en las dos escalas.
 */
int featuresPerChunkFor(size_t candidateCount)
{
  return candidateCount <= 512 ? 1 : 4;
}

/** Prepara un trazado nuevo con la regla de relleno correcta. */
QPainterPath &startChunk(std::vector<QPainterPath> &chunks, int &featuresInChunk)
{
  chunks.emplace_back();
  chunks.back().setFillRule(Qt::WindingFill);
  featuresInChunk = 0;
  return chunks.back();
}

} // namespace

void FastMapRenderer::buildBatch(Batch &batch, const GeometryStore &store, const GeometryStore::Level &level,
                                 const MapToPixel &transform) const
{
  const std::vector<GeometryStore::Feature> &features = store.features();
  const std::vector<uint8_t> &ringKinds = store.ringKinds();

  const double *coords = level.coords.data();
  const uint32_t *ringFirst = level.ringFirst.data();
  const uint32_t *ringCount = level.ringCount.data();

  int polygonFeaturesInChunk = 0;
  int lineFeaturesInChunk = 0;
  QPainterPath *polygonPath = nullptr;
  QPainterPath *linePath = nullptr;
  const int chunkLimit = featuresPerChunkFor(mCandidates.size());

  for (const uint32_t featureIndex : mCandidates)
  {
    const GeometryStore::Feature &feature = features[featureIndex];
    const uint32_t lastRing = feature.firstRing + feature.ringCount;

    bool anyDrawn = false;
    for (uint32_t ring = feature.firstRing; ring < lastRing; ++ring)
    {
      const uint32_t count = ringCount[ring];
      const uint8_t kind = ringKinds[ring];

      // Minimo de vertices para que el anillo se pueda dibujar: un punto es un
      // vertice, una linea dos y un poligono cerrado cuatro.
      const uint32_t minimumVertices = (kind == kKindPoint) ? 1u : ((kind == kKindPolygon) ? 4u : 2u);
      if (count < minimumVertices)
        continue;

      const double *xy = coords + 2 * static_cast<size_t>(ringFirst[ring]);

      if (kind == kKindPoint)
      {
        double x = 0.0;
        double y = 0.0;
        transform.projectInline(xy[0], xy[1], x, y);
        batch.points.emplace_back(x, y);
        batch.vertices += 1;
        anyDrawn = true;
        continue;
      }

      const bool isPolygon = (kind == kKindPolygon);
      QPainterPath *&path = isPolygon ? polygonPath : linePath;
      int &featuresInChunk = isPolygon ? polygonFeaturesInChunk : lineFeaturesInChunk;

      // Cada cierto numero de elementos se empieza un trazado nuevo: un unico
      // QPainterPath con miles de subpaths degrada el rellenado de Qt.
      if (!path || featuresInChunk >= chunkLimit)
        path = &startChunk(isPolygon ? batch.polygonChunks : batch.lineChunks, featuresInChunk);

      double x = 0.0;
      double y = 0.0;
      transform.projectInline(xy[0], xy[1], x, y);
      path->moveTo(x, y);

      for (uint32_t i = 1; i < count; ++i)
      {
        transform.projectInline(xy[2 * i], xy[2 * i + 1], x, y);
        path->lineTo(x, y);
      }

      if (isPolygon)
        path->closeSubpath();

      batch.vertices += count;
      anyDrawn = true;
    }

    if (anyDrawn)
    {
      ++batch.features;
      if (polygonPath)
        ++polygonFeaturesInChunk;
      if (linePath)
        ++lineFeaturesInChunk;
    }
  }
}

QImage FastMapRenderer::render(const std::vector<Layer> &layers, const Rectangle &extent, const QSize &size,
                              const QColor &background, const Options &options)
{
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  image.fill(background);

  if (size.isEmpty())
    return image;

  QPainter painter(&image);
  paint(painter, layers, extent, size, options);
  painter.end();
  return image;
}

void FastMapRenderer::paint(QPainter &painter, const std::vector<Layer> &layers, const Rectangle &extent,
                           const QSize &size, const Options &options)
{
  const MapToPixel transform(extent, size);
  mStats = FrameStats();

  if (!extent.isValid() || size.isEmpty() || !transform.isValid())
    return;

  painter.setRenderHint(QPainter::Antialiasing, options.antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, false);  // El lienzo pinta en coordenadas de dispositivo: no hace falta que Qt vuelva a
  // transformar cada vertice, ya lo hemos proyectado nosotros.
  painter.setWorldTransform(QTransform());
  const double unitsPerPixel = std::max(transform.mapUnitsPerPixel(), 1e-15);
  const double wantedTolerance =
    (options.draft ? options.draftSimplifyPixels : options.simplifyPixels) * unitsPerPixel;
  const double margin = unitsPerPixel * 2.0;

  for (const Layer &layer : layers)
  {
    if (!layer.store || !layer.style || !layer.visible)
      continue;

    const GeometryStore &store = *layer.store;
    const Style &style = *layer.style;

    mStats.totalFeatures += static_cast<qint64>(store.features().size());

    QElapsedTimer timer;
    timer.start();
    mCandidates.clear();
    store.query(extent.xMin() - margin, extent.yMin() - margin, extent.xMax() + margin, extent.yMax() + margin,
                mCandidates);
    mStats.queryMs += timer.elapsed();
    mStats.candidateFeatures += static_cast<qint64>(mCandidates.size());

    if (mCandidates.empty())
      continue;

    int level = store.levelForTolerance(wantedTolerance);
    mStats.level = level;
    mStats.tolerance = wantedTolerance;

    // Se construye el lote del nivel pedido; si el detalle sigue siendo excesivo
    // para el presupuesto de vertices, se sube un nivel de simplificacion.
    const int maximumLevel = std::min(store.maximumLevel(), level + std::max(0, options.maximumLevelAttempts));
    Batch batch;
    for (int attempt = 0;; ++attempt)
    {
      const GeometryStore::Level *levelData = store.acquireLevel(level);
      if (!levelData)
        break;

      batch = Batch();

      timer.restart();
      buildBatch(batch, store, *levelData, transform);
      mStats.pathMs += timer.elapsed();

      mStats.level = level;
      mStats.tolerance = levelData->tolerance;

      const bool fits = (options.vertexBudget <= 0) || (batch.vertices <= options.vertexBudget);
      if (fits || level >= maximumLevel)
        break;

      ++level;
    }

    mStats.drawnFeatures += batch.features;
    mStats.drawnVertices += batch.vertices;

    timer.restart();
    painter.setOpacity(std::clamp(style.opacity / 100.0, 0.0, 1.0));

    // Escalon de calidad del contorno: hairline cuando hay muchos elementos o
    // cuando se esta interactuando (ver Options::hairlineFeatureThreshold).
    const bool hairlineStroke =
      options.draft || (options.hairlineFeatureThreshold > 0
                        && batch.features >= options.hairlineFeatureThreshold);

    if (!batch.polygonChunks.empty())
    {
      for (QPainterPath &path : batch.polygonChunks)
        path.setFillRule(Qt::WindingFill);

      painter.setBrush(style.fillEnabled ? QBrush(style.fillColor) : Qt::NoBrush);
      painter.setPen(strokePen(style, hairlineStroke));
      for (const QPainterPath &path : batch.polygonChunks)
        painter.drawPath(path);
    }

    if (!batch.lineChunks.empty())
    {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(hairlineStroke ? QPen(style.strokeColor, 0.0)
                                    : QPen(style.strokeColor, std::max(style.strokeWidth, 0.1), Qt::SolidLine,
                                           Qt::RoundCap, Qt::RoundJoin));
      for (const QPainterPath &path : batch.lineChunks)
        painter.drawPath(path);
    }

    if (!batch.points.empty())
    {
      // Los marcadores se dibujan como puntos con extremo redondeado: un trazo
      // grueso con cap redondo es un circulo. Dos llamadas bastan para el
      // contorno y el relleno de todos los puntos de la capa.
      const double diameter = std::max(1.0, style.pointSize);
      const int pointTotal = static_cast<int>(batch.points.size());
      const QPointF *points = batch.points.data();

      painter.setBrush(Qt::NoBrush);

      if (style.strokeWidth > 0.0)
      {
        painter.setPen(QPen(style.strokeColor, diameter, Qt::SolidLine, Qt::RoundCap));
        painter.drawPoints(points, pointTotal);
      }

      const double innerDiameter = std::max(0.1, diameter - 2.0 * style.strokeWidth);
      painter.setPen(QPen(style.fillColor, innerDiameter, Qt::SolidLine, Qt::RoundCap));
      painter.drawPoints(points, pointTotal);
    }

    painter.setOpacity(1.0);
    mStats.paintMs += timer.elapsed();
  }
}

} // namespace minigis
