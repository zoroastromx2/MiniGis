#pragma once

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QWidget>

#include <memory>
#include <vector>

#include "minigisfastrenderer.h"
#include "minigismaprenderer.h"
#include "minigisrectangle.h"

namespace minigis
{

class VectorLayer;

/**
 * Lienzo de mapa (equivalente simplificado a QgsMapCanvas).
 *
 * Responsabilidades:
 *   * mantener la lista de capas y la capa activa
 *   * gestionar la extension visible (zoom con la rueda, paneo arrastrando)
 *   * pedir el renderizado a MapRenderer y pintarlo
 */
class MapCanvas : public QWidget
{
  Q_OBJECT

public:
  /**
   * Motor de dibujo del lienzo.
   *
   *   * `Legacy`: lectura directa de la capa con OGR en cada cuadro (el
   *     comportamiento historico; se conserva para poder comparar y como red de
   *     seguridad).
   *   * `Fast`: usa el GeometryStore de cada capa (R-tree + simplificacion +
   *     dibujado por lotes). Las capas que aun no tienen almacen se dibujan con
   *     el camino clasico, respetando el orden de apilado.
   *   * `Auto`: `Fast` en cuanto alguna capa tiene almacen; si no, `Legacy`.
   */
  enum class RendererBackend
  {
    Auto,
    Legacy,
    Fast
  };

  explicit MapCanvas(QWidget *parent = nullptr);
  ~MapCanvas() override;

  std::shared_ptr<VectorLayer> addLayer(const std::shared_ptr<VectorLayer> &layer, bool zoomToExtent = false);
  bool removeLayer(const std::shared_ptr<VectorLayer> &layer);
  void clearLayers();
  const std::vector<std::shared_ptr<VectorLayer>> &layers() const { return mLayers; }
  bool contains(const std::shared_ptr<VectorLayer> &layer) const;

  std::shared_ptr<VectorLayer> currentLayer() const { return mCurrentLayer; }
  void setCurrentLayer(const std::shared_ptr<VectorLayer> &layer);

  Rectangle extent() const { return mExtent; }
  void setExtent(const Rectangle &extent);

  void zoomToFullExtent();
  void zoomToLayer(const std::shared_ptr<VectorLayer> &layer);
  void zoomIn();
  void zoomOut();
  void zoomBy(double factor, const QPointF &anchorPixel = QPointF());

  double mapUnitsPerPixel() const;

  int destinationEpsg() const { return mDestinationEpsg; }
  void setDestinationEpsg(int epsg);

  /** Limite de elementos dibujados por capa (0 = sin limite). */
  void setMaximumFeaturesPerLayer(qint64 maximum) { mMaximumFeatures = maximum; }
  qint64 maximumFeaturesPerLayer() const { return mMaximumFeatures; }

  void setRendererBackend(RendererBackend backend);
  RendererBackend rendererBackend() const { return mBackend; }

  /** Vertices maximos por cuadro en el camino acelerado (0 = sin limite). */
  void setVertexBudget(qint64 budget) { mFastOptions.vertexBudget = budget; }
  qint64 vertexBudget() const { return mFastOptions.vertexBudget; }

  /**
   * Elementos cuya geometria se lee del archivo por cuadro cuando la capa esta
   * indexada por envolventes (capas que no caben en memoria). 0 = sin limite.
   */
  void setReadBudget(qint64 budget)
  {
    mFastOptions.readBudget = budget;
    invalidateFrameCache();
  }
  qint64 readBudget() const { return mFastOptions.readBudget; }

  /**
   * Filtra en origen con el indice espacial del archivo. Solo afecta a las capas
   * que se dibujan con el camino clasico (las que no caben en el almacen).
   */
  void setUseSpatialFilter(bool enabled)
  {
    mRenderer.setUseSpatialFilter(enabled);
    invalidateFrameCache();
    update();
  }
  bool useSpatialFilter() const { return mRenderer.useSpatialFilter(); }

  /**
   * Modo borrador: se activa mientras se arrastra o se hace zoom y se desactiva
   * al parar. Sin suavizado, con simplificacion mas agresiva y a media
   * resolucion; el cuadro final se pide al terminar la interaccion.
   */
  void setDraftMode(bool draft);
  bool draftMode() const { return mDraftMode; }

  /** Estadisticas del ultimo cuadro acelerado (elementos y vertices dibujados). */
  const FastMapRenderer::FrameStats &fastFrameStats() const { return mFastRenderer.lastStats(); }

  /** Milisegundos del ultimo cuadro que reutilizo el anterior (0 si no lo hizo). */
  qint64 lastReuseMilliseconds() const { return mLastReuseMs; }

  /**
   * Imagen del estado actual; la usa el modo --snapshot, el banco de pruebas y
   * el propio paintEvent. Con `draft` se pide el cuadro de baja calidad que se
   * usa durante las interacciones.
   */
  QImage renderToImage(const QSize &size, bool draft = false);

  /** Milisegundos que tardo el ultimo render (se muestra en la barra de estado). */
  qint64 lastRenderMilliseconds() const { return mLastRenderMs; }

  /** Extension combinada de las capas visibles. */
  Rectangle layersExtent() const;

signals:
  void layersChanged();
  void currentLayerChanged();
  void cursorMoved(const QPointF &mapPoint, bool hasCrs);
  void cursorLeft();
  void extentChanged(const Rectangle &extent);
  void scaleChanged(double mapUnitsPerPixel);
  void filesDropped(const QStringList &paths);
  void statusMessage(const QString &message, int timeoutMs);

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  void updateCursorShape();
  Rectangle adjustedExtent(const Rectangle &target) const;
  void paintEmptyState(QPainter &painter) const;

  /** Activa el modo borrador y reinicia la cuenta para el cuadro de calidad. */
  void beginInteraction();

  /** Ajusta la extension a la rejilla de pixeles: evita costuras al reutilizar. */
  Rectangle snappedExtent(const Rectangle &extent) const;

  /**
   * Intenta reutilizar el cuadro anterior del mismo nivel de calidad cuando el
   * cambio de extension es solo un desplazamiento de un numero entero de pixeles
   * a la misma escala: copia la imagen anterior y deja en `exposed` la zona que
   * hay que volver a dibujar.
   */
  bool reusePreviousFrame(QImage &image, const QSize &size, bool draft, QRegion &exposed);

  /** Invalida las caches de cuadro (cambio de capas, de SRC o de tamano). */
  void invalidateFrameCache();

  std::vector<std::shared_ptr<VectorLayer>> mLayers;
  std::shared_ptr<VectorLayer> mCurrentLayer;
  MapRenderer mRenderer;
  FastMapRenderer mFastRenderer;
  FastMapRenderer::Options mFastOptions;
  QTimer *mQualityTimer = nullptr;
  RendererBackend mBackend = RendererBackend::Auto;
  Rectangle mExtent;
  Rectangle mDragStartExtent;
  QSize mLastSize;
  QPointF mDragOrigin;
  bool mPanning = false;
  bool mDraftMode = false;
  int mDestinationEpsg = 4326;
  qint64 mMaximumFeatures = 0;
  qint64 mLastRenderMs = 0;

  // Cache del ultimo cuadro de cada nivel de calidad, para reutilizarlo al
  // desplazar el mapa. Se guardan dos (calidad y borrador) porque durante un
  // arrastre se encadenan cuadros de borrador y, al soltar, uno de calidad: si
  // solo hubiera una entrada, cada cambio de nivel la invalidaria.
  struct FrameCache
  {
    QImage image;
    Rectangle extent;
    QSize size;
    int epsg = 0;
    bool valid = false;
  };

  FrameCache mFrameCaches[2]; // [0] calidad, [1] borrador
  qint64 mLastReuseMs = 0;
};

} // namespace minigis
