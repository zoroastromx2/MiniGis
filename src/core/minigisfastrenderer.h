#pragma once

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QSize>

#include <cstdint>
#include <vector>

#include "minigisgeometrystore.h"
#include "minigisrectangle.h"

namespace minigis
{

class MapToPixel;
class Style;

/**
 * Renderizado vectorial acelerado sobre GeometryStore.
 *
 * Diferencias con MapRenderer (camino clasico, que se conserva intacto):
 *
 *   1. Descarte por extension visible: el R-tree devuelve solo los elementos que
 *      cortan la vista, en lugar de recorrer la capa entera.
 *   2. Sin llamadas a OGR por vertice: las coordenadas ya estan en buffers planos.
 *   3. Simplificacion Douglas-Peucker por nivel de zoom, en unidades de mapa
 *      derivadas de la escala actual (error maximo de ~medio pixel).
 *   4. Dibujado por lotes: un QPainterPath por capa y tipo de geometria y una
 *      sola llamada a drawPath, en lugar de fijar pluma/pincel por elemento.
 *   5. Presupuesto de vertices por cuadro: si la vista tiene demasiado detalle,
 *      se sube de nivel de simplificacion hasta entrar en el presupuesto. Esto
 *      acota el coste del cuadro en lugar de dejar que crezca sin limite.
 *   6. Modo borrador: durante el arrastre/zoom se desactiva el suavizado y se
 *      simplifica mas; al soltar, el lienzo pide un cuadro de calidad.
 */
class FastMapRenderer
{
public:
  struct Options
  {
    bool antialiasing = true;
    /** Modo borrador: se usa durante las interacciones (arrastre, rueda). */
    bool draft = false;
    /** Error maximo de simplificacion en pixeles (calidad). */
    double simplifyPixels = 0.5;
    /** Error maximo de simplificacion en pixeles (borrador). */
    double draftSimplifyPixels = 1.5;
    /** Vertices maximos por cuadro; 0 = sin limite. */
    qint64 vertexBudget = 600000;
    /** Intentos de subir de nivel antes de dibujar lo que haya. */
    int maximumLevelAttempts = 4;
    /**
     * Numero de elementos a partir del cual los contornos finos se trazan como
     * "hairline" (pluma cosmetica de 1 pixel de dispositivo, sin pasar por el
     * contorneador de Qt).
     *
     * El contorneado con suavizado es, con diferencia, la parte mas cara del
     * dibujado vectorial: medido con 12.000 poligonos (186.000 vertices),
     * 1.049 ms con pluma normal frente a 216 ms con pluma cosmetica. A un pixel
     * de grosor la diferencia visual es minima, asi que por encima de este
     * umbral se prioriza la fluidez. Con `draft` se usa siempre.
     */
    qint64 hairlineFeatureThreshold = 2000;
  };

  struct FrameStats
  {
    qint64 candidateFeatures = 0; // elementos devueltos por el R-tree
    qint64 drawnFeatures = 0;
    qint64 drawnVertices = 0;
    qint64 totalFeatures = 0;
    int level = 0;
    double tolerance = 0.0;
    qint64 queryMs = 0;
    qint64 pathMs = 0;
    qint64 paintMs = 0;
  };

  struct Layer
  {
    const GeometryStore *store = nullptr;
    const Style *style = nullptr;
    bool visible = true;
  };

  /**
   * Dibuja las capas sobre un QPainter ya existente. Permite mezclar capas del
   * camino acelerado con capas que aun no tienen almacen (camino clasico) sin
   * alterar el orden de apilado.
   */
  void paint(QPainter &painter, const std::vector<Layer> &layers, const Rectangle &extent, const QSize &size,
             const Options &options);

  /** Imagen nueva con las capas dibujadas; comodidad para capturas y pruebas. */
  QImage render(const std::vector<Layer> &layers, const Rectangle &extent, const QSize &size,
                const QColor &background, const Options &options);

  const FrameStats &lastStats() const { return mStats; }

private:
  struct Batch
  {
    /**
     * Trazados de poligonos y de lineas, troceados.
     *
     * Un unico QPainterPath con miles de subpaths hace que el rellenado de Qt
     * escale de forma no lineal (medido: 8 s con 12.000 subpaths frente a
     * decenas de milisegundos troceado). El troceado mantiene el ahorro de
     * llamadas de dibujo sin caer en ese caso patologico.
     */
    std::vector<QPainterPath> polygonChunks;
    std::vector<QPainterPath> lineChunks;
    std::vector<QPointF> points;
    qint64 vertices = 0;
    qint64 features = 0;
  };

  /** Construye los lotes de una capa en un nivel de detalle dado. */
  void buildBatch(Batch &batch, const GeometryStore &store, const GeometryStore::Level &level,
                  const MapToPixel &transform) const;

  FrameStats mStats;
  std::vector<uint32_t> mCandidates;
};

} // namespace minigis
