#include "minigismapcanvas.h"

#include "minigisvectorlayer.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace minigis
{
namespace
{

constexpr double kZoomStep = 1.25;
constexpr int kStatusTimeoutMs = 4000;

} // namespace

MapCanvas::MapCanvas(QWidget *parent)
  : QWidget(parent)
{
  setMinimumSize(320, 240);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setFocusPolicy(Qt::StrongFocus);
  setAcceptDrops(true);
  setMouseTracking(true);
  setCursor(Qt::CrossCursor);
  setAutoFillBackground(false);
  mRenderer.setDestinationEpsg(mDestinationEpsg);
}

std::shared_ptr<VectorLayer> MapCanvas::addLayer(const std::shared_ptr<VectorLayer> &layer, bool zoomToExtent)
{
  if (!layer)
    return {};

  // No se admiten duplicados: evita dibujar dos veces el mismo archivo.
  for (const std::shared_ptr<VectorLayer> &existing : mLayers)
  {
    if (existing && existing->sourcePath() == layer->sourcePath())
    {
      setCurrentLayer(existing);
      emit statusMessage(QStringLiteral("La capa «%1» ya estaba cargada.").arg(layer->name()), kStatusTimeoutMs);
      return existing;
    }
  }

  mLayers.push_back(layer);
  mCurrentLayer = layer;

  emit layersChanged();
  emit currentLayerChanged();

  // Primera capa (o peticion explicita): encuadrar su extension.
  if (zoomToExtent || !mExtent.isValid())
    zoomToLayer(layer);
  else
    update();

  return layer;
}

bool MapCanvas::removeLayer(const std::shared_ptr<VectorLayer> &layer)
{
  const auto it = std::find(mLayers.begin(), mLayers.end(), layer);
  if (it == mLayers.end())
    return false;

  mLayers.erase(it);
  if (mCurrentLayer == layer)
    mCurrentLayer = mLayers.empty() ? nullptr : mLayers.back();

  emit layersChanged();
  emit currentLayerChanged();
  update();
  return true;
}

void MapCanvas::clearLayers()
{
  if (mLayers.empty())
    return;

  mLayers.clear();
  mCurrentLayer.reset();
  mExtent.setNull();

  emit layersChanged();
  emit currentLayerChanged();
  update();
}

bool MapCanvas::contains(const std::shared_ptr<VectorLayer> &layer) const
{
  return std::find(mLayers.begin(), mLayers.end(), layer) != mLayers.end();
}

void MapCanvas::setCurrentLayer(const std::shared_ptr<VectorLayer> &layer)
{
  if (mCurrentLayer == layer)
    return;

  mCurrentLayer = layer;
  emit currentLayerChanged();
}

void MapCanvas::setDestinationEpsg(int epsg)
{
  mDestinationEpsg = epsg;
  mRenderer.setDestinationEpsg(epsg);
  update();
}

void MapCanvas::setExtent(const Rectangle &extent)
{
  if (!extent.isValid() || extent == mExtent)
    return;

  mExtent = extent;
  // Toda extension que se fija esta ajustada al tamano actual del widget: hay
  // que recordarlo para que el proximo resizeEvent calcule bien la escala (si
  // no, la escala se va multiplicando en cada cambio de tamano).
  mLastSize = size();

  emit extentChanged(mExtent);
  emit scaleChanged(mapUnitsPerPixel());
  update();
}

Rectangle MapCanvas::layersExtent() const
{
  Rectangle result;
  for (const std::shared_ptr<VectorLayer> &layer : mLayers)
  {
    if (!layer || !layer->isVisible())
      continue;
    // La extension se pide en el SRC del lienzo: si la capa viene en otro SRC,
    // encuadrarla con sus propias unidades la dejaria fuera de la vista.
    result.combine(layer->extentInEpsg(mDestinationEpsg));
  }
  return result;
}

Rectangle MapCanvas::adjustedExtent(const Rectangle &target) const
{
  if (!target.isValid())
    return {};

  const double aspect = double(std::max(1, width())) / double(std::max(1, height()));

  // Una capa de un solo punto tiene extension degenerada: se le da un tamano
  // minimo razonable segun parezca en grados o en unidades proyectadas.
  const bool looksGeographic = std::abs(target.centerX()) <= 180.0 && std::abs(target.centerY()) <= 90.0;
  const double minimumSpan = looksGeographic ? 0.02 : 1000.0;

  return target.withMinimumSize(minimumSpan).buffered(0.05).adjustedToAspect(aspect);
}

void MapCanvas::zoomToFullExtent()
{
  const Rectangle full = layersExtent();
  if (!full.isValid())
  {
    emit statusMessage(QStringLiteral("Ninguna capa visible tiene extension conocida."), kStatusTimeoutMs);
    return;
  }
  setExtent(adjustedExtent(full));
}

void MapCanvas::zoomToLayer(const std::shared_ptr<VectorLayer> &layer)
{
  if (!layer)
    return;

  const Rectangle layerExtent = layer->extentInEpsg(mDestinationEpsg);
  if (!layerExtent.isValid())
  {
    emit statusMessage(QStringLiteral("La capa «%1» no informa de su extension.").arg(layer->name()), kStatusTimeoutMs);
    return;
  }

  setCurrentLayer(layer);
  setExtent(adjustedExtent(layerExtent));
}

void MapCanvas::zoomIn()
{
  zoomBy(1.0 / kZoomStep);
}

void MapCanvas::zoomOut()
{
  zoomBy(kZoomStep);
}

void MapCanvas::zoomBy(double factor, const QPointF &anchorPixel)
{
  if (!mExtent.isValid() || factor <= 0.0 || size().isEmpty())
    return;

  const MapToPixel transform(mExtent, size());
  if (!transform.isValid())
    return;

  const QPointF anchor = anchorPixel.isNull()
                           ? QPointF(size().width() / 2.0, size().height() / 2.0)
                           : anchorPixel;
  const QPointF mapAnchor = transform.toMap(anchor);

  const double newWidth = mExtent.width() * factor;
  const double newHeight = mExtent.height() * factor;

  // El punto de mapa bajo el cursor (o el centro) se queda donde estaba.
  const double fractionX = anchor.x() / double(std::max(1, size().width()));
  const double fractionY = anchor.y() / double(std::max(1, size().height()));

  const double newMinX = mapAnchor.x() - fractionX * newWidth;
  const double newMaxY = mapAnchor.y() + fractionY * newHeight;

  const Rectangle next(newMinX, newMaxY - newHeight, newMinX + newWidth, newMaxY);
  setExtent(next.adjustedToAspect(double(std::max(1, size().width())) / double(std::max(1, size().height()))));
}

double MapCanvas::mapUnitsPerPixel() const
{
  const MapToPixel transform(mExtent, size());
  return transform.isValid() ? transform.mapUnitsPerPixel() : 1.0;
}
QImage MapCanvas::renderToImage(const QSize &size)
{
  QElapsedTimer timer;
  timer.start();

  mRenderer.setLayers(mLayers);
  mRenderer.setDestinationEpsg(mDestinationEpsg);
  mRenderer.setMaximumFeaturesPerLayer(mMaximumFeatures);

  Rectangle extent = mExtent;
  if (!extent.isValid())
    extent = adjustedExtent(layersExtent());

  const QImage image = mRenderer.render(extent, size, QColor(248, 248, 244));
  mLastRenderMs = timer.elapsed();
  return image;
}

void MapCanvas::paintEvent(QPaintEvent *event)
{
  Q_UNUSED(event)

  QPainter painter(this);
  painter.drawImage(0, 0, renderToImage(size()));

  if (mLayers.empty())
    paintEmptyState(painter);
}

void MapCanvas::paintEmptyState(QPainter &painter) const
{
  QFont titleFont = font();
  titleFont.setPointSizeF(titleFont.pointSizeF() + 4.0);
  titleFont.setBold(true);

  const QRect area = rect().adjusted(0, 0, 0, 0);
  const QRect titleRect(area.left(), area.top() + area.height() / 2 - 40, area.width(), 34);
  const QRect hintRect(area.left(), titleRect.bottom() + 6, area.width(), 24);

  painter.setPen(QColor(70, 90, 110));
  painter.setFont(titleFont);
  painter.drawText(titleRect, Qt::AlignCenter, QStringLiteral("MiniGIS"));

  painter.setPen(QColor(110, 120, 130));
  painter.setFont(font());
  painter.drawText(hintRect, Qt::AlignCenter,
                   QStringLiteral("Arrastra un archivo .shp aqui, o usa Archivo > Abrir capa (Ctrl+O)"));
}

void MapCanvas::resizeEvent(QResizeEvent *event)
{
  // Se conserva el centro y la escala al cambiar el tamano de la ventana.
  if (mExtent.isValid() && mLastSize.width() > 0 && mLastSize.height() > 0)
  {
    const double unitsPerPixel = mExtent.width() / double(mLastSize.width());
    const double centerX = mExtent.centerX();
    const double centerY = mExtent.centerY();
    const double newWidth = width() * unitsPerPixel;
    const double newHeight = height() * unitsPerPixel;
    mExtent = Rectangle(centerX - newWidth / 2.0, centerY - newHeight / 2.0,
                        centerX + newWidth / 2.0, centerY + newHeight / 2.0);
    emit extentChanged(mExtent);
    emit scaleChanged(mapUnitsPerPixel());
  }

  mLastSize = size();
  QWidget::resizeEvent(event);
}

void MapCanvas::wheelEvent(QWheelEvent *event)
{
  const int delta = event->angleDelta().y();
  if (delta == 0)
  {
    event->ignore();
    return;
  }

  const double steps = double(delta) / 120.0;
  zoomBy(std::pow(kZoomStep, -steps), event->position());
  event->accept();
}

void MapCanvas::mousePressEvent(QMouseEvent *event)
{
  if ((event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) && mExtent.isValid())
  {
    mPanning = true;
    mDragOrigin = event->position();
    mDragStartExtent = mExtent;
    updateCursorShape();
    event->accept();
    return;
  }

  QWidget::mousePressEvent(event);
}

void MapCanvas::mouseMoveEvent(QMouseEvent *event)
{
  const QPointF position = event->position();

  if (mPanning && mExtent.isValid())
  {
    const double unitsPerPixel = mapUnitsPerPixel();
    const QPointF delta = position - mDragOrigin;

    // Al arrastrar hacia la derecha el mapa se mueve con el raton.
    setExtent(Rectangle(mDragStartExtent.xMin() - delta.x() * unitsPerPixel,
                        mDragStartExtent.yMin() + delta.y() * unitsPerPixel,
                        mDragStartExtent.xMax() - delta.x() * unitsPerPixel,
                        mDragStartExtent.yMax() + delta.y() * unitsPerPixel));
  }

  const MapToPixel transform(mExtent, size());
  if (transform.isValid())
    emit cursorMoved(transform.toMap(position), mDestinationEpsg > 0);

  QWidget::mouseMoveEvent(event);
}

void MapCanvas::mouseReleaseEvent(QMouseEvent *event)
{
  if (mPanning && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton))
  {
    mPanning = false;
    updateCursorShape();
    event->accept();
    return;
  }

  QWidget::mouseReleaseEvent(event);
}

void MapCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
  if (event->button() == Qt::LeftButton)
  {
    zoomBy(1.0 / kZoomStep, event->position());
    event->accept();
    return;
  }

  QWidget::mouseDoubleClickEvent(event);
}

void MapCanvas::keyPressEvent(QKeyEvent *event)
{
  if (!mExtent.isValid())
  {
    QWidget::keyPressEvent(event);
    return;
  }

  const double stepX = mExtent.width() * 0.15;
  const double stepY = mExtent.height() * 0.15;

  switch (event->key())
  {
    case Qt::Key_Left:
      setExtent(Rectangle(mExtent.xMin() - stepX, mExtent.yMin(), mExtent.xMax() - stepX, mExtent.yMax()));
      break;
    case Qt::Key_Right:
      setExtent(Rectangle(mExtent.xMin() + stepX, mExtent.yMin(), mExtent.xMax() + stepX, mExtent.yMax()));
      break;
    case Qt::Key_Up:
      setExtent(Rectangle(mExtent.xMin(), mExtent.yMin() + stepY, mExtent.xMax(), mExtent.yMax() + stepY));
      break;
    case Qt::Key_Down:
      setExtent(Rectangle(mExtent.xMin(), mExtent.yMin() - stepY, mExtent.xMax(), mExtent.yMax() - stepY));
      break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
      zoomIn();
      break;
    case Qt::Key_Minus:
      zoomOut();
      break;
    case Qt::Key_F5:
      zoomToFullExtent();
      break;
    default:
      QWidget::keyPressEvent(event);
      return;
  }

  event->accept();
}

void MapCanvas::leaveEvent(QEvent *event)
{
  emit cursorLeft();
  QWidget::leaveEvent(event);
}

void MapCanvas::dragEnterEvent(QDragEnterEvent *event)
{
  if (event->mimeData()->hasUrls())
    event->acceptProposedAction();
  else
    event->ignore();
}

void MapCanvas::dragMoveEvent(QDragMoveEvent *event)
{
  if (event->mimeData()->hasUrls())
    event->acceptProposedAction();
  else
    event->ignore();
}

void MapCanvas::dropEvent(QDropEvent *event)
{
  QStringList paths;
  const QList<QUrl> urls = event->mimeData()->urls();
  for (const QUrl &url : urls)
  {
    if (url.isLocalFile())
      paths << url.toLocalFile();
  }

  if (paths.isEmpty())
  {
    event->ignore();
    return;
  }

  event->acceptProposedAction();
  emit filesDropped(paths);
}

void MapCanvas::updateCursorShape()
{
  setCursor(mPanning ? Qt::ClosedHandCursor : Qt::CrossCursor);
}

} // namespace minigis
