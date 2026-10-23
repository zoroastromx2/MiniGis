#include "minigisidentify.h"

#include "minigisgeometryhit.h"
#include "minigisgeometrystore.h"
#include "minigisvectorlayer.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <ogr_api.h>
#include <ogr_core.h>
#include <ogr_spatialref.h>

namespace minigis
{
namespace
{

/** Un candidato antes de leer sus atributos. */
struct Candidate
{
  qint64 fid = -1;
  bool inside = false;
  double distance = 0.0;
};

/** Distancia del punto a la envolvente del elemento (0 si cae dentro). */
double envelopeDistance(const GeometryStore::Feature &feature, double x, double y)
{
  const double dx = std::max({feature.minX - x, 0.0, x - feature.maxX});
  const double dy = std::max({feature.minY - y, 0.0, y - feature.maxY});
  return std::hypot(dx, dy);
}

/**
 * Filtro espacial de OGR que se retira al salir del ambito. La capa es prestada
 * y el mismo handle lo usa el renderizador: dejar el filtro puesto cambiaria lo
 * que se dibuja en el siguiente cuadro.
 */
class SpatialFilterGuard
{
public:
  explicit SpatialFilterGuard(OGRLayerH layer) : mLayer(layer) {}
  ~SpatialFilterGuard()
  {
    if (mLayer)
      OGR_L_SetSpatialFilter(mLayer, nullptr);
  }

  SpatialFilterGuard(const SpatialFilterGuard &) = delete;
  SpatialFilterGuard &operator=(const SpatialFilterGuard &) = delete;

private:
  OGRLayerH mLayer = nullptr;
};

std::unique_ptr<OGRSpatialReference> traditionalSrs(int epsg)
{
  auto srs = std::make_unique<OGRSpatialReference>();
  if (srs->importFromEPSG(epsg) != OGRERR_NONE)
    return nullptr;
  srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  return srs;
}

/** Copia los anillos de una geometria OGR plana a buffers contiguos. */
void appendGeometry(OGRGeometryH handle, std::vector<double> &coords, std::vector<uint32_t> &ringFirst,
                    std::vector<uint32_t> &ringCount)
{
  if (!handle)
    return;

  const OGRwkbGeometryType type = wkbFlatten(OGR_G_GetGeometryType(handle));

  if (type == wkbPoint)
  {
    if (OGR_G_IsEmpty(handle))
      return;

    ringFirst.push_back(static_cast<uint32_t>(coords.size() / 2));
    ringCount.push_back(1);
    coords.push_back(OGR_G_GetX(handle, 0));
    coords.push_back(OGR_G_GetY(handle, 0));
    return;
  }

  if (type == wkbLineString || type == wkbLinearRing)
  {
    const int count = OGR_G_GetPointCount(handle);
    if (count <= 0)
      return;

    ringFirst.push_back(static_cast<uint32_t>(coords.size() / 2));
    ringCount.push_back(static_cast<uint32_t>(count));
    for (int i = 0; i < count; ++i)
    {
      coords.push_back(OGR_G_GetX(handle, i));
      coords.push_back(OGR_G_GetY(handle, i));
    }
    return;
  }

  if (type == wkbPolygon)
  {
    const int rings = OGR_G_GetGeometryCount(handle);
    for (int ring = 0; ring < rings; ++ring)
      appendGeometry(OGR_G_GetGeometryRef(handle, ring), coords, ringFirst, ringCount);
    return;
  }

  // Multi* y colecciones: se recorren sus partes.
  const int parts = OGR_G_GetGeometryCount(handle);
  for (int part = 0; part < parts; ++part)
    appendGeometry(OGR_G_GetGeometryRef(handle, part), coords, ringFirst, ringCount);
}

void collectFromStore(const GeometryStore &store, const IdentifyRequest &request,
                      const IdentifyOptions &options, QElapsedTimer &timer, QVector<Candidate> &out,
                      IdentifyStats &stats, bool &stoppedEarly)
{
  const double x = request.mapPoint.x();
  const double y = request.mapPoint.y();
  const double tolerance = options.tolerance;

  std::vector<uint32_t> candidates;
  store.query(x - tolerance, y - tolerance, x + tolerance, y + tolerance, candidates);

  if (options.maximumCandidatesPerLayer > 0
      && candidates.size() > static_cast<size_t>(options.maximumCandidatesPerLayer))
  {
    // Los mas cercanos por envolvente: a igualdad de radio importa menos quien
    // este lejos del clic.
    std::sort(candidates.begin(), candidates.end(), [&](uint32_t a, uint32_t b) {
      return envelopeDistance(store.features()[a], x, y) < envelopeDistance(store.features()[b], x, y);
    });
    candidates.resize(static_cast<size_t>(options.maximumCandidatesPerLayer));
    stoppedEarly = true;
  }

  const qint64 readBefore = store.stats().geometriesRead;

  for (const uint32_t index : candidates)
  {
    if (options.timeBudgetMs > 0 && timer.elapsed() > options.timeBudgetMs)
    {
      stats.budgetExceeded = true;
      stoppedEarly = true;
      break;
    }

    bool inside = false;
    double distance = 0.0;
    if (!store.hitTest(index, x, y, tolerance, inside, distance))
      continue;

    ++stats.candidatesTested;
    if (inside || distance <= tolerance)
      out.push_back(Candidate {static_cast<qint64>(index), inside, distance});
  }

  stats.featuresRead += store.stats().geometriesRead - readBefore;
}

void collectFromOgr(const VectorLayer &layer, const IdentifyRequest &request,
                    const IdentifyOptions &options, QElapsedTimer &timer, QVector<Candidate> &out,
                    IdentifyStats &stats, bool &stoppedEarly)
{
  OGRLayerH handle = layer.ogrLayerHandle();
  if (!handle || request.destinationEpsg <= 0)
    return;

  const double x = request.mapPoint.x();
  const double y = request.mapPoint.y();
  const double tolerance = options.tolerance;
  const GeometryKind kind = layer.geometryKind();

  std::unique_ptr<OGRSpatialReference> destination = traditionalSrs(request.destinationEpsg);
  if (!destination)
    return;

  std::unique_ptr<OGRSpatialReference> source;
  if (const OGRSpatialReference *layerSrs = layer.spatialRef())
  {
    source.reset(layerSrs->Clone());
    if (source)
      source->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  }

  std::unique_ptr<OGRCoordinateTransformation> toDestination;
  std::unique_ptr<OGRCoordinateTransformation> toSource;
  const bool needsTransform = source && !source->IsSame(destination.get());
  if (needsTransform)
  {
    toDestination.reset(OGRCreateCoordinateTransformation(source.get(), destination.get()));
    toSource.reset(OGRCreateCoordinateTransformation(destination.get(), source.get()));
    if (!toDestination)
      return; // sin transformacion no se puede comparar con la tolerancia de destino
  }

  SpatialFilterGuard filterGuard(handle);

  if (!needsTransform)
  {
    OGR_L_SetSpatialFilterRect(handle, x - tolerance, y - tolerance, x + tolerance, y + tolerance);
  }
  else if (toSource)
  {
    // Caja de destino al SRC de la capa: cuatro esquinas y envolvente
    // conservadora. Es un filtro, no la prueba: la prueba exacta va despues.
    double xs[4] = {x - tolerance, x + tolerance, x - tolerance, x + tolerance};
    double ys[4] = {y - tolerance, y - tolerance, y + tolerance, y + tolerance};
    if (toSource->Transform(4, xs, ys))
    {
      OGR_L_SetSpatialFilterRect(handle, std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3])),
                                 std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3])),
                                 std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3])),
                                 std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3])));
    }
  }

  OGR_L_ResetReading(handle);

  int read = 0;
  while (OGRFeatureH feature = OGR_L_GetNextFeature(handle))
  {
    ++read;
    ++stats.featuresRead;

    if (options.maximumCandidatesPerLayer > 0 && read > options.maximumCandidatesPerLayer)
    {
      OGR_F_Destroy(feature);
      stoppedEarly = true;
      break;
    }

    if (options.timeBudgetMs > 0 && timer.elapsed() > options.timeBudgetMs)
    {
      OGR_F_Destroy(feature);
      stats.budgetExceeded = true;
      stoppedEarly = true;
      break;
    }

    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    const GIntBig fid = OGR_F_GetFID(feature);
    if (geometry && fid >= 0)
    {
      std::vector<double> coords;
      std::vector<uint32_t> ringFirst;
      std::vector<uint32_t> ringCount;

      if (needsTransform)
      {
        // La transformacion modifica la geometria: se trabaja sobre una copia.
        OGRGeometryH copy = OGR_G_Clone(geometry);
        if (copy)
        {
          if (OGR_G_Transform(copy, reinterpret_cast<OGRCoordinateTransformationH>(toDestination.get()))
              == OGRERR_NONE)
          {
            appendGeometry(copy, coords, ringFirst, ringCount);
          }
          OGR_G_DestroyGeometry(copy);
        }
      }
      else
      {
        appendGeometry(geometry, coords, ringFirst, ringCount);
      }

      if (!ringFirst.empty())
      {
        const PointGeometryTest test = testPointAgainstGeometry(coords, ringFirst, ringCount, 0,
                                                                ringFirst.size(), kind, x, y);
        const bool inside = kind == GeometryKind::Point ? true : test.inside;
        const double distance = inside ? 0.0 : test.distance;
        ++stats.candidatesTested;
        if (inside || distance <= tolerance)
          out.push_back(Candidate {static_cast<qint64>(fid), inside, distance});
      }
    }

    OGR_F_Destroy(feature);
  }

  OGR_L_ResetReading(handle);
}

} // namespace

IdentifyResult identifyAtPoint(const IdentifyRequest &request)
{
  IdentifyResult result;
  result.mapPoint = request.mapPoint;
  result.destinationEpsg = request.destinationEpsg;
  result.tolerance = request.options.tolerance;

  QElapsedTimer timer;
  timer.start();

  const IdentifyOptions &options = request.options;
  const double tolerance = std::max(0.0, options.tolerance);

  // Orden visible: prioritaria primero y despues de arriba abajo. El lienzo
  // guarda las capas de fondo a techo, asi que se recorre al reves.
  std::vector<std::shared_ptr<VectorLayer>> ordered;
  ordered.reserve(request.layers.size() + 1);
  if (request.priorityLayer)
    ordered.push_back(request.priorityLayer);

  for (auto it = request.layers.rbegin(); it != request.layers.rend(); ++it)
  {
    if (*it && (!request.priorityLayer || *it != request.priorityLayer))
      ordered.push_back(*it);
  }

  for (const std::shared_ptr<VectorLayer> &layer : ordered)
  {
    if (!layer)
      continue;

    ++result.stats.layersSearched;

    QVector<Candidate> candidates;
    bool stoppedEarly = false;
    const std::shared_ptr<GeometryStore> store = layer->geometryStore();
    const bool storeUsable =
      store && request.destinationEpsg > 0 && store->destinationEpsg() == request.destinationEpsg;

    if (storeUsable)
      collectFromStore(*store, request, options, timer, candidates, result.stats, stoppedEarly);
    else
      collectFromOgr(*layer, request, options, timer, candidates, result.stats, stoppedEarly);

    if (stoppedEarly)
      result.truncated = true;

    // Dentro primero; despues por distancia y, a igual distancia, por FID.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
      if (a.inside != b.inside)
        return a.inside;
      if (a.distance != b.distance)
        return a.distance < b.distance;
      return a.fid < b.fid;
    });

    int taken = 0;
    for (const Candidate &candidate : candidates)
    {
      if (options.maximumPerLayer > 0 && taken >= options.maximumPerLayer)
      {
        result.truncated = true;
        break;
      }
      if (options.maximumTotal > 0 && result.hits.size() >= options.maximumTotal)
      {
        result.truncated = true;
        break;
      }

      IdentifyHit hit;
      hit.layer = layer;
      hit.fid = candidate.fid;
      hit.inside = candidate.inside;
      hit.distance = candidate.distance;
      hit.values = layer->fieldValues(candidate.fid);
      result.hits.append(hit);
      ++taken;
    }
  }

  result.stats.elapsedMs = timer.elapsed();
  return result;
}

} // namespace minigis
