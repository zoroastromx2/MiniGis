#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QSize>

#include <memory>
#include <vector>

#include "minigisrectangle.h"

class OGRSpatialReference;

namespace minigis
{

class VectorLayer;

/**
 * Conversion entre coordenadas de mapa y pixeles (equivalente minimo a
 * QgsMapToPixel). Mantiene la escala uniforme: no deforma las geometrias,
 * aunque el area de mapa no llene todo el lienzo.
 */
class MapToPixel
{
public:
  MapToPixel() = default;
  MapToPixel(const Rectangle &extent, const QSize &size);

  QPointF transform(double x, double y) const;
  QPointF transform(const QPointF &mapPoint) const { return transform(mapPoint.x(), mapPoint.y()); }
  QPointF toMap(const QPointF &pixel) const;

  /**
   * Variante en linea que evita construir un QPointF por vertice. Se usa en los
   * bucles que proyectan millones de vertices por cuadro (el compilador puede
   * convertir el resto en una simple multiplicacion y suma).
   */
  void projectInline(double x, double y, double &pixelX, double &pixelY) const
  {
    pixelX = mTopLeft.x() + (x - mExtent.xMin()) * mScale;
    pixelY = mTopLeft.y() + (mExtent.yMax() - y) * mScale;
  }

  /** Pixeles por unidad de mapa (escala de dibujo: a mayor valor, mas cerca). */
  double pixelsPerMapUnit() const { return mScale; }

  /** Unidades de mapa por pixel (a mayor valor, mas alejado). */
  double mapUnitsPerPixel() const { return mScale > 0.0 ? 1.0 / mScale : 1.0; }

  const Rectangle &extent() const { return mExtent; }
  bool isValid() const { return mValid; }

private:
  Rectangle mExtent;
  QPointF mTopLeft;
  double mScale = 1.0;
  bool mValid = false;
};

/**
 * Renderiza las capas vectoriales a una imagen (equivalente simplificado a
 * QgsMapRenderer). Si el SRC de una capa no coincide con el de destino, las
 * geometrias se reproyectan al vuelo con GDAL/PROJ.
 */
class MapRenderer
{
public:
  void setLayers(const std::vector<std::shared_ptr<VectorLayer>> &layers) { mLayers = layers; }
  const std::vector<std::shared_ptr<VectorLayer>> &layers() const { return mLayers; }

  /** SRC de destino (por defecto EPSG:4326, orden GIS tradicional). */
  void setDestinationEpsg(int epsg) { mDestinationEpsg = epsg; }
  int destinationEpsg() const { return mDestinationEpsg; }

  /**
   * Limite de elementos por capa; 0 = sin limite.
   *
   * Nota: con la lectura directa el limite se aplica DESPUES de descartar por
   * filtro espacial, de modo que cuenta elementos dibujables, no leidos.
   */
  void setMaximumFeaturesPerLayer(qint64 maximum) { mMaximumFeatures = maximum; }

  /**
   * Filtra en origen con el indice espacial del archivo (OGR_L_SetSpatialFilterRect).
   *
   * Sirve para capas demasiado grandes para caber en memoria (donde no se puede
   * construir el GeometryStore): el driver descarta los elementos que no cortan
   * la vista antes de leerlos, apoyandose en el R-tree del GeoPackage o en el
   * indice del shapefile. El coste es que el filtro limita el recorrido del
   * cursor, asi que el limite de elementos y el orden de lectura cambian
   * respecto al comportamiento historico; por eso es opcional y por defecto
   * esta desactivado (el camino clasico sin filtro sigue siendo reproducible
   * para comparar medidas).
   */
  void setUseSpatialFilter(bool enabled) { mUseSpatialFilter = enabled; }
  bool useSpatialFilter() const { return mUseSpatialFilter; }

  QImage render(const Rectangle &extent, const QSize &size, const QColor &background) const;

  /**
   * Dibuja una sola capa sobre un QPainter ya existente. Lo usa el lienzo para
   * intercalar, respetando el orden de apilado, capas con almacen indexado
   * (camino acelerado) y capas que aun se estan preparando (este camino).
   */
  void paintLayerInto(QPainter &painter, const MapToPixel &transform, const VectorLayer &layer) const;

  /** Extension combinada de las capas visibles. */
  Rectangle fullExtent() const;

private:
  void paintLayer(QPainter &painter, const MapToPixel &transform, const VectorLayer &layer) const;

  std::vector<std::shared_ptr<VectorLayer>> mLayers;
  int mDestinationEpsg = 4326;
  qint64 mMaximumFeatures = 0;
  bool mUseSpatialFilter = false;
};

} // namespace minigis
