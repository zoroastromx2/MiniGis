#include "minigisgeometrystore.h"

#include "minigisvectorlayer.h"

#include <QElapsedTimer>
#include <QString>

#include <algorithm>
#include <cmath>
#include <numeric>

#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>

namespace minigis
{
namespace
{

/** Capacidad de una hoja/nodo del R-tree. */
constexpr uint32_t kNodeCapacity = 16;

/** Vertices minimos que debe conservar la simplificacion de cada anillo. */
constexpr uint32_t kMinimumRingVertices = 4;

/** Numero maximo de niveles de simplificacion que se mantienen en cache. */
constexpr int kMaximumCachedLevels = 10;

constexpr uint8_t kKindUnknown = 0;
constexpr uint8_t kKindPoint = 1;
constexpr uint8_t kKindLine = 2;
constexpr uint8_t kKindPolygon = 3;

/** Area con signo (positiva = sentido antihorario en coordenadas de mapa). */
double signedArea(const double *xy, int count)
{
  double sum = 0.0;
  for (int i = 0; i < count; ++i)
  {
    const double *current = xy + 2 * i;
    const double *next = xy + 2 * ((i + 1) % count);
    sum += current[0] * next[1] - next[0] * current[1];
  }
  return sum / 2.0;
}

/**
 * Distancia al cuadrado de un punto al segmento [a,b].
 *
 * Douglas-Peucker clasico mide la distancia a la recta infinita, que se dispara
 * con segmentos casi degenerados (anillos con vertices repetidos). La distancia
 * al segmento es mas estable y no cuesta mas.
 */
inline double distanceToSegmentSquared(double px, double py, double ax, double ay, double bx, double by)
{
  const double dx = bx - ax;
  const double dy = by - ay;
  const double lengthSquared = dx * dx + dy * dy;

  if (lengthSquared <= 0.0)
  {
    const double ex = px - ax;
    const double ey = py - ay;
    return ex * ex + ey * ey;
  }

  double t = ((px - ax) * dx + (py - ay) * dy) / lengthSquared;
  t = std::clamp(t, 0.0, 1.0);

  const double ex = px - (ax + t * dx);
  const double ey = py - (ay + t * dy);
  return ex * ex + ey * ey;
}

/**
 * Memorias intermedias de la simplificacion, reutilizadas entre anillos para no
 * reservar memoria en cada uno.
 */
struct SimplifyScratch
{
  std::vector<uint8_t> keep;
  std::vector<uint32_t> compact;
  std::vector<uint8_t> keepCompact;
  std::vector<std::pair<int, int>> stack;
};

/**
 * Simplificacion en dos fases sobre un anillo/polilinea en buffer plano.
 *
 *  1. Filtro radial O(n): se conserva un vertice solo si dista de la tolerancia
 *     del ultimo conservado. Es el criterio que usan los visores para reducir
 *     densidad y, sobre todo, evita el caso cuadratico de Douglas-Peucker en
 *     anillos densos (que es lo habitual en datos reales).
 *  2. Douglas-Peucker sobre los vertices que sobrevivieron al filtro.
 *
 * La salida se anade a `out` (x,y intercalados).
 */
void simplifyRing(const double *source, int count, double tolerance, SimplifyScratch &scratch,
                  std::vector<double> &out)
{
  if (count <= 4 || tolerance <= 0.0)
  {
    out.insert(out.end(), source, source + 2 * count);
    return;
  }

  const double toleranceSquared = tolerance * tolerance;

  // --- Fase 1: filtro radial ------------------------------------------------
  scratch.keep.assign(static_cast<size_t>(count), 0);
  scratch.keep[0] = 1;
  scratch.keep[static_cast<size_t>(count) - 1] = 1;

  int lastKept = 0;
  for (int i = 1; i < count - 1; ++i)
  {
    const double dx = source[2 * i] - source[2 * lastKept];
    const double dy = source[2 * i + 1] - source[2 * lastKept + 1];
    if (dx * dx + dy * dy >= toleranceSquared)
    {
      scratch.keep[static_cast<size_t>(i)] = 1;
      lastKept = i;
    }
  }

  scratch.compact.clear();
  for (int i = 0; i < count; ++i)
  {
    if (scratch.keep[static_cast<size_t>(i)])
      scratch.compact.push_back(static_cast<uint32_t>(i));
  }

  // --- Fase 2: Douglas-Peucker sobre los vertices conservados ---------------
  const int compactCount = static_cast<int>(scratch.compact.size());
  scratch.keepCompact.assign(static_cast<size_t>(compactCount), 0);
  scratch.keepCompact[0] = 1;
  scratch.keepCompact[static_cast<size_t>(compactCount) - 1] = 1;

  scratch.stack.clear();
  scratch.stack.emplace_back(0, compactCount - 1);

  while (!scratch.stack.empty())
  {
    const std::pair<int, int> range = scratch.stack.back();
    scratch.stack.pop_back();
    const int first = range.first;
    const int last = range.second;

    if (last - first < 2)
      continue;

    const double ax = source[2 * scratch.compact[static_cast<size_t>(first)]];
    const double ay = source[2 * scratch.compact[static_cast<size_t>(first)] + 1];
    const double bx = source[2 * scratch.compact[static_cast<size_t>(last)]];
    const double by = source[2 * scratch.compact[static_cast<size_t>(last)] + 1];

    double worstDistance = -1.0;
    int worstIndex = -1;
    for (int i = first + 1; i < last; ++i)
    {
      const uint32_t sourceIndex = scratch.compact[static_cast<size_t>(i)];
      const double distance =
        distanceToSegmentSquared(source[2 * sourceIndex], source[2 * sourceIndex + 1], ax, ay, bx, by);
      if (distance > worstDistance)
      {
        worstDistance = distance;
        worstIndex = i;
      }
    }

    if (worstDistance > toleranceSquared && worstIndex > first)
    {
      scratch.keepCompact[static_cast<size_t>(worstIndex)] = 1;
      scratch.stack.emplace_back(first, worstIndex);
      scratch.stack.emplace_back(worstIndex, last);
    }
  }

  const size_t firstOutput = out.size();
  for (int i = 0; i < compactCount; ++i)
  {
    if (!scratch.keepCompact[static_cast<size_t>(i)])
      continue;
    const uint32_t sourceIndex = scratch.compact[static_cast<size_t>(i)];
    out.push_back(source[2 * sourceIndex]);
    out.push_back(source[2 * sourceIndex + 1]);
  }

  // Un anillo simplificado en exceso pierde la forma (o desaparece): en ese caso
  // se conserva el original, que es lo que hace cualquier visor a escala de
  // pixel. El limite solo se aplica si el anillo era suficientemente grande.
  const size_t outputVertices = (out.size() - firstOutput) / 2;
  if (count >= static_cast<int>(kMinimumRingVertices) && outputVertices < kMinimumRingVertices)
  {
    out.resize(firstOutput);
    out.insert(out.end(), source, source + 2 * count);
  }
}

/**
 * Acumulador de la lectura de la capa: anade anillos y elementos a buffers
 * planos y calcula las envolventes sobre la marcha.
 */
class StoreBuilder
{
public:
  StoreBuilder() = default;

  std::vector<GeometryStore::Feature> features;
  std::vector<uint8_t> ringKinds;
  std::vector<double> coords;
  std::vector<uint32_t> ringFirst;
  std::vector<uint32_t> ringCount;

  qint64 vertexCount = 0;

  void beginFeature()
  {
    mFeatureFirstRing = static_cast<uint32_t>(ringFirst.size());
    // La envolvente se reinicia en CADA elemento: si no, cada elemento hereda la
    // union de todos los anteriores y el R-tree acaba con nodos que cubren la
    // capa entera, con lo que no se descarta nada al consultar.
    mHasEnvelope = false;
  }

  void endFeature()
  {
    GeometryStore::Feature feature;
    feature.firstRing = mFeatureFirstRing;
    feature.ringCount = static_cast<uint32_t>(ringFirst.size()) - mFeatureFirstRing;
    if (!mHasEnvelope)
      return; // geometria vacia: no se indexa

    feature.minX = mMinX;
    feature.minY = mMinY;
    feature.maxX = mMaxX;
    feature.maxY = mMaxY;
    features.push_back(feature);
    mHasEnvelope = false;
  }

  /** Anade un anillo ya en buffer plano; `reverse` invierte el sentido. */
  void addRing(const std::vector<double> &xy, uint8_t kind, bool reverse)
  {
    const uint32_t count = static_cast<uint32_t>(xy.size() / 2);
    if (count == 0)
      return;

    const uint32_t first = static_cast<uint32_t>(coords.size() / 2);

    // OJO: no se reserva el tamano exacto en cada anillo. `reserve(size + n)`
    // obliga a reasignar y copiar TODO lo acumulado en cada llamada, lo que
    // convierte la lectura en O(n^2): con 12.000 anillos suponia ~30 s de
    // memcpy. push_back crece de forma geometrica (amortizado O(1)).
    for (uint32_t i = 0; i < count; ++i)
    {
      const uint32_t index = reverse ? (count - 1 - i) : i;
      append(xy[2 * index], xy[2 * index + 1]);
    }

    ringFirst.push_back(first);
    ringCount.push_back(count);
    ringKinds.push_back(kind);
    vertexCount += count;
  }

  void append(double x, double y)
  {
    coords.push_back(x);
    coords.push_back(y);

    if (!mHasEnvelope)
    {
      mMinX = mMaxX = x;
      mMinY = mMaxY = y;
      mHasEnvelope = true;
    }
    else
    {
      mMinX = std::min(mMinX, x);
      mMinY = std::min(mMinY, y);
      mMaxX = std::max(mMaxX, x);
      mMaxY = std::max(mMaxY, y);
    }
  }

private:
  uint32_t mFeatureFirstRing = 0;
  bool mHasEnvelope = false;
  double mMinX = 0.0;
  double mMinY = 0.0;
  double mMaxX = 0.0;
  double mMaxY = 0.0;
};

/** Cierra el anillo si el primer y el ultimo vertice no coinciden. */
void xyEnsureClosed(std::vector<double> &xy)
{
  if (xy.size() < 6)
    return;

  if (xy[0] != xy[xy.size() - 2] || xy[1] != xy[xy.size() - 1])
  {
    xy.push_back(xy[0]);
    xy.push_back(xy[1]);
  }
}

/**
 * Vuelca los puntos de una curva a un buffer plano. Las curvas compuestas se
 * recorren por tramos; `skipFirst` evita repetir el vertice de union.
 */
void readCurve(const OGRCurve *curve, std::vector<double> &out, bool skipFirst)
{
  if (!curve || curve->IsEmpty())
    return;

  // getX/getY solo existen en OGRSimpleCurve (lineas, anillos, arcos).
  if (const OGRSimpleCurve *simple = curve->toSimpleCurve())
  {
    const int count = simple->getNumPoints();
    out.reserve(out.size() + static_cast<size_t>(count) * 2);
    for (int i = skipFirst ? 1 : 0; i < count; ++i)
    {
      out.push_back(simple->getX(i));
      out.push_back(simple->getY(i));
    }
    return;
  }

  if (const OGRCompoundCurve *compound = curve->toCompoundCurve())
  {
    const int count = compound->getNumCurves();
    for (int i = 0; i < count; ++i)
      readCurve(compound->getCurve(i), out, i > 0);
  }
}

void addPointGeometry(StoreBuilder &builder, const OGRPoint *point)
{
  if (point->IsEmpty())
    return;

  std::vector<double> xy {point->getX(), point->getY()};
  builder.addRing(xy, kKindPoint, false);
}

void addLineGeometry(StoreBuilder &builder, const OGRCurve *curve, std::vector<double> &scratch)
{
  if (curve->IsEmpty())
    return;

  scratch.clear();
  readCurve(curve, scratch, false);
  if (scratch.size() >= 4)
    builder.addRing(scratch, kKindLine, false);
}

/** Anade los anillos de un poligono: exterior en un sentido, huecos en el otro. */
void addPolygonGeometry(StoreBuilder &builder, const OGRCurvePolygon *polygon, std::vector<double> &scratch)
{
  if (const OGRCurve *exterior = polygon->getExteriorRingCurve())
  {
    scratch.clear();
    readCurve(exterior, scratch, false);
    xyEnsureClosed(scratch);
    if (scratch.size() >= 8)
    {
      // Sentido antihorario para el anillo exterior: junto con Qt::WindingFill
      // los huecos restan y los solapes entre elementos suman (se rellenan).
      const bool reverse = signedArea(scratch.data(), static_cast<int>(scratch.size() / 2)) < 0.0;
      builder.addRing(scratch, kKindPolygon, reverse);
    }
  }

  const int interiorCount = polygon->getNumInteriorRings();
  for (int i = 0; i < interiorCount; ++i)
  {
    const OGRCurve *interior = polygon->getInteriorRingCurve(i);
    if (!interior || interior->IsEmpty())
      continue;

    scratch.clear();
    readCurve(interior, scratch, false);
    xyEnsureClosed(scratch);
    if (scratch.size() < 8)
      continue;

    // Huecos en sentido contrario al exterior.
    const bool reverse = signedArea(scratch.data(), static_cast<int>(scratch.size() / 2)) > 0.0;
    builder.addRing(scratch, kKindPolygon, reverse);
  }
}

/** Recorre una geometria (posiblemente una coleccion) y la vuelca al builder. */
void addGeometry(StoreBuilder &builder, const OGRGeometry *geometry, std::vector<double> &scratch)
{
  if (!geometry || geometry->IsEmpty())
    return;

  switch (wkbFlatten(geometry->getGeometryType()))
  {
    case wkbPoint:
      addPointGeometry(builder, geometry->toPoint());
      return;

    case wkbLineString:
    case wkbLinearRing:
    case wkbCircularString:
    case wkbCompoundCurve:
      addLineGeometry(builder, geometry->toCurve(), scratch);
      return;

    case wkbPolygon:
    case wkbTriangle:
    case wkbCurvePolygon:
      addPolygonGeometry(builder, geometry->toCurvePolygon(), scratch);
      return;

    default:
    {
      const OGRGeometryCollection *collection = geometry->toGeometryCollection();
      if (!collection)
        return; // tipo no soportado (superficies 3D, TIN, poliedros...)

      const int count = collection->getNumGeometries();
      for (int i = 0; i < count; ++i)
        addGeometry(builder, collection->getGeometryRef(i), scratch);
      return;
    }
  }
}

/** Aplana un tipo de geometria OGR al tipo simplificado de MiniGIS. */
uint8_t kindForOgrType(OGRwkbGeometryType type)
{
  switch (wkbFlatten(type))
  {
    case wkbPoint:
    case wkbMultiPoint:
      return kKindPoint;

    case wkbLineString:
    case wkbLinearRing:
    case wkbMultiLineString:
    case wkbCircularString:
    case wkbCompoundCurve:
    case wkbMultiCurve:
      return kKindLine;

    case wkbPolygon:
    case wkbTriangle:
    case wkbCurvePolygon:
    case wkbMultiPolygon:
    case wkbMultiSurface:
      return kKindPolygon;

    default:
      return kKindUnknown;
  }
}

} // namespace

std::shared_ptr<GeometryStore> GeometryStore::build(
  OGRLayer *layer, int destinationEpsg, const Options &options,
  const std::function<bool(int, const QString &)> &progress, QString *error)
{
  const auto fail = [error](const QString &message) -> std::shared_ptr<GeometryStore> {
    if (error)
      *error = message;
    return {};
  };

  if (!layer)
    return fail(QStringLiteral("No hay capa OGR que indexar."));

  const auto notify = [&progress](int percent, const QString &message) -> bool {
    return progress ? progress(percent, message) : true;
  };

  if (!notify(2, QStringLiteral("Preparando la lectura de la capa...")))
    return fail(QStringLiteral("Carga cancelada por el usuario."));

  // --- Reproyeccion a la salida: se prepara una sola vez --------------------
  OGRSpatialReference destination;
  const bool hasDestination =
    (destinationEpsg > 0) && (destination.importFromEPSG(destinationEpsg) == OGRERR_NONE);
  if (hasDestination)
    destination.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  std::unique_ptr<OGRSpatialReference> sourceClone;
  bool reprojected = false;
  if (const OGRSpatialReference *source = layer->GetSpatialRef())
  {
    sourceClone.reset(source->Clone());
    if (sourceClone)
      sourceClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  }

  std::unique_ptr<OGRCoordinateTransformation> transformation;
  if (hasDestination && sourceClone && !sourceClone->IsSame(&destination))
  {
    transformation.reset(OGRCreateCoordinateTransformation(sourceClone.get(), &destination));
    reprojected = (transformation != nullptr);
  }

  // --- Lectura de la capa --------------------------------------------------
  StoreBuilder builder;
  std::vector<double> scratch;

  QElapsedTimer phaseTimer;
  phaseTimer.start();

  const GIntBig declaredCount = layer->GetFeatureCount(1);
  const qint64 total = declaredCount > 0 ? static_cast<qint64>(declaredCount) : 0;

  layer->ResetReading();

  qint64 read = 0;
  int lastPercent = -1;
  while (OGRFeature *feature = layer->GetNextFeature())
  {
    OGRGeometry *geometry = feature->GetGeometryRef();
    if (geometry && kindForOgrType(geometry->getGeometryType()) != kKindUnknown)
    {
      builder.beginFeature();

      if (transformation)
      {
        // La transformacion modifica la geometria in situ: se trabaja sobre una
        // copia. Es una sola vez por elemento, no una vez por cuadro.
        std::unique_ptr<OGRGeometry> transformed(geometry->clone());
        if (transformed && transformed->transform(transformation.get()) == OGRERR_NONE)
          addGeometry(builder, transformed.get(), scratch);
      }
      else
      {
        addGeometry(builder, geometry, scratch);
      }

      builder.endFeature();
    }

    OGRFeature::DestroyFeature(feature);
    ++read;

    if (options.vertexBudget > 0 && builder.vertexCount > options.vertexBudget)
      return fail(QStringLiteral("La capa tiene mas de %1 vertices y no cabe en el almacen en memoria "
                                 "(se usara el renderizado por lectura directa).")
                    .arg(options.vertexBudget));

    // Progreso: 5 %..80 % cubre la lectura de la capa.
    if ((read & 0x3FF) == 0)
    {
      int percent = 5;
      if (total > 0)
        percent = 5 + static_cast<int>(75.0 * static_cast<double>(read) / static_cast<double>(total));
      else
        percent = 5 + std::min(75, static_cast<int>(75.0 * static_cast<double>(read) /
                                                    static_cast<double>(read + 4000)));
      percent = std::clamp(percent, 5, 80);

      if (percent != lastPercent)
      {
        lastPercent = percent;
        if (!notify(percent, QStringLiteral("Leyendo geometrias: %1 elementos...").arg(read)))
          return fail(QStringLiteral("Carga cancelada por el usuario."));
      }
    }
  }

  layer->ResetReading();

  if (builder.features.empty())
    return fail(QStringLiteral("La capa no contiene geometrias que se puedan indexar."));

  if (!notify(82, QStringLiteral("Construyendo el indice espacial (%1 elementos)...")
                    .arg(builder.features.size())))
    return fail(QStringLiteral("Carga cancelada por el usuario."));

  auto store = std::shared_ptr<GeometryStore>(new GeometryStore);
  store->mFeatures = std::move(builder.features);
  store->mRingKinds = std::move(builder.ringKinds);
  store->mDestinationEpsg = destinationEpsg;
  store->mReprojected = reprojected;
  store->mReadMs = phaseTimer.elapsed();

  // Nivel 0: datos tal cual se leyeron (sin simplificar).
  auto raw = std::make_shared<Level>();
  raw->index = 0;
  raw->tolerance = 0.0;
  raw->coords = std::move(builder.coords);
  raw->ringFirst = std::move(builder.ringFirst);
  raw->ringCount = std::move(builder.ringCount);
  raw->vertices = builder.vertexCount;

  // Extension completa: fija la tolerancia base de la escalera de niveles.
  double minX = raw->coords[0];
  double minY = raw->coords[1];
  double maxX = minX;
  double maxY = minY;
  for (size_t i = 0; i + 1 < raw->coords.size(); i += 2)
  {
    minX = std::min(minX, raw->coords[i]);
    maxX = std::max(maxX, raw->coords[i]);
    minY = std::min(minY, raw->coords[i + 1]);
    maxY = std::max(maxY, raw->coords[i + 1]);
  }

  store->mFullSpan = std::max(std::max(maxX - minX, maxY - minY), 1e-12);
  // Tolerancia base = extension/2^16. Por debajo de ella (extensiones menores que
  // 1/32 de la capa) se dibujan los datos crudos: ahi quedan pocos elementos en
  // pantalla, mientras que construir un nivel casi sin simplificar costaria una
  // pasada completa por toda la capa (cientos de milisegundos).
  store->mBaseTolerance = store->mFullSpan / 65536.0;
  store->mMaximumLevel = 24;

  store->mLevels.emplace(0, raw);

  phaseTimer.restart();
  store->buildIndex();
  store->mIndexMs = phaseTimer.elapsed();
  phaseTimer.restart();

  if (!notify(88, QStringLiteral("Preparando niveles de detalle...")))
    return fail(QStringLiteral("Carga cancelada por el usuario."));

  // Niveles precalculados: se cubre la escalera de acercamientos habituales
  // (extension completa, x2, x4...). El resto se calcula bajo demanda.
  qint64 levelBytes = 0;
  for (int step = 0; step < std::max(0, options.precomputedLevels); ++step)
  {
    const double viewSpan = store->mFullSpan / std::pow(2.0, step);
    const double unitsPerPixel = viewSpan / 1024.0;
    const int index = store->levelForTolerance(0.5 * unitsPerPixel);
    if (index <= 0)
      break;

    if (store->mLevels.find(index) != store->mLevels.end())
      continue;

    auto level = store->buildLevel(index, *raw);
    if (!level)
      break;

    const qint64 bytes = level->vertices * (2 * static_cast<qint64>(sizeof(double)) + 2 * sizeof(uint32_t));
    if (levelBytes + bytes > options.precomputeBytes)
      break;

    levelBytes += bytes;
    store->mLevels.emplace(index, std::move(level));

    if (!notify(88 + 10 * (step + 1) / std::max(1, options.precomputedLevels),
                QStringLiteral("Nivel de detalle %1 listo.").arg(index)))
      return fail(QStringLiteral("Carga cancelada por el usuario."));
  }

  (void)levelBytes;
  store->mLevelsMs = phaseTimer.elapsed();

  if (!notify(100, QStringLiteral("Capa preparada.")))
    return fail(QStringLiteral("Carga cancelada por el usuario."));

  return store;
}

std::shared_ptr<GeometryStore> GeometryStore::buildFromLayer(
  const VectorLayer &layer, int destinationEpsg, const Options &options,
  const std::function<bool(int, const QString &)> &progress, QString *error)
{
  OGRLayerH handle = layer.ogrLayerHandle();
  if (!handle)
  {
    if (error)
      *error = QStringLiteral("La capa no esta abierta en GDAL.");
    return {};
  }

  return build(reinterpret_cast<OGRLayer *>(handle), destinationEpsg, options, progress, error);
}

/** Construye el R-tree empaquetado (Sort-Tile-Recursive) sobre las envolventes. */
void GeometryStore::buildIndex()
{
  const uint32_t featureCount = static_cast<uint32_t>(mFeatures.size());
  if (featureCount == 0)
    return;

  std::vector<uint32_t> order(featureCount);
  std::iota(order.begin(), order.end(), 0u);

  const auto centerX = [this](uint32_t index) {
    return mFeatures[index].minX + mFeatures[index].maxX;
  };
  const auto centerY = [this](uint32_t index) {
    return mFeatures[index].minY + mFeatures[index].maxY;
  };

  std::sort(order.begin(), order.end(), [&centerX](uint32_t a, uint32_t b) { return centerX(a) < centerX(b); });

  const uint32_t leafCount = (featureCount + kNodeCapacity - 1) / kNodeCapacity;
  const uint32_t sliceCount = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(leafCount)))));
  const uint32_t perSlice = (featureCount + sliceCount - 1) / sliceCount;

  mNodes.clear();
  mNodes.reserve(featureCount / kNodeCapacity * 2 + 8);
  mLeafIds.clear();
  mLeafIds.reserve(featureCount);
  mChildIds.clear();
  std::vector<uint32_t> currentLevel;

  for (uint32_t slice = 0; slice < sliceCount; ++slice)
  {
    const uint32_t begin = slice * perSlice;
    if (begin >= featureCount)
      break;
    const uint32_t end = std::min(featureCount, begin + perSlice);

    std::sort(order.begin() + begin, order.begin() + end,
              [&centerY](uint32_t a, uint32_t b) { return centerY(a) < centerY(b); });

    for (uint32_t i = begin; i < end; i += kNodeCapacity)
    {
      const uint32_t count = std::min(kNodeCapacity, end - i);

      RNode node;
      node.leaf = true;
      // Los elementos de la hoja se guardan agrupados al final de mLeafIds; el
      // desplazamiento se toma antes de anadir, no del tamano ya reservado.
      node.first = static_cast<uint32_t>(mLeafIds.size());
      node.count = count;
      node.minX = node.maxX = mFeatures[order[i]].minX;
      node.minY = node.maxY = mFeatures[order[i]].minY;

      for (uint32_t k = 0; k < count; ++k)
      {
        const Feature &feature = mFeatures[order[i + k]];
        node.minX = std::min(node.minX, feature.minX);
        node.minY = std::min(node.minY, feature.minY);
        node.maxX = std::max(node.maxX, feature.maxX);
        node.maxY = std::max(node.maxY, feature.maxY);
        mLeafIds.push_back(order[i + k]);
      }

      currentLevel.push_back(static_cast<uint32_t>(mNodes.size()));
      mNodes.push_back(node);
    }
  }

  // Niveles superiores: los nodos se empaquetan con el mismo criterio. Los hijos
  // se guardan en mChildIds porque, tras ordenar por centroide, sus indices ya no
  // son consecutivos y no se pueden deducir del primero.
  while (currentLevel.size() > 1)
  {
    const auto nodeCenterX = [this](uint32_t index) {
      return mNodes[index].minX + mNodes[index].maxX;
    };
    const auto nodeCenterY = [this](uint32_t index) {
      return mNodes[index].minY + mNodes[index].maxY;
    };

    std::sort(currentLevel.begin(), currentLevel.end(),
              [&nodeCenterX](uint32_t a, uint32_t b) { return nodeCenterX(a) < nodeCenterX(b); });

    const uint32_t parentCount = (static_cast<uint32_t>(currentLevel.size()) + kNodeCapacity - 1) / kNodeCapacity;
    const uint32_t parentSlices = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(parentCount)))));
    const uint32_t perParentSlice = (static_cast<uint32_t>(currentLevel.size()) + parentSlices - 1) / parentSlices;

    std::vector<uint32_t> nextLevel;

    for (uint32_t slice = 0; slice < parentSlices; ++slice)
    {
      const uint32_t begin = slice * perParentSlice;
      if (begin >= currentLevel.size())
        break;
      const uint32_t end = std::min(static_cast<uint32_t>(currentLevel.size()), begin + perParentSlice);

      std::sort(currentLevel.begin() + begin, currentLevel.begin() + end,
                [&nodeCenterY](uint32_t a, uint32_t b) { return nodeCenterY(a) < nodeCenterY(b); });

      for (uint32_t i = begin; i < end; i += kNodeCapacity)
      {
        const uint32_t count = std::min(kNodeCapacity, end - i);

        RNode node;
        node.leaf = false;
        node.first = static_cast<uint32_t>(mChildIds.size());
        node.count = count;
        node.minX = node.maxX = mNodes[currentLevel[i]].minX;
        node.minY = node.maxY = mNodes[currentLevel[i]].minY;

        for (uint32_t k = 0; k < count; ++k)
        {
          const uint32_t childIndex = currentLevel[i + k];
          const RNode &child = mNodes[childIndex];
          node.minX = std::min(node.minX, child.minX);
          node.minY = std::min(node.minY, child.minY);
          node.maxX = std::max(node.maxX, child.maxX);
          node.maxY = std::max(node.maxY, child.maxY);
          mChildIds.push_back(childIndex);
        }

        nextLevel.push_back(static_cast<uint32_t>(mNodes.size()));
        mNodes.push_back(node);
      }
    }

    currentLevel.swap(nextLevel);
  }

  mRoot = currentLevel.empty() ? 0 : currentLevel.front();
}

std::shared_ptr<GeometryStore::Level> GeometryStore::buildLevel(int index, const Level &raw) const
{
  auto level = std::make_shared<Level>();
  level->index = index;
  level->tolerance = mBaseTolerance * std::pow(2.0, index - 1);

  const size_t ringTotal = raw.ringFirst.size();
  level->ringFirst.resize(ringTotal);
  level->ringCount.resize(ringTotal);

  // Estimacion inicial: la simplificacion suele dejar entre el 10 % y el 40 %.
  level->coords.reserve(static_cast<size_t>(raw.vertices) / 2 * 2);

  std::vector<uint8_t> scratch;
  std::vector<double> ringBuffer;
  ringBuffer.reserve(4096);

  SimplifyScratch simplifyScratch;

  for (size_t ring = 0; ring < ringTotal; ++ring)
  {
    const uint32_t first = raw.ringFirst[ring];
    const uint32_t count = raw.ringCount[ring];
    const double *xy = raw.coords.data() + 2 * static_cast<size_t>(first);

    level->ringFirst[ring] = static_cast<uint32_t>(level->coords.size() / 2);

    const uint8_t kind = mRingKinds[ring];
    const bool simple = (kind == kKindPoint) || (count <= 2);

    if (simple)
    {
      level->coords.insert(level->coords.end(), xy, xy + 2 * static_cast<size_t>(count));
      level->ringCount[ring] = count;
    }
    else
    {
      ringBuffer.clear();
      simplifyRing(xy, static_cast<int>(count), level->tolerance, simplifyScratch, ringBuffer);
      level->coords.insert(level->coords.end(), ringBuffer.begin(), ringBuffer.end());
      level->ringCount[ring] = static_cast<uint32_t>(ringBuffer.size() / 2);
    }

    level->vertices += level->ringCount[ring];
  }

  return level;
}

const GeometryStore::Level *GeometryStore::acquireLevel(int index) const
{
  std::lock_guard<std::mutex> lock(mMutex);

  const int wanted = std::clamp(index, 0, mMaximumLevel);
  const auto existing = mLevels.find(wanted);
  if (existing != mLevels.end())
    return existing->second.get();

  const auto raw = mLevels.find(0);
  if (raw == mLevels.end())
    return nullptr;

  std::shared_ptr<Level> level = buildLevel(wanted, *raw->second);

  // Presupuesto de cache: se descarta el nivel mas alejado del pedido.
  while (static_cast<int>(mLevels.size()) >= kMaximumCachedLevels)
  {
    auto victim = mLevels.end();
    int worstDistance = -1;
    for (auto it = mLevels.begin(); it != mLevels.end(); ++it)
    {
      if (it->first == 0 || it->first == wanted)
        continue;
      const int distance = std::abs(it->first - wanted);
      if (distance > worstDistance)
      {
        worstDistance = distance;
        victim = it;
      }
    }

    if (victim == mLevels.end())
      break;
    mLevels.erase(victim);
  }

  const auto inserted = mLevels.emplace(wanted, std::move(level));
  return inserted.first->second.get();
}

int GeometryStore::levelForTolerance(double tolerance) const
{
  if (!(tolerance > mBaseTolerance))
    return 0;

  const double ratio = tolerance / mBaseTolerance;
  const int index = 1 + static_cast<int>(std::floor(std::log2(ratio)));
  return std::clamp(index, 0, mMaximumLevel);
}

void GeometryStore::query(double minX, double minY, double maxX, double maxY, std::vector<uint32_t> &out) const
{
  if (mNodes.empty())
    return;

  mStack.clear();
  mStack.push_back(mRoot);

  while (!mStack.empty())
  {
    const uint32_t nodeIndex = mStack.back();
    mStack.pop_back();

    const RNode &node = mNodes[nodeIndex];
    if (node.maxX < minX || node.minX > maxX || node.maxY < minY || node.minY > maxY)
      continue;

    if (node.leaf)
    {
      for (uint32_t i = 0; i < node.count; ++i)
      {
        const uint32_t featureIndex = mLeafIds[node.first + i];
        const Feature &feature = mFeatures[featureIndex];
        if (feature.maxX < minX || feature.minX > maxX || feature.maxY < minY || feature.minY > maxY)
          continue;
        out.push_back(featureIndex);
      }
    }
    else
    {
      for (uint32_t i = 0; i < node.count; ++i)
        mStack.push_back(mChildIds[node.first + i]);
    }
  }
}

qint64 GeometryStore::cachedVertices() const
{
  std::lock_guard<std::mutex> lock(mMutex);

  qint64 total = 0;
  for (const auto &entry : mLevels)
    total += entry.second->vertices;
  return total;
}

GeometryStore::Stats GeometryStore::stats() const
{
  std::lock_guard<std::mutex> lock(mMutex);

  Stats result;
  result.features = static_cast<qint64>(mFeatures.size());
  result.rings = static_cast<qint64>(mRingKinds.size());
  result.cachedLevels = static_cast<int>(mLevels.size());
  result.destinationEpsg = mDestinationEpsg;
  result.reprojected = mReprojected;
  result.readMs = mReadMs;
  result.indexMs = mIndexMs;
  result.levelsMs = mLevelsMs;

  const auto raw = mLevels.find(0);
  if (raw != mLevels.end())
    result.vertices = raw->second->vertices;

  result.bytes = static_cast<qint64>(mFeatures.size() * sizeof(Feature))
                 + static_cast<qint64>(mLeafIds.size() * sizeof(uint32_t))
                 + static_cast<qint64>(mNodes.size() * sizeof(RNode));

  for (const auto &entry : mLevels)
  {
    const Level &level = *entry.second;
    result.bytes += static_cast<qint64>(level.coords.size() * sizeof(double))
                    + static_cast<qint64>(level.ringFirst.size() * sizeof(uint32_t))
                    + static_cast<qint64>(level.ringCount.size() * sizeof(uint32_t));
  }

  return result;
}

} // namespace minigis
