#pragma once

#include <QtGlobal>

#include <ogr_api.h>

namespace minigis
{

/**
 * Partes de geometria de un elemento, contadas por tipo.
 *
 * Se cuentan partes y no elementos porque un solo `Placemark` de KML puede traer
 * un punto, una linea y un poligono en la misma `MultiGeometry`.
 */
struct GeometryKindCounts
{
  qint64 points = 0;
  qint64 lines = 0;
  qint64 polygons = 0;
  qint64 unsupported = 0;

  qint64 drawable() const { return points + lines + polygons; }
};

/**
 * Recorre la geometria de un elemento y cuenta sus partes por tipo.
 *
 * Por que recursivo: una `MultiGeometry` de KML con contenido mixto llega como
 * coleccion, y mirar solo el tipo de arriba diria "desconocido" y perderia el
 * punto, la linea y el poligono que si sabemos dibujar. Lo usan el diagnostico
 * --check-kml y el resumen de tipo de la capa, para que los dos cuenten
 * exactamente igual.
 */
inline void countGeometryKinds(OGRGeometryH geometry, GeometryKindCounts &counts)
{
  if (!geometry)
    return;

  switch (wkbFlatten(OGR_G_GetGeometryType(geometry)))
  {
    case wkbPoint:
      ++counts.points;
      return;

    case wkbLineString:
    case wkbLinearRing:
    case wkbCircularString:
    case wkbCompoundCurve:
      ++counts.lines;
      return;

    case wkbPolygon:
    case wkbTriangle:
    case wkbCurvePolygon:
      ++counts.polygons;
      return;

    case wkbMultiPoint:
    case wkbMultiLineString:
    case wkbMultiPolygon:
    case wkbMultiCurve:
    case wkbMultiSurface:
    case wkbGeometryCollection:
    {
      const int parts = OGR_G_GetGeometryCount(geometry);
      for (int i = 0; i < parts; ++i)
        countGeometryKinds(OGR_G_GetGeometryRef(geometry, i), counts);
      return;
    }

    default:
      ++counts.unsupported;
      return;
  }
}

} // namespace minigis
