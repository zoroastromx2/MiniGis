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
class RasterMosaic;
class MapToPixel;
struct RasterStyle;

/**
 * Dibujado de capas raster sobre un QPainter.
 *
 * Principio de diseno: por cuadro solo se lee la parte visible del raster, a la
 * resolucion del lienzo. Nunca se materializa la imagen completa en memoria; un
 * GeoTIFF de 8192x8192 ocupa cientos de MB y no cabe en un cuadro fluido.
 *
 * Un mosaico se dibuja con `paintMosaic`, que es el camino que escala a cientos
 * de hojas: descarta las que no cortan la vista antes de crear warps y agrupa la
 * escala en niveles de detalle para reutilizar las vistas entre pasos de zoom.
 */
class RasterRenderer
{
public:
  struct Options
  {
    /** Durante el arrastre/zoom se lee a media resolucion. */
    bool draft = false;
    /**
     * Maximo de hojas de mosaico que se leen en un cuadro (0 = sin limite).
     * Durante la carga de una cobertura se dibuja un subconjunto para que
     * ningun cuadro pase del presupuesto.
     */
    int tileBudget = 0;
    /**
     * Tamano de pixel forzado para la rejilla del warp (unidades de mapa por
     * pixel). Con 0 se usa el de la escala exacta del lienzo. El mosaico lo fija
     * a una potencia de 2 para que un paso de zoom no rehaga el warp de cada
     * hoja (la causa medida de los 6,5 ms por hoja y cuadro).
     */
    double warpPixelSize = 0.0;
  };

  struct FrameStats
  {
    /** Bytes leidos de GDAL en este cuadro (todas las bandas). */
    qint64 bytesRead = 0;
    qint64 pixelsDrawn = 0;
    int layersDrawn = 0;
    /** Hojas de mosaico leidas en este cuadro. */
    int tilesRead = 0;
    /** Hojas del mosaico que cortaban la vista (leidas o no por el presupuesto). */
    int tilesVisible = 0;
    /** Vistas reproyectadas creadas en este cuadro (0 = todas se reutilizaron). */
    int warpsCreated = 0;
  };

  /** Deja las estadisticas a cero al empezar un cuadro. */
  void resetStats();

  void paint(QPainter &painter, const std::vector<std::shared_ptr<RasterLayer>> &layers,
             const Rectangle &extent, const QSize &size, const Options &options, int destinationEpsg);

  /** Dibuja una capa suelta como una lectura de ventana. */
  void paintLayer(QPainter &painter, const RasterLayer &layer, const Rectangle &extent, const QSize &size,
                  const Options &options, int destinationEpsg, const RasterStyle *styleOverride = nullptr);

  /**
   * Dibuja un mosaico en el orden de sus hojas. Se descarta lo que no corta la
   * vista antes de pedir la vista (crear un warp por hoja fuera de pantalla fue
   * el coste medido de cargar 508 imagenes como capas sueltas).
   */
  void paintMosaic(QPainter &painter, const RasterMosaic &mosaic, const Rectangle &extent, const QSize &size,
                   const Options &options, int destinationEpsg);

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
  QImage compose(const RasterLayer &layer, const RasterStyle &style, const QRectF &target,
                 const MapToPixel &transform, int destinationEpsg, double warpPixelSize, bool draft,
                 FrameStats &stats, QRectF *destination) const;

  FrameStats mStats;
};

} // namespace minigis
