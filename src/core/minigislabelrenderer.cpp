#include "minigislabelrenderer.h"

#include "minigisgeometrystore.h"
#include "minigismaprenderer.h"
#include "minigisstyle.h"
#include "minigisvectorlayer.h"

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QHash>
#include <QImage>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace minigis
{
namespace
{

/** Tipos de anillo almacenados en GeometryStore::ringKinds(). */
constexpr uint8_t kKindPoint = 1;
constexpr uint8_t kKindPolygon = 3;

/** Lecturas de atributos por etiqueta colocada, como maximo. */
constexpr int kValueReadsPerLabel = 4;

/** Etiquetas que se intentan colocar como maximo por cada una que se dibuja. */
constexpr int kCandidatesPerLabel = 6;

/** Margen con el que se consulta el indice: el texto puede salir de la vista. */
constexpr double kQueryMarginPixels = 80.0;

/** Limite de textos cacheados (memoria acotada en sesiones largas). */
constexpr int kTextCacheLimit = 200000;

/**
 * Limites del tamano de fuente, en pixeles de pantalla.
 *
 * El maximo no es caprichoso: por encima de ahi el texto deja de leerse (ocupa mas
 * que la pantalla) y suele indicar un valor mal interpretado, asi que se recorta en
 * lugar de dibujar manchas negras.
 */
constexpr double kMinimumFontPixels = 1.0;
constexpr double kMaximumFontPixels = 400.0;

/**
 * Comprueba si la plataforma puede trazar el CONTORNO de los glifos.
 *
 * El halo se dibuja trazando el contorno del texto, que es lo que da el resultado
 * limpio y rapido. Hay motores de fuente que no dan contornos (la plataforma sin
 * ventana, por ejemplo): alli el trazado no pinta nada y las etiquetas quedarian
 * invisibles. En vez de suponerlo, se prueba una vez por familia y se recuerda.
 */
bool glyphOutlinesAvailable(const QFont &font)
{
  static QHash<QString, bool> cache;
  const QString key = font.family();
  const auto cached = cache.constFind(key);
  if (cached != cache.constEnd())
    return *cached;

  QImage probe(64, 32, QImage::Format_ARGB32_Premultiplied);
  probe.fill(Qt::white);

  {
    QPainter probePainter(&probe);
    QPainterPath path;
    path.addText(QPointF(4.0, 24.0), font, QStringLiteral("W"));
    probePainter.fillPath(path, Qt::black);
  }

  bool available = false;
  const QRgb white = qRgb(255, 255, 255);
  for (int y = 0; y < probe.height() && !available; ++y)
  {
    const QRgb *row = reinterpret_cast<const QRgb *>(probe.constScanLine(y));
    for (int x = 0; x < probe.width(); ++x)
    {
      if ((row[x] & 0x00FFFFFF) != (white & 0x00FFFFFF))
      {
        available = true;
        break;
      }
    }
  }

  cache.insert(key, available);
  return available;
}

/** Clave que identifica la configuracion de la que depende el texto cacheado. */
quint64 configurationKey(const VectorLayer *layer, const LabelStyle &style)
{
  quint64 key = reinterpret_cast<quint64>(layer) * 1099511628211ULL;
  for (const QString &field : style.fields)
    key ^= qHash(field) + 0x9E3779B97F4A7C15ULL + (key << 6) + (key >> 2);
  key ^= qHash(style.separator) + 0x9E3779B97F4A7C15ULL + (key << 6) + (key >> 2);
  return key;
}

/** Anade a `crossings` los cruces de la horizontal `scanY` con los lados del anillo. */
void collectCrossings(const double *xy, uint32_t count, double scanY, std::vector<double> &crossings)
{
  for (uint32_t i = 0; i < count; ++i)
  {
    const double x1 = xy[2 * i];
    const double y1 = xy[2 * i + 1];
    const uint32_t next = (i + 1) % count;
    const double x2 = xy[2 * next];
    const double y2 = xy[2 * next + 1];

    if ((y1 <= scanY && y2 > scanY) || (y2 <= scanY && y1 > scanY))
    {
      const double t = (scanY - y1) / (y2 - y1);
      crossings.push_back(x1 + t * (x2 - x1));
    }
  }
}

/**
 * Punto medio del tramo interior mas ancho, a partir de los cruces ordenados.
 * Los cruces se emparejan (regla par-impar), de modo que los huecos quedan fuera.
 */
bool widestInteriorPoint(std::vector<double> &crossings, double scanY, QPointF &point)
{
  if (crossings.size() < 2)
    return false;

  std::sort(crossings.begin(), crossings.end());

  double bestWidth = 0.0;
  double bestX = 0.0;
  for (size_t i = 0; i + 1 < crossings.size(); i += 2)
  {
    const double width = crossings[i + 1] - crossings[i];
    if (width > bestWidth)
    {
      bestWidth = width;
      bestX = (crossings[i] + crossings[i + 1]) / 2.0;
    }
  }

  if (bestWidth <= 0.0)
    return false;

  point = QPointF(bestX, scanY);
  return true;
}

} // namespace

bool LabelRenderer::CollisionGrid::collides(const QRectF &rect, double margin) const
{
  const QRectF padded = rect.adjusted(-margin, -margin, margin, margin);
  const int firstX = static_cast<int>(std::floor(padded.left() / kCellSize));
  const int lastX = static_cast<int>(std::floor(padded.right() / kCellSize));
  const int firstY = static_cast<int>(std::floor(padded.top() / kCellSize));
  const int lastY = static_cast<int>(std::floor(padded.bottom() / kCellSize));

  for (int cellY = firstY; cellY <= lastY; ++cellY)
  {
    for (int cellX = firstX; cellX <= lastX; ++cellX)
    {
      const qint64 key = (static_cast<qint64>(cellX) << 32) ^ static_cast<quint32>(cellY);
      const auto found = mCells.constFind(key);
      if (found == mCells.constEnd())
        continue;

      for (const QRectF &placed : *found)
      {
        // Se compara el rectangulo AMPLIADO: si se comparase el original, el
        // margen solo serviria para buscar en mas celdas y la distancia minima
        // entre etiquetas no tendria efecto (el parametro se ignoraba).
        if (padded.intersects(placed))
          return true;
      }
    }
  }

  return false;
}

void LabelRenderer::CollisionGrid::insert(const QRectF &rect)
{
  const int firstX = static_cast<int>(std::floor(rect.left() / kCellSize));
  const int lastX = static_cast<int>(std::floor(rect.right() / kCellSize));
  const int firstY = static_cast<int>(std::floor(rect.top() / kCellSize));
  const int lastY = static_cast<int>(std::floor(rect.bottom() / kCellSize));

  for (int cellY = firstY; cellY <= lastY; ++cellY)
  {
    for (int cellX = firstX; cellX <= lastX; ++cellX)
    {
      const qint64 key = (static_cast<qint64>(cellX) << 32) ^ static_cast<quint32>(cellY);
      mCells[key].append(rect);
    }
  }
}

void LabelRenderer::clearCache()
{
  mTextCache.clear();
  mTextCacheKey = 0;
}

bool LabelRenderer::anchorFor(const Layer &layer, uint32_t feature, QPointF &mapAnchor,
                              double &featureSize) const
{
  const GeometryStore &store = *layer.store;
  const std::vector<GeometryStore::Feature> &features = store.features();
  if (feature >= features.size())
    return false;

  const GeometryStore::Feature &entry = features[feature];
  featureSize = std::max(entry.maxX - entry.minX, entry.maxY - entry.minY);

  if (store.mode() == GeometryStore::Mode::Envelopes)
  {
    // Modo envolventes: no se lee la geometria para etiquetar. El centro de la
    // envolvente es una aproximacion suficiente y no cuesta ni una lectura.
    mapAnchor = QPointF((entry.minX + entry.maxX) / 2.0, (entry.minY + entry.maxY) / 2.0);
    return true;
  }

  // Modo memoria: el ancla se calcula con el primer anillo del elemento, que para
  // un punto es el punto y para un poligono o linea es su centro visual. Es mas
  // fiable que el centro de la envolvente en formas concavas.
  const GeometryStore::Level *level = store.acquireLevel(0);
  if (!level || feature >= store.entryFirstRing().size())
  {
    mapAnchor = QPointF((entry.minX + entry.maxX) / 2.0, (entry.minY + entry.maxY) / 2.0);
    return true;
  }

  const uint32_t firstRing = store.entryFirstRing()[feature];
  const uint32_t rings = store.entryRingCount()[feature];
  if (rings == 0 || firstRing >= level->ringCount.size())
  {
    mapAnchor = QPointF((entry.minX + entry.maxX) / 2.0, (entry.minY + entry.maxY) / 2.0);
    return true;
  }

  const uint32_t count = level->ringCount[firstRing];
  if (count == 0)
    return false;

  const double *xy = level->coords.data() + 2 * static_cast<size_t>(level->ringFirst[firstRing]);
  const uint8_t kind = store.ringKinds()[firstRing];

  if (kind == kKindPoint || count == 1)
  {
    mapAnchor = QPointF(xy[0], xy[1]);
    return true;
  }

  // Para poligonos se busca un punto INTERIOR: el centro de la envolvente (o el
  // promedio de los vertices) cae fuera de la forma en poligonos concavos y, sobre
  // todo, dentro de los huecos. Se corta el elemento con la horizontal de su
  // centro y se toma el tramo interior mas ancho, emparejando los cruces de TODOS
  // los anillos (exterior y huecos) para que los huecos queden descartados.
  if (kind == kKindPolygon && count >= 4)
  {
    double sumY = 0.0;
    for (uint32_t i = 0; i < count; ++i)
      sumY += xy[2 * i + 1];
    const double scanY = sumY / count;

    std::vector<double> crossings;
    crossings.reserve(count * 2);

    for (uint32_t ring = firstRing; ring < firstRing + rings && ring < level->ringCount.size(); ++ring)
    {
      const uint32_t ringPoints = level->ringCount[ring];
      if (ringPoints < 4 || store.ringKinds()[ring] != kKindPolygon)
        continue;

      collectCrossings(level->coords.data() + 2 * static_cast<size_t>(level->ringFirst[ring]), ringPoints,
                       scanY, crossings);
    }

    if (widestInteriorPoint(crossings, scanY, mapAnchor))
      return true;
  }

  double sumX = 0.0;
  double sumY = 0.0;
  for (uint32_t i = 0; i < count; ++i)
  {
    sumX += xy[2 * i];
    sumY += xy[2 * i + 1];
  }

  mapAnchor = QPointF(sumX / count, sumY / count);
  return true;
}

QString LabelRenderer::textFor(const Layer &layer, qint64 row, const QVector<int> &fieldIndices,
                               quint64 cacheKey)
{
  // La cache se invalida sola cuando cambian la capa, los campos o el separador.
  if (mTextCacheKey != cacheKey)
  {
    mTextCache.clear();
    mTextCacheKey = cacheKey;
  }

  const auto cached = mTextCache.constFind(row);
  if (cached != mTextCache.constEnd())
  {
    ++mStats.cacheHits;
    return *cached;
  }

  // Una sola lectura de GDAL por elemento y cuadro: fieldValues() lee el registro
  // completo de una vez (mucho mejor que pedir campo a campo).
  const QVector<QString> values = layer.layer->fieldValues(row);
  ++mStats.valueReads;

  QStringList parts;
  parts.reserve(fieldIndices.size());
  for (const int index : fieldIndices)
  {
    if (index < 0 || index >= values.size())
      continue;

    const QString value = values[index].trimmed();
    if (!value.isEmpty())
      parts << value;
  }

  const QString text = parts.join(layer.style->separator);

  // La cache de textos crece con lo que se navega; se acota para que una sesion
  // larga moviendose por una capa de millones de elementos no acumule memoria sin
  // limite. Al vaciarla solo se pierde el ahorro de lecturas de un cuadro.
  if (mTextCache.size() >= kTextCacheLimit)
    mTextCache.clear();

  mTextCache.insert(row, text);
  return text;
}

void LabelRenderer::paint(QPainter &painter, const std::vector<Layer> &layers,
                          const MapToPixel &transform, const QSize &size, const Options &options)
{
  mStats = FrameStats();
  mGrid.clear();
  mGlyphOutlines = glyphOutlinesAvailable(painter.font());

  if (!options.enabled || layers.empty() || !transform.isValid() || size.isEmpty())
    return;

  QElapsedTimer timer;
  timer.start();

  const double unitsPerPixel = std::max(transform.mapUnitsPerPixel(), 1e-15);
  const QPointF topLeft = transform.transform(transform.extent().xMin(), transform.extent().yMax());

  for (const Layer &layer : layers)
  {
    if (!layer.layer || !layer.store || !layer.style || !layer.style->enabled)
      continue;

    const LabelStyle &style = *layer.style;
    if (style.fields.isEmpty() || !style.isVisibleAtScale(unitsPerPixel))
      continue;

    ++mStats.layers;

    // --- Resolucion de los campos por nombre (una vez por capa y cuadro) ----
    const quint64 cacheKey = configurationKey(layer.layer, style);
    if (mFieldCacheKey != cacheKey)
    {
      mFieldIndices.clear();
      const QVector<VectorLayer::FieldInfo> &fields = layer.layer->fields();
      for (const QString &wanted : style.fields)
      {
        for (int i = 0; i < fields.size(); ++i)
        {
          if (fields[i].name.compare(wanted, Qt::CaseInsensitive) == 0)
          {
            mFieldIndices.append(i);
            break;
          }
        }
      }
      mFieldCacheKey = cacheKey;
    }

    if (mFieldIndices.isEmpty())
      continue;

    // --- Fuente ------------------------------------------------------------
    QFont font = painter.font();
    if (!style.fontFamily.isEmpty())
      font.setFamily(style.fontFamily);
    font.setBold(style.bold);
    font.setItalic(style.italic);

    if (style.sizeMode == LabelStyle::SizeMode::MapUnits)
    {
      // Tamano en unidades del mapa: el texto crece al acercarse.
      double pixels = style.fontSize * transform.pixelsPerMapUnit();
      if (pixels < kMinimumFontPixels || pixels > kMaximumFontPixels)
      {
        // Limite de seguridad. Sin el, un valor mal puesto (o interpretado como
        // unidades del mapa cuando se queria decir puntos) da un texto de miles de
        // pixeles que se ve como manchas negras en lugar de letras.
        pixels = std::clamp(pixels, kMinimumFontPixels, kMaximumFontPixels);
        ++mStats.sizeClamped;
      }

      font.setPixelSize(std::max(1, static_cast<int>(std::lround(pixels))));
    }
    else
    {
      font.setPointSizeF(std::clamp(style.fontSize, 0.5, kMaximumFontPixels));
    }

    const QFontMetricsF metrics(font);
    painter.setFont(font);

    // --- Elementos visibles -------------------------------------------------
    const double margin = kQueryMarginPixels * unitsPerPixel;
    const Rectangle &extent = transform.extent();
    mCandidates.clear();
    layer.store->query(extent.xMin() - margin, extent.yMin() - margin, extent.xMax() + margin,
                       extent.yMax() + margin, mCandidates);
    mStats.candidates += static_cast<qint64>(mCandidates.size());

    const qint64 limit = style.maximumLabels > 0 ? style.maximumLabels : static_cast<qint64>(mCandidates.size());
    const qint64 readLimit = limit * kValueReadsPerLabel;
    qint64 valueReadsBefore = mStats.valueReads;
    QSet<QString> placedTexts;

    for (const uint32_t feature : mCandidates)
    {
      if (mStats.placed >= limit)
        break;

      if (mStats.valueReads - valueReadsBefore >= readLimit)
      {
        mStats.skippedByLimit += 1;
        break;
      }

      if (options.draft && (feature % 2) != 0)
        continue; // en modo borrador se etiqueta uno de cada dos

      double featureSize = 0.0;
      QPointF mapAnchor;
      if (!anchorFor(layer, feature, mapAnchor, featureSize))
        continue;

      // Elementos demasiado pequenos: su etiqueta taparia el mapa sin aportar.
      // No se aplica a las capas de puntos: su envolvente es de tamano cero (el
      // elemento es un punto), asi que el filtro los descartaria todos; el tamano
      // que importa ahi es el del simbolo, no el de la geometria.
      const bool pointLayer = (layer.layer->geometryKind() == GeometryKind::Point);
      if (!pointLayer && style.minimumFeatureSize > 0.0
          && featureSize * transform.pixelsPerMapUnit() < style.minimumFeatureSize)
      {
        ++mStats.skippedSmall;
        continue;
      }

      const QString text = textFor(layer, static_cast<qint64>(feature), mFieldIndices, cacheKey);
      if (text.isEmpty())
      {
        ++mStats.skippedEmpty;
        continue;
      }

      if (style.discardDuplicates && placedTexts.contains(text))
      {
        ++mStats.skippedDuplicate;
        continue;
      }

      double anchorX = 0.0;
      double anchorY = 0.0;
      transform.projectInline(mapAnchor.x(), mapAnchor.y(), anchorX, anchorY);

      const QRectF bounds = metrics.boundingRect(text);
      QRectF labelRect;
      labelRect.setSize(bounds.size());

      switch (style.placement)
      {
        case LabelStyle::Placement::OverPoint:
          labelRect.moveCenter(QPointF(anchorX + style.offsetX, anchorY + style.offsetY));
          break;

        case LabelStyle::Placement::Line:
          labelRect.moveCenter(QPointF(anchorX + style.offsetX, anchorY + style.offsetY));
          break;

        case LabelStyle::Placement::AroundPoint:
        default:
          // Centrado horizontalmente y apoyado encima del elemento.
          labelRect.moveCenter(QPointF(anchorX + style.offsetX, anchorY + style.offsetY - labelRect.height() / 2.0));
          break;
      }

      if (style.minimumDistance > 0.0 && mGrid.collides(labelRect, style.minimumDistance))
      {
        ++mStats.skippedCollision;
        continue;
      }

      // --- Dibujado --------------------------------------------------------
      // Linea base del texto a partir del rectangulo medido.
      //
      // NO se puede componer con bounds.topLeft(): hay motores de fuente (la
      // plataforma sin ventana, por ejemplo) que devuelven un rectangulo con el
      // TAMANO correcto pero la POSICION descolocada (valores de decenas de miles),
      // y el texto acabaria dibujado fuera de la imagen. La ascent si es fiable, y
      // la linea base queda donde debe: el texto no se dibuja una ascent mas abajo
      // de la caja usada para detectar solapes.
      const double bearingX = std::abs(bounds.left()) <= bounds.height() ? bounds.left() : 0.0;
      const QPointF baseline(labelRect.left() - bearingX, labelRect.top() + metrics.ascent());

      bool drawn = false;
      if (style.haloEnabled && style.haloWidth > 0.0)
      {
        if (mGlyphOutlines)
        {
          QPainterPath path;
          path.addText(baseline, font, text);

          QPen haloPen(style.haloColor, style.haloWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
          painter.setPen(haloPen);
          painter.setBrush(Qt::NoBrush);
          painter.drawPath(path);
          painter.fillPath(path, style.color);
          drawn = true;
        }
        else
        {
          // Sin contornos de glifo el halo se consigue dibujando el texto varias
          // veces alrededor, en el color del halo, y el texto encima. Cuesta mas
          // llamadas, pero es la diferencia entre ver la etiqueta o no verla.
          painter.setPen(style.haloColor);
          const int steps = std::max(1, static_cast<int>(std::lround(style.haloWidth)));
          for (int dy = -steps; dy <= steps; ++dy)
          {
            for (int dx = -steps; dx <= steps; ++dx)
            {
              if (dx != 0 || dy != 0)
                painter.drawText(baseline + QPointF(dx, dy), text);
            }
          }

          ++mStats.textFallbacks;
        }
      }

      if (!drawn)
      {
        painter.setPen(style.color);
        painter.drawText(baseline, text);
      }

      mGrid.insert(labelRect);

      if (style.discardDuplicates)
        placedTexts.insert(text);

      ++mStats.placed;
    }
  }

  mStats.paintMs = timer.elapsed();
}

} // namespace minigis
