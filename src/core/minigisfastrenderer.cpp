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
                                 const MapToPixel &transform, qint64 pointStride) const
{
  buildBatch(batch, level, store.ringKinds(), store.entryFirstRing(), store.entryRingCount(), mCandidates,
             transform, pointStride);
}

void FastMapRenderer::buildBatch(Batch &batch, const GeometryStore::Level &level,
                                 const std::vector<uint8_t> &ringKinds,
                                 const std::vector<uint32_t> &entryFirstRing,
                                 const std::vector<uint32_t> &entryRingCount,
                                 const std::vector<uint32_t> &entries, const MapToPixel &transform,
                                 qint64 pointStride) const
{
  const double *coords = level.coords.data();
  const uint32_t *ringFirst = level.ringFirst.data();
  const uint32_t *ringCount = level.ringCount.data();

  int polygonFeaturesInChunk = 0;
  int lineFeaturesInChunk = 0;
  QPainterPath *polygonPath = nullptr;
  QPainterPath *linePath = nullptr;
  const int chunkLimit = featuresPerChunkFor(entries.size());
  qint64 pointCounter = 0;

  for (const uint32_t entryIndex : entries)
  {
    if (entryIndex >= entryFirstRing.size())
      continue;

    const uint32_t first = entryFirstRing[entryIndex];
    const uint32_t ringTotal = entryRingCount[entryIndex];
    const uint32_t lastRing = first + ringTotal;

    bool anyDrawn = false;
    for (uint32_t ring = first; ring < lastRing; ++ring)
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
        // Los marcadores se muestrean si la capa tiene millones de puntos: no se
        // pueden simplificar y dibujarlos todos cuesta segundos por cuadro.
        if (pointStride > 1 && (pointCounter++ % pointStride) != 0)
          continue;

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

    // Muestreo de marcadores: los puntos no se pueden simplificar, asi que el
    // numero de elementos visibles acota el trabajo del cuadro.
    const qint64 pointStride =
      (options.pointBudget > 0 && static_cast<qint64>(mCandidates.size()) > options.pointBudget)
        ? (static_cast<qint64>(mCandidates.size()) + options.pointBudget - 1) / options.pointBudget
        : 1;

    if (pointStride > 1)
      mStats.viewDependent = true;

    int level = store.levelForTolerance(wantedTolerance);
    mStats.level = level;
    mStats.tolerance = wantedTolerance;

    // Se construye el lote del nivel pedido; si el detalle sigue siendo excesivo
    // para el presupuesto de vertices, se sube un nivel de simplificacion.
    const int maximumLevel = std::min(store.maximumLevel(), level + std::max(0, options.maximumLevelAttempts));
    Batch batch;

    if (store.mode() == GeometryStore::Mode::Envelopes)
    {
      // Modo envolventes: la geometria de lo visible se lee del archivo bajo
      // demanda (con presupuesto por cuadro) y se simplifica al vuelo. Si a esta
      // escala cada elemento ocupa menos de un pixel, en lugar de leerlo se
      // dibuja su envolvente como punto.
      timer.restart();
      const double minimumGeometrySize =
        (options.minimumGeometryPixels > 0.0) ? options.minimumGeometryPixels * unitsPerPixel : 0.0;

      const std::shared_ptr<const GeometryStore::Subset> subset =
        store.acquireSubset(mCandidates, wantedTolerance, options.readBudget, minimumGeometrySize,
                            options.dotBudget);
      mStats.readFeatures += subset ? subset->readFeatures : 0;
      mStats.skippedFeatures += subset ? subset->missingFeatures - subset->readFeatures : 0;
      mStats.envelopeMode = true;
      mStats.queryMs += timer.elapsed();

      if (subset && subset->level && !subset->level->coords.empty())
      {
        mPositions.resize(subset->entryFirstRing.size());
        for (size_t i = 0; i < mPositions.size(); ++i)
          mPositions[i] = static_cast<uint32_t>(i);

        timer.restart();
        buildBatch(batch, *subset->level, subset->entryRingKind, subset->entryFirstRing,
                   subset->entryRingCount, mPositions, transform, 1);
        mStats.pathMs += timer.elapsed();
      }

      // La nube de puntos se muestrea segun el numero de elementos de la vista, de
      // modo que el cuadro no es una traslacion pura del anterior.
      if (subset && subset->missingFeatures > 0
          && static_cast<qint64>(subset->dotEntries.size()) < subset->missingFeatures)
        mStats.viewDependent = true;

      if (subset && !subset->dotEntries.empty())
      {
        // Nube de puntos con las envolventes: cada elemento sub-pixel se dibuja
        // en el centro de su envolvente.
        const std::vector<GeometryStore::Feature> &features = store.features();
        mDots.clear();
        mDots.reserve(subset->dotEntries.size());

        timer.restart();
        for (const uint32_t fid : subset->dotEntries)
        {
          if (fid >= features.size())
            continue;

          const GeometryStore::Feature &feature = features[fid];
          double x = 0.0;
          double y = 0.0;
          transform.projectInline((feature.minX + feature.maxX) / 2.0, (feature.minY + feature.maxY) / 2.0, x, y);
          mDots.emplace_back(x, y);
        }
        mStats.pathMs += timer.elapsed();
        mStats.dotFeatures += static_cast<qint64>(mDots.size());

        if (!mDots.empty())
        {
          // Nube de puntos: se dibuja con pluma cosmetica y sin suavizado. Con
          // suavizado, 150.000 puntos costaban ~200 ms de cuadro; sin el, unas
          // decimas. A un pixel de tamano la diferencia visual es despreciable.
          const bool antialiasingWasOn = painter.renderHints() & QPainter::Antialiasing;
          painter.setRenderHint(QPainter::Antialiasing, false);
          painter.setBrush(Qt::NoBrush);
          painter.setPen(QPen(style.strokeColor, 0.0));
          painter.drawPoints(mDots.data(), static_cast<int>(mDots.size()));
          painter.setRenderHint(QPainter::Antialiasing, antialiasingWasOn);
        }
      }
    }
    else
    {
      for (int attempt = 0;; ++attempt)
      {
        const GeometryStore::Level *levelData = store.acquireLevel(level);
        if (!levelData)
          break;

        batch = Batch();

        timer.restart();
        buildBatch(batch, store, *levelData, transform, pointStride);
        mStats.pathMs += timer.elapsed();

        mStats.level = level;
        mStats.tolerance = levelData->tolerance;

        const bool fits = (options.vertexBudget <= 0) || (batch.vertices <= options.vertexBudget);
        if (fits || level >= maximumLevel)
          break;

        // La subida de nivel depende del numero de vertices de la vista, no solo de
        // la escala: el cuadro deja de ser una traslacion pura del anterior.
        mStats.viewDependent = true;
        ++level;
      }
    }

    mStats.drawnFeatures += batch.features;
    mStats.drawnVertices += batch.vertices;

    timer.restart();
    painter.setOpacity(std::clamp(style.opacity / 100.0, 0.0, 1.0));

    // Escalon de calidad del contorno: hairline cuando hay muchos elementos o
    // cuando se esta interactuando (ver Options::hairlineFeatureThreshold).
    // La decision se cuantiza (escalones de 1024 elementos) para que no dependa del
    // encuadre exacto: al desplazar el mapa el numero de elementos visibles cambia
    // un poco y, si la decision oscilara entre cuadros, la parte copiada del cuadro
    // anterior y la franja nueva se dibujarian con estilos distintos.
    const qint64 quantizedFeatures = (batch.features / 1024) * 1024;
    const bool hairlineStroke =
      options.draft || (options.hairlineFeatureThreshold > 0
                        && quantizedFeatures >= options.hairlineFeatureThreshold);

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
      const int pointTotal = static_cast<int>(batch.points.size());
      const QPointF *points = batch.points.data();

      painter.setBrush(Qt::NoBrush);

      if (pointStride > 1)
      {
        // Capa de millones de puntos muestreada: a esta densidad el simbolo
        // completo no aporta informacion y cuesta. Se dibuja un punto de un
        // pixel, sin suavizado y con pluma cosmetica (medido con 115.000
        // puntos: 284 ms -> decenas de ms).
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(style.strokeColor, 0.0));
        painter.drawPoints(points, pointTotal);
        painter.setRenderHint(QPainter::Antialiasing, options.antialiasing);
      }
      else
      {
        const double diameter = std::max(1.0, style.pointSize);

        if (style.strokeWidth > 0.0)
        {
          painter.setPen(QPen(style.strokeColor, diameter, Qt::SolidLine, Qt::RoundCap));
          painter.drawPoints(points, pointTotal);
        }

        const double innerDiameter = std::max(0.1, diameter - 2.0 * style.strokeWidth);
        painter.setPen(QPen(style.fillColor, innerDiameter, Qt::SolidLine, Qt::RoundCap));
        painter.drawPoints(points, pointTotal);
      }
    }

    painter.setOpacity(1.0);
    mStats.paintMs += timer.elapsed();
  }
}

} // namespace minigis
