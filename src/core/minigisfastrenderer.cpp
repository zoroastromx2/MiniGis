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

void FastMapRenderer::buildBatch(std::vector<Batch> &batches, const GeometryStore &store,
                                 const GeometryStore::Level &level, const MapToPixel &transform,
                                 qint64 pointStride) const
{
  const std::vector<uint16_t> &styleOf = store.featureStyles();
  buildBatch(batches, level, store.ringKinds(), store.entryFirstRing(), store.entryRingCount(),
             mCandidates, transform, pointStride, styleOf.empty() ? nullptr : &styleOf);
}

void FastMapRenderer::buildBatch(std::vector<Batch> &batches, const GeometryStore::Level &level,
                                 const std::vector<uint8_t> &ringKinds,
                                 const std::vector<uint32_t> &entryFirstRing,
                                 const std::vector<uint32_t> &entryRingCount,
                                 const std::vector<uint32_t> &entries, const MapToPixel &transform,
                                 qint64 pointStride, const std::vector<uint16_t> *styleOf) const
{
  const double *coords = level.coords.data();
  const uint32_t *ringFirst = level.ringFirst.data();
  const uint32_t *ringCount = level.ringCount.data();

  const size_t groups = batches.empty() ? 1 : batches.size();

  // Estado de troceado por grupo de estilo: un QPainterPath solo se puede
  // dibujar con una pluma y un pincel, asi que cada estilo lleva su propio
  // trazado en curso. Con un solo estilo esto es exactamente el bucle de antes.
  std::vector<QPainterPath *> polygonPaths(groups, nullptr);
  std::vector<QPainterPath *> linePaths(groups, nullptr);
  std::vector<int> polygonCounts(groups, 0);
  std::vector<int> lineCounts(groups, 0);

  const int chunkLimit = featuresPerChunkFor(entries.size());
  qint64 pointCounter = 0;

  for (const uint32_t entryIndex : entries)
  {
    if (entryIndex >= entryFirstRing.size())
      continue;

    size_t group = 0;
    if (styleOf && entryIndex < styleOf->size())
      group = std::min<size_t>((*styleOf)[entryIndex], groups - 1);
    Batch &batch = batches[group];

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
      QPainterPath *&path = isPolygon ? polygonPaths[group] : linePaths[group];
      int &featuresInChunk = isPolygon ? polygonCounts[group] : lineCounts[group];

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
      if (polygonPaths[group])
        ++polygonCounts[group];
      if (linePaths[group])
        ++lineCounts[group];
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

    // Un lote por estilo: la simbologia importada del archivo (KML) puede variar
    // de un elemento a otro y un QPainterPath solo se dibuja con una pluma y un
    // pincel. Con un solo estilo hay un unico lote y el coste es el de siempre.
    const bool useStoreStyles = layer.useStoreStyles && !store.styles().empty();
    const size_t styleCount = useStoreStyles ? store.styles().size() : 1;
    std::vector<Batch> batches(styleCount);

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
        // El indice de estilo va por elemento del archivo y aqui las entradas son
        // posiciones del subconjunto, asi que no se puede aplicar: el modo
        // envolventes es de shapefile y no trae simbologia importada.
        buildBatch(batches, *subset->level, subset->entryRingKind, subset->entryFirstRing,
                   subset->entryRingCount, mPositions, transform, 1, nullptr);
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
      // En modo memoria tambien hay elementos sub-pixel: una capa de 711 000
      // lineas vista entera deja cada rio en menos de dos pixeles. Dibujarlos
      // como trazados cuesta cientos de milisegundos y no aporta nada; se
      // dibujan como puntos (muestreados) y solo los que se ven se trazan.
      mDots.clear();
      std::vector<uint32_t> pathCandidates;
      pathCandidates.reserve(mCandidates.size());

      const double minimumSize =
        (options.minimumGeometryPixels > 0.0) ? options.minimumGeometryPixels * unitsPerPixel : 0.0;
      if (minimumSize > 0.0)
      {
        const std::vector<GeometryStore::Feature> &features = store.features();
        const std::vector<uint8_t> &ringKinds = store.ringKinds();
        for (const uint32_t fid : mCandidates)
        {
          if (fid >= features.size())
            continue;

          const GeometryStore::Feature &feature = features[fid];
          const bool pointKind =
            feature.firstRing < ringKinds.size() && ringKinds[feature.firstRing] == kKindPoint;
          const bool subPixel = !pointKind && (feature.maxX - feature.minX) < minimumSize
                                && (feature.maxY - feature.minY) < minimumSize;
          if (!subPixel)
          {
            pathCandidates.push_back(fid);
            continue;
          }

          double x = 0.0;
          double y = 0.0;
          transform.projectInline((feature.minX + feature.maxX) / 2.0,
                                  (feature.minY + feature.maxY) / 2.0, x, y);
          mDots.emplace_back(x, y);
        }
      }
      else
      {
        pathCandidates = mCandidates;
      }

      const std::vector<uint16_t> &styleOf = store.featureStyles();
      for (int attempt = 0;; ++attempt)
      {
        const GeometryStore::Level *levelData = store.acquireLevel(level);
        if (!levelData)
          break;

        batches.assign(styleCount, Batch());

        timer.restart();
        buildBatch(batches, *levelData, store.ringKinds(), store.entryFirstRing(),
                   store.entryRingCount(), pathCandidates, transform, pointStride,
                   styleOf.empty() ? nullptr : &styleOf);
        mStats.pathMs += timer.elapsed();

        mStats.level = level;
        mStats.tolerance = levelData->tolerance;

        qint64 batchVertices = 0;
        for (const Batch &built : batches)
          batchVertices += built.vertices;

        const bool fits = (options.vertexBudget <= 0) || (batchVertices <= options.vertexBudget);
        if (fits || level >= maximumLevel)
          break;

        // La subida de nivel depende del numero de vertices de la vista, no solo de
        // la escala: el cuadro deja de ser una traslacion pura del anterior.
        mStats.viewDependent = true;
        ++level;
      }
    }

    qint64 drawnFeatures = 0;
    qint64 drawnVertices = 0;
    for (const Batch &built : batches)
    {
      drawnFeatures += built.features;
      drawnVertices += built.vertices;
    }
    mStats.drawnFeatures += drawnFeatures;
    mStats.drawnVertices += drawnVertices;

    timer.restart();
    painter.setOpacity(std::clamp(style.opacity / 100.0, 0.0, 1.0));

    // Escalon de calidad del contorno: hairline cuando hay muchos elementos o
    // cuando se esta interactuando (ver Options::hairlineFeatureThreshold).
    // La decision se cuantiza (escalones de 1024 elementos) para que no dependa del
    // encuadre exacto: al desplazar el mapa el numero de elementos visibles cambia
    // un poco y, si la decision oscilara entre cuadros, la parte copiada del cuadro
    // anterior y la franja nueva se dibujarian con estilos distintos.
    const qint64 quantizedFeatures = (drawnFeatures / 1024) * 1024;
    const bool hairlineStroke =
      options.draft || (options.hairlineFeatureThreshold > 0
                        && quantizedFeatures >= options.hairlineFeatureThreshold);

    // Un grupo por estilo. Con una sola simbologia (lo habitual) esto es una
    // vuelta y el dibujado es identico al de antes de existir los estilos.
    for (size_t group = 0; group < batches.size(); ++group)
    {
      const Batch &batch = batches[group];
      if (batch.polygonChunks.empty() && batch.lineChunks.empty() && batch.points.empty())
        continue;

      const Style &drawStyle = useStoreStyles ? store.styles()[group] : style;
      painter.setOpacity(std::clamp(drawStyle.opacity / 100.0, 0.0, 1.0));

      if (!batch.polygonChunks.empty())
      {
        // El troceado ya fija la regla de relleno al empezar cada trazado.
        painter.setBrush(drawStyle.fillEnabled ? QBrush(drawStyle.fillColor) : Qt::NoBrush);
        painter.setPen(strokePen(drawStyle, hairlineStroke));
        for (const QPainterPath &path : batch.polygonChunks)
          painter.drawPath(path);
      }

      if (!batch.lineChunks.empty())
      {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(hairlineStroke
                         ? QPen(drawStyle.strokeColor, 0.0)
                         : QPen(drawStyle.strokeColor, std::max(drawStyle.strokeWidth, 0.1), Qt::SolidLine,
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
          painter.setPen(QPen(drawStyle.strokeColor, 0.0));
          painter.drawPoints(points, pointTotal);
          painter.setRenderHint(QPainter::Antialiasing, options.antialiasing);
        }
        else
        {
          const double diameter = std::max(1.0, drawStyle.pointSize);

          if (drawStyle.strokeWidth > 0.0)
          {
            painter.setPen(QPen(drawStyle.strokeColor, diameter, Qt::SolidLine, Qt::RoundCap));
            painter.drawPoints(points, pointTotal);
          }

          const double innerDiameter = std::max(0.1, diameter - 2.0 * drawStyle.strokeWidth);
          painter.setPen(QPen(drawStyle.fillColor, innerDiameter, Qt::SolidLine, Qt::RoundCap));
          painter.drawPoints(points, pointTotal);
        }
      }
    }

    // Nube de elementos sub-pixel del modo memoria (el modo envolventes ya la
    // dibuja en su rama). Se muestrea si hay mas puntos que el presupuesto: a
    // esta escala la densidad es lo unico que aporta informacion.
    if (store.mode() == GeometryStore::Mode::InMemory && !mDots.empty())
    {
      if (options.dotBudget > 0 && static_cast<qint64>(mDots.size()) > options.dotBudget)
      {
        const qint64 stride = (static_cast<qint64>(mDots.size()) + options.dotBudget - 1) / options.dotBudget;
        std::vector<QPointF> sampled;
        sampled.reserve(static_cast<size_t>(options.dotBudget));
        for (size_t i = 0; i < mDots.size(); i += static_cast<size_t>(stride))
          sampled.push_back(mDots[i]);
        mDots.swap(sampled);
        mStats.viewDependent = true;
      }

      const bool antialiasingWasOn = painter.testRenderHint(QPainter::Antialiasing);
      painter.setRenderHint(QPainter::Antialiasing, false);
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(style.strokeColor, 0.0));
      painter.drawPoints(mDots.data(), static_cast<int>(mDots.size()));
      painter.setRenderHint(QPainter::Antialiasing, antialiasingWasOn);

      mStats.dotFeatures += static_cast<qint64>(mDots.size());
      mStats.drawnFeatures += static_cast<qint64>(mDots.size());
    }

    painter.setOpacity(1.0);
    mStats.paintMs += timer.elapsed();
  }
}

} // namespace minigis
