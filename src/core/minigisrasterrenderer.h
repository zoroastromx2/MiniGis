#pragma once

#include <QImage>
#include <QRectF>
#include <QSize>

#include <memory>
#include <vector>

#include "minigisrectangle.h"

class QPainter;

namespace minigis
{

class RasterLayer;
class MapToPixel;

/**
 * Dibujado de capas raster sobre un QPainter.
 *
 * Principio de diseno: por cuadro solo se lee la parte visible del raster, a la
 * resolucion del lienzo. Nunca se materializa la imagen completa en memoria; un
 * GeoTIFF de 8192x8192 ocupa cientos de MB y no cabe en un cuadro fluido.
 *
 * En esta primera version (T4) solo se dibuja cuando la capa y el lienzo
 * comparten SRC, sin estiramiento ni warp. T6 anade el warp y T7 el contraste.
 */
class RasterRenderer
{
public:
  struct Options
  {
    /** Durante el arrastre/zoom se lee a media resolucion. */
    bool draft = false;
  };

  struct FrameStats
  {
    /** Bytes leidos de GDAL en este cuadro (todas las bandas). */
    qint64 bytesRead = 0;
    qint64 pixelsDrawn = 0;
    int layersDrawn = 0;
  };

  void paint(QPainter &painter, const std::vector<std::shared_ptr<RasterLayer>> &layers,
             const Rectangle &extent, const QSize &size, const Options &options, int destinationEpsg);

  const FrameStats &lastStats() const { return mStats; }

private:
  /**
   * Compone la imagen de una capa para el rectangulo de pantalla indicado, o
   * devuelve una imagen nula si no se puede dibujar (sin geotransformacion,
   * rotada, fuera de la vista...).
   *
   * `destination` recibe el rectangulo donde hay que dibujar la imagen: es el
   * que corresponde a la ventana de pixeles de origen leida, que puede ser algo
   * mayor que `target` al ajustarse a pixeles enteros. Dibujarla en `target`
   * desplazaria el contenido y descuadraria las hojas de un mosaico.
   */
  QImage compose(const RasterLayer &layer, const QRectF &target, const MapToPixel &transform, int destinationEpsg,
                 bool draft, FrameStats &stats, QRectF *destination) const;

  FrameStats mStats;
};

} // namespace minigis
