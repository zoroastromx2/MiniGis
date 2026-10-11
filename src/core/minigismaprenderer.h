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

  /** Limite de elementos por capa; 0 = sin limite. */
  void setMaximumFeaturesPerLayer(qint64 maximum) { mMaximumFeatures = maximum; }

  QImage render(const Rectangle &extent, const QSize &size, const QColor &background) const;

  /** Extension combinada de las capas visibles. */
  Rectangle fullExtent() const;

private:
  void paintLayer(QPainter &painter, const MapToPixel &transform, const VectorLayer &layer) const;

  std::vector<std::shared_ptr<VectorLayer>> mLayers;
  int mDestinationEpsg = 4326;
  qint64 mMaximumFeatures = 0;
};

} // namespace minigis
