#include "minigisgeometryhit.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace minigis
{
namespace
{

/** Distancia minima de un punto al segmento (x1,y1)-(x2,y2). */
double pointSegmentDistance(double x, double y, double x1, double y1, double x2, double y2)
{
  const double dx = x2 - x1;
  const double dy = y2 - y1;
  const double lengthSquared = dx * dx + dy * dy;
  if (lengthSquared <= 0.0)
    return std::hypot(x - x1, y - y1);

  double t = ((x - x1) * dx + (y - y1) * dy) / lengthSquared;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(x - (x1 + t * dx), y - (y1 + t * dy));
}

} // namespace

PointGeometryTest testPointAgainstGeometry(const std::vector<double> &coords,
                                           const std::vector<uint32_t> &ringFirst,
                                           const std::vector<uint32_t> &ringCount,
                                           size_t ringBegin, size_t ringEnd,
                                           GeometryKind kind, double x, double y)
{
  PointGeometryTest result;
  result.distance = std::numeric_limits<double>::max();

  if (ringBegin >= ringEnd || ringEnd > ringFirst.size() || ringEnd > ringCount.size())
    return result;

  const bool polygon = kind == GeometryKind::Polygon;
  const bool point = kind == GeometryKind::Point;

  int crossings = 0;

  for (size_t ring = ringBegin; ring < ringEnd; ++ring)
  {
    const uint32_t first = ringFirst[ring];
    const uint32_t count = ringCount[ring];
    if (count == 0)
      continue;

    const size_t base = static_cast<size_t>(first) * 2;
    const size_t end = base + static_cast<size_t>(count) * 2;
    if (end > coords.size())
      continue;

    if (point || count == 1)
    {
      result.distance = std::min(result.distance, std::hypot(coords[base] - x, coords[base + 1] - y));
      continue;
    }

    // Para una linea el ultimo vertice no cierra el anillo; para un poligono si.
    const uint32_t segments = polygon ? count : count - 1;
    for (uint32_t i = 0; i < segments; ++i)
    {
      const uint32_t j = (i + 1) % count;
      const double x1 = coords[base + 2 * static_cast<size_t>(i)];
      const double y1 = coords[base + 2 * static_cast<size_t>(i) + 1];
      const double x2 = coords[base + 2 * static_cast<size_t>(j)];
      const double y2 = coords[base + 2 * static_cast<size_t>(j) + 1];

      result.distance = std::min(result.distance, pointSegmentDistance(x, y, x1, y1, x2, y2));

      // Cruce del rayo horizontal hacia la derecha: basta la paridad.
      if (polygon && ((y1 > y) != (y2 > y)))
      {
        const double crossX = x1 + (y - y1) * (x2 - x1) / (y2 - y1);
        if (x < crossX)
          ++crossings;
      }
    }
  }

  result.inside = polygon && (crossings % 2) != 0;
  if (result.inside)
    result.distance = 0.0;

  return result;
}

} // namespace minigis
