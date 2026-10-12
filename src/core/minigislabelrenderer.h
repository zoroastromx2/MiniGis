#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <vector>

#include "minigislabelstyle.h"

class QPainter;
class QSize;

namespace minigis
{

class GeometryStore;
class MapToPixel;
class VectorLayer;

/**
 * Dibuja las etiquetas de los elementos geograficos.
 *
 * Se ejecuta DESPUES de la geometria y siempre sobre el lienzo completo (no sobre
 * la franja que se repinta al desplazar), porque el texto no se puede reutilizar
 * desplazando una imagen: una etiqueta copiada quedaria en el sitio equivocado.
 * Como el lienzo guarda su cuadro cacheado antes de esta pasada, la copia sigue
 * siendo valida y las etiquetas se redibujan encima.
 *
 * Lo que hace que un etiquetado sea usable, y que aqui se resuelve:
 *
 *   * **Solo lo que se ve**: los elementos salen de la consulta al R-tree del
 *     almacen, no de recorrer la capa.
 *   * **Saturacion acotada**: limite de etiquetas por cuadro, distancia minima
 *     entre ellas (rejilla de colision) y descarte de textos repetidos.
 *   * **Legibilidad**: halo opcional y tamano minimo del elemento, para no
 *     etiquetar lo que no se distingue ni tapar el mapa con texto.
 *   * **Coste acotado**: los textos se cachean por elemento entre cuadros y el
 *     numero de lecturas de atributos por cuadro esta limitado, de modo que
 *     desplazar el mapa no vuelve a leer los mismos campos.
 */
class LabelRenderer
{
public:
  struct Options
  {
    /** Interruptor global del lienzo: apaga todas las etiquetas de golpe. */
    bool enabled = true;
    /** En modo borrador se etiquetan menos elementos (el mapa se esta moviendo). */
    bool draft = false;
  };

  struct Layer
  {
    const VectorLayer *layer = nullptr;
    const GeometryStore *store = nullptr;
    const LabelStyle *style = nullptr;
  };

  struct FrameStats
  {
    qint64 layers = 0;
    qint64 candidates = 0;
    qint64 placed = 0;
    qint64 skippedCollision = 0;
    qint64 skippedDuplicate = 0;
    qint64 skippedEmpty = 0;
    qint64 skippedSmall = 0;
    qint64 skippedByLimit = 0;
    qint64 valueReads = 0;   // lecturas de atributos en este cuadro
    qint64 cacheHits = 0;
    /** Etiquetas dibujadas sin halo porque la plataforma no da contornos de glifo. */
    qint64 textFallbacks = 0;
    /** Etiquetas cuyo tamano hubo que recortar por estar fuera de los limites. */
    qint64 sizeClamped = 0;
    qint64 paintMs = 0;
  };

  void paint(QPainter &painter, const std::vector<Layer> &layers, const MapToPixel &transform,
             const QSize &size, const Options &options);

  const FrameStats &lastStats() const { return mStats; }

  /** Olvida los textos cacheados (al cambiar campos, fuente o la propia capa). */
  void clearCache();

  /** Deja las estadisticas a cero (cuadro sin etiquetas). */
  void resetStats() { mStats = FrameStats(); }

private:
  /** Rejilla de celdas para comprobar solapes sin recorrer todas las etiquetas. */
  class CollisionGrid
  {
  public:
    void clear() { mCells.clear(); }
    bool collides(const QRectF &rect, double margin) const;
    void insert(const QRectF &rect);

  private:
    static constexpr double kCellSize = 32.0;
    QHash<qint64, QVector<QRectF>> mCells;
  };

  /** Texto de la etiqueta de un elemento, o vacio si no hay nada que mostrar. */
  QString textFor(const Layer &layer, qint64 row, const QVector<int> &fieldIndices, quint64 cacheKey);

  /** Punto de anclaje en coordenadas de mapa. */
  bool anchorFor(const Layer &layer, uint32_t feature, QPointF &mapAnchor, double &featureSize) const;

  FrameStats mStats;
  CollisionGrid mGrid;
  QHash<qint64, QString> mTextCache;
  quint64 mTextCacheKey = 0;
  quint64 mFieldCacheKey = 0;
  QVector<int> mFieldIndices;
  /** Elementos visibles del cuadro (consulta al R-tree), reutilizado entre capas. */
  std::vector<uint32_t> mCandidates;
  /** La plataforma puede trazar contornos de glifo (se decide al primer uso). */
  bool mGlyphOutlines = true;
};

} // namespace minigis
