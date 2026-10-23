#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "minigisstyle.h"

namespace minigis
{

/** Resultado de probar un punto contra la geometria de un elemento. */
struct PointGeometryTest
{
  /** True si el punto cae dentro de un poligono (paridad even-odd). */
  bool inside = false;
  /**
   * Distancia del punto a la geometria, en las mismas unidades que las
   * coordenadas. Es 0 cuando el punto esta dentro de un poligono.
   */
  double distance = 0.0;
};

/**
 * Prueba un punto contra los anillos de una geometria plana.
 *
 * `coords` son coordenadas x,y intercaladas; `ringFirst` y `ringCount` indexan
 * en vertices (no en dobles) y describen los anillos de toda la geometria.
 * El rango [ringBegin, ringEnd) limita la prueba a los anillos de un elemento,
 * de modo que el almacen puede pasar sus buffers completos sin copiarlos.
 *
 * Los poligonos se resuelven por paridad (even-odd) sobre todos sus anillos,
 * asi los huecos quedan fuera sin necesitar el rol de cada anillo. Las lineas y
 * los puntos usan distancia. No se simplifica nada: el llamador debe pasar el
 * nivel 0 (datos tal cual).
 */
PointGeometryTest testPointAgainstGeometry(const std::vector<double> &coords,
                                           const std::vector<uint32_t> &ringFirst,
                                           const std::vector<uint32_t> &ringCount,
                                           size_t ringBegin, size_t ringEnd,
                                           GeometryKind kind, double x, double y);

} // namespace minigis
