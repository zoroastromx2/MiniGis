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
    /**
     * Elementos cuya geometria se lee del archivo por cuadro en el modo
     * envolventes (capas que no caben en memoria). Si la vista contiene mas, se
     * toma uno de cada k: el R-tree devuelve los elementos en orden espacial, de
     * modo que la muestra queda repartida por toda la vista. 0 = sin limite.
     */
    qint64 readBudget = 40000;
    /**
     * Tamano minimo (en pixeles) que debe ocupar un elemento para leer su
     * geometria en el modo envolventes. Por debajo, la capa se dibuja como nube
     * de puntos con las envolventes: a esa escala el contorno no se distingue y
     * leerlo costaria segundos de I/O dispersa. 0 desactiva la degradacion.
     */
    double minimumGeometryPixels = 2.0;
    /** Puntos maximos que se dibujan en esa nube (0 = sin limite). */
    qint64 dotBudget = 150000;
    /**
     * Puntos maximos que se dibujan por cuadro en cualquier modo. Los marcadores
     * no se pueden simplificar (cada elemento es un vertice), asi que una capa de
     * millones de puntos se muestrea para acotar el coste del cuadro. 0 = sin
     * limite.
     */
    qint64 pointBudget = 120000;
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
    /** Modo envolventes: elementos leidos del archivo en este cuadro y omitidos. */
    qint64 readFeatures = 0;
    qint64 skippedFeatures = 0;
    /** Elementos dibujados como punto (envolvente) por ser sub-pixel. */
    qint64 dotFeatures = 0;
    bool envelopeMode = false;
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
                  const MapToPixel &transform, qint64 pointStride) const;

  /**
   * Igual, a partir de arrays planos por elemento. Los dos modos del almacen
   * (memoria y envolventes) comparten este bucle: solo cambia de donde salen los
   * anillos de cada elemento.
   */
  void buildBatch(Batch &batch, const GeometryStore::Level &level, const std::vector<uint8_t> &ringKinds,
                  const std::vector<uint32_t> &entryFirstRing, const std::vector<uint32_t> &entryRingCount,
                  const std::vector<uint32_t> &entries, const MapToPixel &transform,
                  qint64 pointStride) const;

  FrameStats mStats;
  std::vector<uint32_t> mCandidates;
  /** Secuencia 0..n-1 reutilizada por el modo envolventes (subconjunto por posicion). */
  std::vector<uint32_t> mPositions;
  /** Puntos de la nube de envolventes (elementos sub-pixel). */
  std::vector<QPointF> mDots;
};

} // namespace minigis
