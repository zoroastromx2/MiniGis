#pragma once

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QWidget>

#include <memory>
#include <vector>

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
  explicit MapCanvas(QWidget *parent = nullptr);

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

  /** Imagen del estado actual; la usa el modo --snapshot y el propio paintEvent. */
  QImage renderToImage(const QSize &size);

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

  std::vector<std::shared_ptr<VectorLayer>> mLayers;
  std::shared_ptr<VectorLayer> mCurrentLayer;
  MapRenderer mRenderer;
  Rectangle mExtent;
  Rectangle mDragStartExtent;
  QSize mLastSize;
  QPointF mDragOrigin;
  bool mPanning = false;
  int mDestinationEpsg = 4326;
  qint64 mMaximumFeatures = 0;
  qint64 mLastRenderMs = 0;
};

} // namespace minigis
