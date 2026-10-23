#include "minigisgeometrystore.h"

#include "minigisgeometryhit.h"
#include "minigisshapefileindex.h"
#include "minigisvectorlayer.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cmath>
#include <numeric>

#include <cpl_string.h>
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
 * Destino de los anillos extraidos de una geometria.
 *
 * Existe para no duplicar el recorrido de OGR (curvas, colecciones y
 * normalizacion del sentido de los anillos) entre la lectura completa de la capa
 * (modo memoria) y la lectura bajo demanda (modo envolventes).
 */
class RingSink
{
public:
  virtual ~RingSink() = default;
  virtual void addRing(const std::vector<double> &xy, uint8_t kind, bool reverse) = 0;
};

/**
 * Acumulador de la lectura de la capa: anade anillos y elementos a buffers
 * planos y calcula las envolventes sobre la marcha.
 */
class StoreBuilder : public RingSink
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
  void addRing(const std::vector<double> &xy, uint8_t kind, bool reverse) override
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

/**
 * Destino que guarda la geometria de un solo elemento (modo envolventes): sus
 * anillos, con el sentido ya normalizado, listos para simplificar y dibujar.
 */
class CacheEntryBuilder : public RingSink
{
public:
  GeometryStore::CachedGeometry geometry;

  void addRing(const std::vector<double> &xy, uint8_t kind, bool reverse) override
  {
    const uint32_t count = static_cast<uint32_t>(xy.size() / 2);
    if (count == 0)
      return;

    geometry.coords.reserve(geometry.coords.size() + xy.size());
    const uint32_t first = static_cast<uint32_t>(geometry.coords.size() / 2);

    for (uint32_t i = 0; i < count; ++i)
    {
      const uint32_t index = reverse ? (count - 1 - i) : i;
      geometry.coords.push_back(xy[2 * index]);
      geometry.coords.push_back(xy[2 * index + 1]);
    }

    geometry.ringFirst.push_back(first);
    geometry.ringCount.push_back(count);
    geometry.ringKind.push_back(kind);
  }
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

void addPointGeometry(RingSink &sink, const OGRPoint *point)
{
  if (point->IsEmpty())
    return;

  std::vector<double> xy {point->getX(), point->getY()};
  sink.addRing(xy, kKindPoint, false);
}

void addLineGeometry(RingSink &sink, const OGRCurve *curve, std::vector<double> &scratch)
{
  if (curve->IsEmpty())
    return;

  scratch.clear();
  readCurve(curve, scratch, false);
  if (scratch.size() >= 4)
    sink.addRing(scratch, kKindLine, false);
}

/** Anade los anillos de un poligono: exterior en un sentido, huecos en el otro. */
void addPolygonGeometry(RingSink &sink, const OGRCurvePolygon *polygon, std::vector<double> &scratch)
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
      sink.addRing(scratch, kKindPolygon, reverse);
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
    sink.addRing(scratch, kKindPolygon, reverse);
  }
}

/** Recorre una geometria (posiblemente una coleccion) y la vuelca al builder. */
void addGeometry(RingSink &sink, const OGRGeometry *geometry, std::vector<double> &scratch)
{
  if (!geometry || geometry->IsEmpty())
    return;

  switch (wkbFlatten(geometry->getGeometryType()))
  {
    case wkbPoint:
      addPointGeometry(sink, geometry->toPoint());
      return;

    case wkbLineString:
    case wkbLinearRing:
    case wkbCircularString:
    case wkbCompoundCurve:
      addLineGeometry(sink, geometry->toCurve(), scratch);
      return;

    case wkbPolygon:
    case wkbTriangle:
    case wkbCurvePolygon:
      addPolygonGeometry(sink, geometry->toCurvePolygon(), scratch);
      return;

    default:
    {
      const OGRGeometryCollection *collection = geometry->toGeometryCollection();
      if (!collection)
        return; // tipo no soportado (superficies 3D, TIN, poliedros...)

      const int count = collection->getNumGeometries();
      for (int i = 0; i < count; ++i)
        addGeometry(sink, collection->getGeometryRef(i), scratch);
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

/**
 * Ignora temporalmente todos los campos de una capa y los restaura al salir.
 *
 * Por que hace falta un guardian: en cuanto se ignoran los campos, toda lectura
 * de atributos devuelve nulos. Si la construccion del almacen sale por un camino
 * de error (presupuesto de vertices superado, cancelacion, fallo de GDAL) sin
 * restaurarlos, la capa se queda con los atributos vacios y la tabla de atributos
 * muestra "(nulo)" en todas las filas. Es exactamente el fallo que sufrio
 * nz-building-outlines: el intento en memoria se aborta a proposito en la
 * comprobacion temprana, y aquel `return` se saltaba la restauracion.
 *
 * El destructor se ejecuta en TODAS las salidas.
 */
class IgnoredFieldsGuard
{
public:
  IgnoredFieldsGuard(OGRLayer *layer, char **fields)
    : mLayer(layer)
    , mFields(fields)
  {
  }

  ~IgnoredFieldsGuard()
  {
    if (!mFields)
      return;

    // NULL = volver a tener en cuenta todos los campos (documentado en OGR).
    OGR_L_SetIgnoredFields(mLayer, nullptr);
    CSLDestroy(mFields);
  }

  IgnoredFieldsGuard(const IgnoredFieldsGuard &) = delete;
  IgnoredFieldsGuard &operator=(const IgnoredFieldsGuard &) = delete;

private:
  OGRLayer *mLayer = nullptr;
  char **mFields = nullptr;
};

} // namespace

std::shared_ptr<GeometryStore> GeometryStore::build(
  OGRLayer *layer, const OGRSpatialReference *sourceSrs, int destinationEpsg, const Options &options,
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
  if (sourceSrs)
  {
    sourceClone.reset(sourceSrs->Clone());
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

  // Para indexar la geometria no hacen falta los atributos, y leerlos cuesta muy
  // caro cuando el DBF es enorme: en nz-addresses (2,43 M de elementos, DBF de
  // 1,47 GB) suponia 51 de los 52 s de la construccion. OGR permite ignorar
  // todos los campos y el driver del shapefile evita entonces leer el registro
  // del DBF. El guardian los restaura en cualquier salida, tambien en los
  // abortos por presupuesto.
  char **ignoredFields = nullptr;
  if (OGRFeatureDefnH definition = OGR_L_GetLayerDefn(layer))
  {
    const int fieldCount = OGR_FD_GetFieldCount(definition);
    for (int i = 0; i < fieldCount; ++i)
    {
      if (OGRFieldDefnH field = OGR_FD_GetFieldDefn(definition, i))
      {
        const char *name = OGR_Fld_GetNameRef(field);
        if (name && *name)
          ignoredFields = CSLAddString(ignoredFields, name);
      }
    }

    if (ignoredFields)
      OGR_L_SetIgnoredFields(layer, const_cast<const char **>(ignoredFields));
  }

  IgnoredFieldsGuard ignoredFieldsGuard(layer, ignoredFields);

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
                                 "(se usara el indice de envolventes o la lectura directa).")
                    .arg(options.vertexBudget));

    // Estimacion temprana: si con los primeros cientos de elementos ya se ve que
    // el total no va a caber, se aborta ahora. Sin esto, una capa de 3,2 M de
    // edificios se leia entera (varios segundos) solo para descubrir al final que
    // no cabia, y luego habia que indexarla igualmente.
    if (options.vertexBudget > 0 && read == 512 && total > 512)
    {
      const double verticesPerFeature = static_cast<double>(builder.vertexCount) / static_cast<double>(read);
      const double projectedVertices = verticesPerFeature * static_cast<double>(total);
      if (projectedVertices > static_cast<double>(options.vertexBudget))
        return fail(QStringLiteral("La capa tiene unos %1 millones de vertices (estimados sobre los "
                                   "primeros %2 elementos) y no cabe en el almacen en memoria "
                                   "(se usara el indice de envolventes o la lectura directa).")
                      .arg(QString::number(projectedVertices / 1e6, 'f', 1))
                      .arg(read));
    }

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

  // Arrays planos por elemento: el bucle de dibujado recorre miles de elementos
  // por cuadro y asi evita saltar por una estructura de 48 bytes por elemento.
  store->mEntryFirstRing.resize(store->mFeatures.size());
  store->mEntryRingCount.resize(store->mFeatures.size());
  for (size_t i = 0; i < store->mFeatures.size(); ++i)
  {
    store->mEntryFirstRing[i] = store->mFeatures[i].firstRing;
    store->mEntryRingCount[i] = store->mFeatures[i].ringCount;
  }

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

    // Si simplificar no reduce nada (una capa de puntos: cada elemento tiene un
    // solo vertice), los niveles serian copias exactas de los datos crudos. Con
    // 2,4 M de puntos eso son ~40 MB duplicados por nivel: mejor no construirlos
    // y quedarse con el nivel 0.
    if (step == 1 && level->vertices * 20 > raw->vertices * 19)
    {
      store->mSimplificationUseful = false;
      store->mLevels.clear();
      store->mLevels.emplace(0, raw);
      break;
    }

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

  // El SRC sale del VectorLayer, no de la capa OGR: si la capa no lo declara, lo
  // hereda de otra capa del archivo (VectorLayer::spatialRef) y aqui hace falta
  // para reproyectar las geometrias.
  return build(reinterpret_cast<OGRLayer *>(handle), layer.spatialRef(), destinationEpsg, options,
               progress, error);
}

std::shared_ptr<GeometryStore> GeometryStore::buildEnvelopeIndex(
  const VectorLayer &layer, int destinationEpsg, const Options &options,
  const std::function<bool(int, const QString &)> &progress, QString *error)
{
  const auto fail = [error](const QString &message) -> std::shared_ptr<GeometryStore> {
    if (error)
      *error = message;
    return {};
  };

  const QString sourcePath = layer.sourcePath();
  if (!ShapefileIndex::supports(layer.driverName(), sourcePath))
    return fail(QStringLiteral("El indice de envolventes solo esta disponible para archivos ESRI "
                               "Shapefile en disco (driver «%1»).")
                  .arg(layer.driverName()));

  OGRLayerH handle = layer.ogrLayerHandle();
  if (!handle)
    return fail(QStringLiteral("La capa no esta abierta en GDAL."));

  // --- Indice de envolventes: un recorrido secuencial del .shp --------------
  if (progress && !progress(0, QStringLiteral("Recorriendo el archivo para indexar las envolventes...")))
    return fail(QStringLiteral("Carga cancelada por el usuario."));

  ShapefileIndex::Stats indexStats;
  std::vector<ShapefileIndex::Entry> envelopes;

  // El escaneo ocupa la franja 0..85 % del progreso de la construccion.
  const bool scanned = ShapefileIndex::scan(
    sourcePath, envelopes, indexStats,
    [&progress](int percent, const QString &message) {
      return progress ? progress(percent * 85 / 100, message) : true;
    },
    error);

  if (!scanned)
    return fail(error ? *error : QStringLiteral("No se pudo indexar el archivo."));

  if (envelopes.empty())
    return fail(QStringLiteral("El archivo «%1» no contiene ninguna geometria indexable.")
                  .arg(QFileInfo(sourcePath).fileName()));

  auto store = std::shared_ptr<GeometryStore>(new GeometryStore);
  store->mMode = Mode::Envelopes;
  store->mDestinationEpsg = destinationEpsg;
  store->mCacheBudget = std::max<qint64>(1024, options.cacheBudget);
  store->mReadMs = indexStats.scanMs;

  // La capa se presta: las lecturas bajo demanda usan este mismo handle.
  store->mBorrowedLayer = reinterpret_cast<OGRLayer *>(handle);

  // Envolventes y reproyeccion de la capa completa (barata: 4 puntos por elemento).
  OGRSpatialReference destination;
  const bool hasDestination =
    (destinationEpsg > 0) && (destination.importFromEPSG(destinationEpsg) == OGRERR_NONE);
  if (hasDestination)
    destination.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  std::unique_ptr<OGRSpatialReference> sourceClone;
  if (const OGRSpatialReference *source = layer.spatialRef())
  {
    sourceClone.reset(source->Clone());
    if (sourceClone)
      sourceClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  }

  std::unique_ptr<OGRCoordinateTransformation> envelopeTransform;
  const bool reprojected = hasDestination && sourceClone && !sourceClone->IsSame(&destination);
  if (reprojected)
  {
    envelopeTransform.reset(OGRCreateCoordinateTransformation(sourceClone.get(), &destination));
    if (envelopeTransform)
      store->mTransformation.reset(envelopeTransform->Clone());
  }
  store->mReprojected = (store->mTransformation != nullptr);

  store->mFeatures.reserve(envelopes.size());
  double fullMinX = 0.0;
  double fullMinY = 0.0;
  double fullMaxX = 0.0;
  double fullMaxY = 0.0;

  for (size_t i = 0; i < envelopes.size(); ++i)
  {
    if (envelopeTransform)
    {
      // Se reproyectan las cuatro esquinas de la envolvente; la envolvente de la
      // reproyeccion las cubre (puede ser algo mayor, lo cual es conservador).
      double xs[4] = {envelopes[i].minX, envelopes[i].maxX, envelopes[i].minX, envelopes[i].maxX};
      double ys[4] = {envelopes[i].minY, envelopes[i].minY, envelopes[i].maxY, envelopes[i].maxY};
      if (envelopeTransform->Transform(4, xs, ys))
      {
        envelopes[i].minX = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
        envelopes[i].maxX = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
        envelopes[i].minY = std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3]));
        envelopes[i].maxY = std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
      }
    }

    GeometryStore::Feature feature;
    feature.minX = envelopes[i].minX;
    feature.minY = envelopes[i].minY;
    feature.maxX = envelopes[i].maxX;
    feature.maxY = envelopes[i].maxY;
    feature.firstRing = 0;
    feature.ringCount = 0;
    store->mFeatures.push_back(feature);

    if (i == 0)
    {
      fullMinX = feature.minX;
      fullMinY = feature.minY;
      fullMaxX = feature.maxX;
      fullMaxY = feature.maxY;
    }
    else
    {
      fullMinX = std::min(fullMinX, feature.minX);
      fullMinY = std::min(fullMinY, feature.minY);
      fullMaxX = std::max(fullMaxX, feature.maxX);
      fullMaxY = std::max(fullMaxY, feature.maxY);
    }

    if (progress && (i & 0x1FFFF) == 0)
    {
      const int percent = 85 + static_cast<int>(10.0 * static_cast<double>(i) / static_cast<double>(envelopes.size()));
      if (!progress(percent, QStringLiteral("Reproyectando envolventes: %1 elementos...").arg(i)))
        return fail(QStringLiteral("Carga cancelada por el usuario."));
    }
  }

  store->mFullSpan = std::max(std::max(fullMaxX - fullMinX, fullMaxY - fullMinY), 1e-12);
  store->mBaseTolerance = store->mFullSpan / 65536.0;
  store->mMaximumLevel = 24;

  if (progress && !progress(95, QStringLiteral("Construyendo el indice espacial (%1 elementos)...")
                                  .arg(store->mFeatures.size())))
    return fail(QStringLiteral("Carga cancelada por el usuario."));

  QElapsedTimer indexTimer;
  indexTimer.start();
  store->buildIndex();
  store->mIndexMs = indexTimer.elapsed();

  if (progress && !progress(100, QStringLiteral("Indice de envolventes listo: %1 elementos.").arg(indexStats.records)))
    return fail(QStringLiteral("Carga cancelada por el usuario."));

  return store;
}

bool GeometryStore::fetchGeometry(uint32_t fid) const
{
  if (!mBorrowedLayer || fid >= mFeatures.size())
    return false;

  const auto existing = mCache.find(fid);
  if (existing != mCache.end())
    return true;

  OGRFeature *feature = mBorrowedLayer->GetFeature(static_cast<GIntBig>(fid));
  if (!feature)
    return false;

  OGRGeometry *geometry = feature->GetGeometryRef();
  if (geometry)
  {
    auto cached = std::make_shared<CachedGeometry>();
    CacheEntryBuilder builder;
    std::vector<double> scratch;

    if (mTransformation)
    {
      std::unique_ptr<OGRGeometry> transformed(geometry->clone());
      if (transformed && transformed->transform(mTransformation.get()) == OGRERR_NONE)
        addGeometry(builder, transformed.get(), scratch);
    }
    else
    {
      addGeometry(builder, geometry, scratch);
    }

    cached->coords = std::move(builder.geometry.coords);
    cached->ringFirst = std::move(builder.geometry.ringFirst);
    cached->ringCount = std::move(builder.geometry.ringCount);
    cached->ringKind = std::move(builder.geometry.ringKind);

    mCacheBytes += static_cast<qint64>(cached->coords.size() * sizeof(double) + cached->ringFirst.size() * 8 + 64);
    mCache.emplace(fid, std::move(cached));
    mCacheOrder.push_back(fid);
    ++mGeometriesRead;
  }

  OGRFeature::DestroyFeature(feature);
  return true;
}

void GeometryStore::evictCacheIfNeeded() const
{
  while (static_cast<qint64>(mCache.size()) > mCacheBudget && !mCacheOrder.empty())
  {
    const uint32_t victim = mCacheOrder.front();
    mCacheOrder.pop_front();

    const auto found = mCache.find(victim);
    if (found == mCache.end())
      continue;

    mCacheBytes -= static_cast<qint64>(found->second->coords.size() * sizeof(double)
                                       + found->second->ringFirst.size() * 8 + 64);
    mCache.erase(found);
  }
}

std::shared_ptr<const GeometryStore::Subset> GeometryStore::acquireSubset(
  const std::vector<uint32_t> &candidates, double tolerance, qint64 readBudget,
  double minimumGeometrySize, qint64 dotBudget) const
{
  std::lock_guard<std::mutex> lock(mMutex);

  auto subset = std::make_shared<Subset>();

  if (candidates.empty())
    return subset;

  // --- Vale la pena leer la geometria a esta escala? -----------------------
  // Se estima el tamano tipico de un elemento con una muestra de sus
  // envolventes. Si esta por debajo del minimo, se dibujan puntos: a esa escala
  // la geometria no aporta nada y leerla cuesta I/O aleatoria por todo el
  // archivo (medido: 40.000 lecturas dispersas en un .shp de 584 MB = 63 s).
  bool useDotsOnly = false;
  if (minimumGeometrySize > 0.0)
  {
    const size_t sampleCount = std::min<size_t>(candidates.size(), 64);
    double accumulated = 0.0;
    for (size_t i = 0; i < sampleCount; ++i)
    {
      const Feature &feature = mFeatures[candidates[i]];
      accumulated += std::max(feature.maxX - feature.minX, feature.maxY - feature.minY);
    }
    const double typicalSize = sampleCount > 0 ? accumulated / static_cast<double>(sampleCount) : 0.0;
    useDotsOnly = typicalSize < minimumGeometrySize;
  }

  if (useDotsOnly)
  {
    subset->geometryRead = false;
    subset->missingFeatures = static_cast<qint64>(candidates.size());
    subset->readFeatures = 0;

    // Se muestrean las envolventes para que los puntos queden repartidos sin
    // dibujar millones de ellos.
    const qint64 budget = dotBudget > 0 ? dotBudget : static_cast<qint64>(candidates.size());
    const size_t stride =
      static_cast<size_t>(std::max<qint64>(1, (static_cast<qint64>(candidates.size()) + budget - 1) / budget));

    subset->dotEntries.reserve(static_cast<size_t>(budget));
    for (size_t i = 0; i < candidates.size(); i += stride)
      subset->dotEntries.push_back(candidates[i]);

    return subset;
  }

  // --- Que falta por leer --------------------------------------------------
  mMissingScratch.clear();
  for (const uint32_t fid : candidates)
  {
    if (mCache.find(fid) == mCache.end())
      mMissingScratch.push_back(fid);
  }

  subset->missingFeatures = static_cast<qint64>(mMissingScratch.size());

  if (!mMissingScratch.empty())
  {
    // Presupuesto de lectura por cuadro. Si la vista tiene mas elementos de los
    // que se pueden leer, se toma uno de cada k: la consulta devuelve los
    // elementos en orden espacial, asi que la muestra queda repartida por toda
    // la vista en lugar de amontonarse en una esquina.
    const qint64 budget = readBudget > 0 ? readBudget : static_cast<qint64>(mMissingScratch.size());
    const size_t stride = static_cast<size_t>(
      std::max<qint64>(1, (static_cast<qint64>(mMissingScratch.size()) + budget - 1) / budget));

    for (size_t i = 0; i < mMissingScratch.size(); i += stride)
    {
      if (subset->readFeatures >= budget)
        break;

      if (fetchGeometry(mMissingScratch[i]))
        ++subset->readFeatures;
    }

    evictCacheIfNeeded();

    // Los que no se han leido (por presupuesto) se dibujan como punto: es mejor
    // mostrar donde hay edificios que dejar la zona vacia.
    const qint64 remaining = static_cast<qint64>(mMissingScratch.size()) - subset->readFeatures;
    if (remaining > 0 && dotBudget != 0)
    {
      const qint64 budgetDots = dotBudget > 0 ? dotBudget : remaining;
      const size_t strideDots =
        static_cast<size_t>(std::max<qint64>(1, (remaining + budgetDots - 1) / budgetDots));

      qint64 drawn = 0;
      for (size_t i = 0; i < mMissingScratch.size() && drawn < budgetDots; i += strideDots)
      {
        const uint32_t fid = mMissingScratch[i];
        if (mCache.find(fid) != mCache.end())
          continue;
        subset->dotEntries.push_back(fid);
        ++drawn;
      }
    }
  }

  // --- Subconjunto dibujable ----------------------------------------------
  auto level = std::make_shared<Level>();
  level->index = -1;
  level->tolerance = tolerance;

  subset->entryFirstRing.reserve(candidates.size());
  subset->entryRingCount.reserve(candidates.size());

  SimplifyScratch simplifyScratch;
  std::vector<double> ringBuffer;

  qint64 vertices = 0;
  for (const uint32_t fid : candidates)
  {
    const auto found = mCache.find(fid);
    if (found == mCache.end())
      continue;

    const CachedGeometry &geometry = *found->second;
    // OJO: aqui va el indice del primer ANILLO (no del primer vertice): el bucle
    // de dibujado recorre `entryFirstRing .. +entryRingCount` sobre los arrays de
    // anillos del nivel. Confundirlo con el indice de vertice hacia que se leyera
    // fuera de los arrays (fallo detectado al probar con la capa de edificios).
    subset->entryFirstRing.push_back(static_cast<uint32_t>(level->ringFirst.size()));
    subset->entryRingCount.push_back(static_cast<uint32_t>(geometry.ringFirst.size()));

    const size_t ringTotal = geometry.ringFirst.size();
    for (size_t ring = 0; ring < ringTotal; ++ring)
    {
      const uint32_t first = geometry.ringFirst[ring];
      const uint32_t count = geometry.ringCount[ring];
      const double *xy = geometry.coords.data() + 2 * static_cast<size_t>(first);

      level->ringFirst.push_back(static_cast<uint32_t>(level->coords.size() / 2));

      const uint8_t kind = geometry.ringKind[ring];
      if (kind == kKindPoint || count <= 2)
      {
        level->coords.insert(level->coords.end(), xy, xy + 2 * static_cast<size_t>(count));
        level->ringCount.push_back(count);
      }
      else
      {
        ringBuffer.clear();
        simplifyRing(xy, static_cast<int>(count), tolerance, simplifyScratch, ringBuffer);
        level->coords.insert(level->coords.end(), ringBuffer.begin(), ringBuffer.end());
        level->ringCount.push_back(static_cast<uint32_t>(ringBuffer.size() / 2));
      }

      level->vertices += level->ringCount.back();
      vertices += level->ringCount.back();
      subset->entryRingKind.push_back(kind);
    }
  }

  subset->level = std::move(level);
  return subset;
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
  if (!mSimplificationUseful)
    return 0;

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

bool GeometryStore::hitTest(uint32_t index, double x, double y, double tolerance,
                            bool &inside, double &distance) const
{
  inside = false;
  distance = 0.0;

  if (index >= mFeatures.size())
    return false;

  const Feature &feature = mFeatures[index];

  // Descarte por envolvente: la distancia a la geometria nunca es menor que la
  // distancia a su envolvente, asi que por encima de la tolerancia no hace falta
  // leer nada (en modo envolventes esto ahorra I/O).
  const double dx = std::max({feature.minX - x, 0.0, x - feature.maxX});
  const double dy = std::max({feature.minY - y, 0.0, y - feature.maxY});
  const double envelopeDistance = std::hypot(dx, dy);
  if (envelopeDistance > tolerance)
  {
    distance = envelopeDistance;
    return true;
  }

  const std::vector<double> *coords = nullptr;
  const std::vector<uint32_t> *ringFirst = nullptr;
  const std::vector<uint32_t> *ringCount = nullptr;
  size_t ringBegin = 0;
  size_t ringEnd = 0;
  GeometryKind kind = GeometryKind::Unknown;

  std::shared_ptr<CachedGeometry> cached;

  if (mMode == Mode::InMemory)
  {
    const Level *levelData = acquireLevel(0);
    if (!levelData || index >= mEntryFirstRing.size() || index >= mEntryRingCount.size())
      return false;

    ringBegin = mEntryFirstRing[index];
    ringEnd = ringBegin + mEntryRingCount[index];
    if (ringEnd > levelData->ringFirst.size() || ringEnd > levelData->ringCount.size()
        || ringBegin >= mRingKinds.size())
      return false;

    kind = static_cast<GeometryKind>(mRingKinds[ringBegin]);
    coords = &levelData->coords;
    ringFirst = &levelData->ringFirst;
    ringCount = &levelData->ringCount;
  }
  else
  {
    {
      std::lock_guard<std::mutex> lock(mMutex);
      if (!fetchGeometry(index))
        return false;

      const auto found = mCache.find(index);
      if (found == mCache.end())
        return false;

      cached = found->second;
      // El shared_ptr mantiene viva la geometria aunque la cache la desaloje.
      evictCacheIfNeeded();
    }

    if (cached->ringFirst.empty() || cached->ringKind.empty())
      return false;

    ringBegin = 0;
    ringEnd = cached->ringFirst.size();
    kind = static_cast<GeometryKind>(cached->ringKind.front());
    coords = &cached->coords;
    ringFirst = &cached->ringFirst;
    ringCount = &cached->ringCount;
  }

  const PointGeometryTest test =
    testPointAgainstGeometry(*coords, *ringFirst, *ringCount, ringBegin, ringEnd, kind, x, y);

  // Un elemento puntual no tiene interior: si ha pasado el filtro de la
  // envolvente es que se ha pulsado su simbolo.
  inside = kind == GeometryKind::Point ? true : test.inside;
  distance = inside ? 0.0 : test.distance;
  return true;
}

bool GeometryStore::featureRings(uint32_t index, std::vector<double> &coords,
                                 std::vector<uint32_t> &ringFirst,
                                 std::vector<uint32_t> &ringCount, GeometryKind &kind) const
{
  coords.clear();
  ringFirst.clear();
  ringCount.clear();
  kind = GeometryKind::Unknown;

  if (index >= mFeatures.size())
    return false;

  const auto appendRing = [&](const double *xy, uint32_t count) {
    if (count == 0)
      return;
    ringFirst.push_back(static_cast<uint32_t>(coords.size() / 2));
    ringCount.push_back(count);
    coords.insert(coords.end(), xy, xy + 2 * static_cast<size_t>(count));
  };

  if (mMode == Mode::InMemory)
  {
    const Level *level = acquireLevel(0);
    if (!level || index >= mEntryFirstRing.size() || index >= mEntryRingCount.size())
      return false;

    const uint32_t first = mEntryFirstRing[index];
    const uint32_t total = mEntryRingCount[index];
    if (first + total > level->ringFirst.size() || first + total > level->ringCount.size()
        || first >= mRingKinds.size())
      return false;

    kind = static_cast<GeometryKind>(mRingKinds[first]);
    for (uint32_t ring = first; ring < first + total; ++ring)
    {
      const uint32_t count = level->ringCount[ring];
      const size_t base = static_cast<size_t>(level->ringFirst[ring]) * 2;
      if (base + static_cast<size_t>(count) * 2 > level->coords.size())
        continue;
      appendRing(level->coords.data() + base, count);
    }
    return !ringFirst.empty();
  }

  std::shared_ptr<CachedGeometry> cached;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!fetchGeometry(index))
      return false;

    const auto found = mCache.find(index);
    if (found == mCache.end())
      return false;

    cached = found->second;
    evictCacheIfNeeded();
  }

  if (!cached || cached->ringFirst.empty() || cached->ringKind.empty())
    return false;

  kind = static_cast<GeometryKind>(cached->ringKind.front());
  for (size_t ring = 0; ring < cached->ringFirst.size(); ++ring)
  {
    const uint32_t count = cached->ringCount[ring];
    const size_t base = static_cast<size_t>(cached->ringFirst[ring]) * 2;
    if (base + static_cast<size_t>(count) * 2 > cached->coords.size())
      continue;
    appendRing(cached->coords.data() + base, count);
  }

  return !ringFirst.empty();
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
  result.mode = mMode;
  result.cachedGeometries = static_cast<qint64>(mCache.size());
  result.geometriesRead = mGeometriesRead;

  const auto raw = mLevels.find(0);
  if (raw != mLevels.end())
    result.vertices = raw->second->vertices;

  result.bytes = static_cast<qint64>(mFeatures.size() * sizeof(Feature))
                 + static_cast<qint64>(mLeafIds.size() * sizeof(uint32_t))
                 + static_cast<qint64>(mChildIds.size() * sizeof(uint32_t))
                 + static_cast<qint64>(mNodes.size() * sizeof(RNode))
                 + static_cast<qint64>(mEntryFirstRing.size() * sizeof(uint32_t))
                 + static_cast<qint64>(mEntryRingCount.size() * sizeof(uint32_t));
  result.cachedGeometriesBytes = mCacheBytes;

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
