#include "minigismapcanvas.h"

#include "minigisgeometrystore.h"
#include "minigisrasterlayer.h"
#include "minigisvectorlayer.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QRegion>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cstring>
#include <cmath>

namespace minigis
{
namespace
{

constexpr double kZoomStep = 1.25;
constexpr int kStatusTimeoutMs = 4000;

/**
 * Tiempo de calma antes de volver a la calidad completa. Mientras el usuario
 * arrastra o gira la rueda se dibuja en modo borrador; al parar, este tiempo
 * despues, se pide un cuadro definitivo.
 */
constexpr int kQualityDelayMs = 140;

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

  // Un solo disparo: cada gesto reinicia la cuenta, de modo que solo se
  // renderiza en calidad cuando el usuario ha dejado de interactuar.
  mQualityTimer = new QTimer(this);
  mQualityTimer->setSingleShot(true);
  mQualityTimer->setInterval(kQualityDelayMs);
  connect(mQualityTimer, &QTimer::timeout, this, [this]() {
    if (!mDraftMode)
      return;
    setDraftMode(false);
  });
}

MapCanvas::~MapCanvas() = default;

void MapCanvas::setRendererBackend(RendererBackend backend)
{
  if (mBackend == backend)
    return;

  mBackend = backend;
  invalidateFrameCache();
  update();
}

void MapCanvas::setDraftMode(bool draft)
{
  if (mDraftMode == draft)
    return;

  mDraftMode = draft;
  mFastOptions.draft = draft;

  // No se invalidan las caches: cada nivel de calidad tiene la suya, de modo que
  // el cuadro de borrador anterior sigue sirviendo para el siguiente cuadro de
  // borrador (que es lo que hace fluido el arrastre) y el de calidad para el
  // siguiente cuadro de calidad. Invalidar ambas al entrar y salir del modo
  // borrador desperdiciaba justo la reutilizacion que se busca.
  update();
}

void MapCanvas::beginInteraction()
{
  if (!mDraftMode)
    setDraftMode(true);

  if (mQualityTimer)
    mQualityTimer->start();
}

Rectangle MapCanvas::snappedExtent(const Rectangle &extent) const
{
  if (!extent.isValid() || size().isEmpty())
    return extent;

  const MapToPixel transform(extent, size());
  const double scale = transform.pixelsPerMapUnit();
  if (!transform.isValid() || scale <= 0.0)
    return extent;

  // Se alinea xMin/yMin a la rejilla de pixeles: asi el desplazamiento entre dos
  // cuadros es un numero entero de pixeles y la copia del cuadro anterior encaja
  // sin costuras (ademas evita que los simbolos "vibren" al arrastrar).
  const double xMin = std::round(extent.xMin() * scale) / scale;
  const double yMin = std::round(extent.yMin() * scale) / scale;
  return Rectangle(xMin, yMin, xMin + extent.width(), yMin + extent.height());
}

void MapCanvas::invalidateFrameCache()
{
  ++mOptionsRevision;
  mFrameCaches[0].valid = false;
  mFrameCaches[0].image = QImage();
  mFrameCaches[1].valid = false;
  mFrameCaches[1].image = QImage();
}

void MapCanvas::refreshMap()
{
  invalidateFrameCache();
  update();
}

void MapCanvas::setLabelsVisible(bool visible)
{
  if (mLabelsVisible == visible)
    return;

  mLabelsVisible = visible;
  // El cuadro cacheado no lleva etiquetas (se dibujan despues), pero el lienzo
  // anterior si las mostraba: se descarta para no arrastrar nada.
  refreshMap();
}

void MapCanvas::drawLabels(QImage &image, const MapToPixel &transform, const QSize &size, bool draft)
{
  LabelRenderer::Options options;
  options.enabled = mLabelsVisible;
  options.draft = draft;

  if (!mLabelsVisible)
  {
    // Se dejan las estadisticas a cero, para que quien las consulte sepa que este
    // cuadro no lleva etiquetas y no lea las del cuadro anterior.
    mLabelRenderer.resetStats();
    return;
  }

  std::vector<LabelRenderer::Layer> labelLayers;
  labelLayers.reserve(mLayers.size());
  for (const std::shared_ptr<VectorLayer> &layer : mLayers)
  {
    if (!layer || !layer->isVisible())
      continue;

    labelLayers.push_back(
      LabelRenderer::Layer {layer.get(), layer->geometryStore().get(), &layer->labelStyle()});
  }

  if (labelLayers.empty())
  {
    mLabelRenderer.resetStats();
    return;
  }

  QPainter labelPainter(&image);
  labelPainter.setRenderHint(QPainter::Antialiasing, true);
  labelPainter.setRenderHint(QPainter::TextAntialiasing, true);
  mLabelRenderer.paint(labelPainter, labelLayers, transform, size, options);
}


quint64 MapCanvas::contentRevision() const
{
  // Mezcla sencilla: basta con que dos estados distintos den numeros distintos.
  quint64 revision = mOptionsRevision * 1099511628211ULL;
  for (const std::shared_ptr<VectorLayer> &layer : mLayers)
  {
    if (!layer)
      continue;
    revision ^= layer->revision() + 0x9E3779B97F4A7C15ULL + (revision << 6) + (revision >> 2);
  }
  for (const std::shared_ptr<RasterLayer> &layer : mRasterLayers)
  {
    if (!layer)
      continue;
    revision ^= layer->revision() + 0x9E3779B97F4A7C15ULL + (revision << 6) + (revision >> 2);
  }
  return revision;
}

bool MapCanvas::reusePreviousFrame(QImage &image, const QSize &size, bool draft, QRegion &exposed)
{
  exposed = QRegion(QRect(QPoint(0, 0), size));

  FrameCache &cache = mFrameCaches[draft ? 1 : 0];

  if (!cache.valid || cache.image.isNull())
    return false;

  if (cache.size != size || cache.epsg != mDestinationEpsg)
    return false;

  // Un cuadro dibujado con muestreo dependiente de la vista no se puede reutilizar
  // desplazandolo (ver FrameCache::viewDependent): se redibuja entero.
  if (cache.viewDependent)
    return false;

  // Si ha cambiado algo que afecta al dibujado (simbologia, visibilidad, opciones
  // del motor), el cuadro guardado ya no representa el estado actual: se
  // redibuja entero. Sin esta comprobacion, cambiar el estilo de una capa no se
  // reflejaba en el mapa hasta mover o redimensionar la vista.
  if (cache.revision != contentRevision())
    return false;

  if (!cache.extent.isValid() || !mExtent.isValid())
    return false;

  // Solo vale para desplazamientos puros: misma escala (mismo ancho y alto).
  const double widthRatio = mExtent.width() / cache.extent.width();
  const double heightRatio = mExtent.height() / cache.extent.height();
  if (std::abs(widthRatio - 1.0) > 1e-9 || std::abs(heightRatio - 1.0) > 1e-9)
    return false;

  const MapToPixel transform(mExtent, size);
  if (!transform.isValid())
    return false;

  const double scale = transform.pixelsPerMapUnit();
  const double rawShiftX = (mExtent.xMin() - cache.extent.xMin()) * scale;
  const double rawShiftY = (cache.extent.yMax() - mExtent.yMax()) * scale;

  const double shiftX = std::round(rawShiftX);
  const double shiftY = std::round(rawShiftY);

  // Si el desplazamiento no cae en pixeles enteros, reutilizar dejaria costuras.
  if (std::abs(rawShiftX - shiftX) > 0.25 || std::abs(rawShiftY - shiftY) > 0.25)
    return false;

  const int dx = static_cast<int>(shiftX);
  const int dy = static_cast<int>(shiftY);

  if (dx == 0 && dy == 0)
  {
    // Mismo encuadre: el cuadro anterior sigue siendo valido tal cual.
    image = cache.image;
    exposed = QRegion();
    return true;
  }

  if (std::abs(dx) >= size.width() || std::abs(dy) >= size.height())
    return false;

  // Copia por filas (mismo formato y tamano: es un memcpy por linea).
  //
  // Signo del desplazamiento: para un punto fijo del mapa,
  //     pixel_cacheado = pixel_nuevo + (nuevo.xMin - cache.xMin) * escala
  // es decir, si xMin crece (la vista se desplaza hacia el este) el contenido
  // aparece MAS A LA IZQUIERDA en el cuadro nuevo. Por tanto el pixel `p` del
  // cuadro guardado va al pixel `p - dx`: el destino es el origen menos el
  // desplazamiento. Invertir esto —copiar hacia el lado contrario— desplaza todo
  // el contenido copiado el doble de lo debido y deja una costura vertical justo
  // en el limite de la franja repintada.
  const int destinationX = std::max(0, -dx);
  const int destinationY = std::max(0, -dy);
  const int sourceX = std::max(0, dx);
  const int sourceY = std::max(0, dy);
  const int copyWidth = size.width() - std::abs(dx);

  for (int row = 0; row < size.height() - std::abs(dy); ++row)
  {
    std::memcpy(image.scanLine(destinationY + row) + destinationX * 4,
                cache.image.constScanLine(sourceY + row) + sourceX * 4,
                static_cast<size_t>(copyWidth) * 4);
  }

  // Zona que cubre la copia: es EXACTAMENTE la que se acaba de escribir, y es lo
  // que hay que restar para saber que franja queda por dibujar. Calcularla aparte
  // (por ejemplo como el rectangulo desplazado) es un error facil y caro: al
  // invertir el signo de la copia, el rectangulo desplazado dejo de coincidir con
  // la zona realmente copiada y quedaba sin pintar una banda del ancho del
  // desplazamiento (el mapa se veia incompleto al arrastrar).
  const QRect covered(destinationX, destinationY, copyWidth, size.height() - std::abs(dy));
  exposed = QRegion(QRect(QPoint(0, 0), size)).subtracted(QRegion(covered));
  return true;
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

  invalidateFrameCache();
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

  invalidateFrameCache();
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
  if (mLayers.empty() && mRasterLayers.empty())
    return;

  invalidateFrameCache();
  mLayers.clear();
  mRasterLayers.clear();
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

std::shared_ptr<RasterLayer> MapCanvas::addRasterLayer(const std::shared_ptr<RasterLayer> &layer,
                                                       bool zoomToExtent)
{
  if (!layer)
    return {};

  // No se admiten duplicados: evita dibujar dos veces el mismo archivo.
  for (const std::shared_ptr<RasterLayer> &existing : mRasterLayers)
  {
    if (existing && existing->sourcePath() == layer->sourcePath())
    {
      emit statusMessage(QStringLiteral("La capa «%1» ya estaba cargada.").arg(layer->name()), kStatusTimeoutMs);
      return existing;
    }
  }

  invalidateFrameCache();
  mRasterLayers.push_back(layer);

  emit layersChanged();

  if (zoomToExtent || !mExtent.isValid())
    zoomToLayer(layer);
  else
    update();

  return layer;
}

bool MapCanvas::removeRasterLayer(const std::shared_ptr<RasterLayer> &layer)
{
  const auto it = std::find(mRasterLayers.begin(), mRasterLayers.end(), layer);
  if (it == mRasterLayers.end())
    return false;

  invalidateFrameCache();
  mRasterLayers.erase(it);

  emit layersChanged();
  update();
  return true;
}

void MapCanvas::clearRasterLayers()
{
  if (mRasterLayers.empty())
    return;

  invalidateFrameCache();
  mRasterLayers.clear();

  emit layersChanged();
  update();
}

bool MapCanvas::containsRaster(const std::shared_ptr<RasterLayer> &layer) const
{
  return std::find(mRasterLayers.begin(), mRasterLayers.end(), layer) != mRasterLayers.end();
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
  invalidateFrameCache();
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
  for (const std::shared_ptr<RasterLayer> &layer : mRasterLayers)
  {
    if (!layer || !layer->isVisible())
      continue;
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

void MapCanvas::zoomToLayer(const std::shared_ptr<RasterLayer> &layer)
{
  if (!layer)
    return;

  const Rectangle layerExtent = layer->extentInEpsg(mDestinationEpsg);
  if (!layerExtent.isValid())
  {
    emit statusMessage(QStringLiteral("La capa «%1» no informa de su extension.").arg(layer->name()), kStatusTimeoutMs);
    return;
  }

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
QImage MapCanvas::renderToImage(const QSize &size, bool draft)
{
  QElapsedTimer timer;
  timer.start();
  mLastReuseMs = 0;

  Rectangle extent = mExtent;
  if (!extent.isValid())
    extent = adjustedExtent(layersExtent());

  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  image.fill(QColor(248, 248, 244));

  if (size.isEmpty() || !extent.isValid())
  {
    mLastRenderMs = timer.elapsed();
    return image;
  }

  // La extension se ajusta a la rejilla de pixeles: asi el desplazamiento entre
  // cuadros es entero y el cuadro anterior se puede copiar tal cual. El ajuste
  // es de menos de un pixel (invisible) y ademas evita que los simbolos
  // "vibren" al arrastrar.
  if (extent == mExtent)
  {
    const Rectangle snapped = snappedExtent(extent);
    if (snapped.isValid() && snapped != extent)
    {
      extent = snapped;
      mExtent = snapped;
    }
  }

  const MapToPixel transform(extent, size);
  if (!transform.isValid())
  {
    mLastRenderMs = timer.elapsed();
    return image;
  }

  // Reutilizacion del cuadro anterior: si solo se ha desplazado el mapa a la
  // misma escala, se copia y solo se vuelve a dibujar la franja descubierta.
  QRegion exposedRegion(QRect(QPoint(0, 0), size));
  const bool reused = reusePreviousFrame(image, size, draft, exposedRegion);
  if (reused && exposedRegion.isEmpty())
  {
    // Cuadro reutilizado entero: la geometria ya esta, pero las etiquetas hay que
    // dibujarlas igualmente (la cache no las guarda).
    mLastReuseMs = timer.elapsed();
    drawLabels(image, transform, size, draft);
    mLastRenderMs = timer.elapsed();
    return image;
  }

  mRenderer.setLayers(mLayers);
  mRenderer.setDestinationEpsg(mDestinationEpsg);
  mRenderer.setMaximumFeaturesPerLayer(mMaximumFeatures);

  FastMapRenderer::Options options = mFastOptions;
  options.draft = draft;

  RasterRenderer::Options rasterOptions;
  rasterOptions.draft = draft;

  QPainter painter(&image);
  if (reused)
  {
    // Cuando lo descubierto es un solo rectangulo (arrastre horizontal o vertical
    // puro, que es el caso normal) se recorta con un rectangulo en lugar de con
    // una region: Qt trata el recorte rectangular como un simple recorte de
    // exploracion y el suavizado de los simbolos que caen dentro sale identico al
    // del dibujado completo. Con region, algunos bordes suavizados cambian en un
    // punado de pixeles.
    if (exposedRegion.rectCount() == 1)
      painter.setClipRect(exposedRegion.boundingRect());
    else
      painter.setClipRegion(exposedRegion);
  }

  // Raster debajo del vectorial y con el mismo recorte que la franja a repintar.
  mRasterRenderer.paint(painter, mRasterLayers, extent, size, rasterOptions, mDestinationEpsg);

  // Las capas se recorren en orden de apilado. Las que tienen almacen se agrupan
  // para dibujarlas de una vez; cuando aparece una capa sin almacen (todavia
  // cargando) se vuelca el grupo y se dibuja esa capa con el camino clasico.
  std::vector<FastMapRenderer::Layer> fastGroup;
  const auto flushFastGroup = [&]() {
    if (fastGroup.empty())
      return;
    mFastRenderer.paint(painter, fastGroup, extent, size, options);
    fastGroup.clear();
  };

  for (const std::shared_ptr<VectorLayer> &layer : mLayers)
  {
    if (!layer || !layer->isVisible())
      continue;

    const std::shared_ptr<GeometryStore> store = layer->geometryStore();
    const bool storeUsable = (mBackend != RendererBackend::Legacy) && store
                             && (store->destinationEpsg() == mDestinationEpsg);

    if (storeUsable)
    {
      fastGroup.push_back(FastMapRenderer::Layer {store.get(), &layer->style(), true});
      continue;
    }

    flushFastGroup();
    painter.setRenderHint(QPainter::Antialiasing, true);
    mRenderer.paintLayerInto(painter, transform, *layer);
  }

  flushFastGroup();
  painter.end();

  if (reused)
    mLastReuseMs = timer.elapsed();

  // El cuadro pasa a ser la referencia para el siguiente desplazamiento. Se guarda
  // si el dibujado dependio del encuadre exacto: en ese caso no se podra reutilizar
  // (y el limite de elementos del camino clasico tambien lo hace depender, porque
  // trunca el dibujo segun lo que haya en la vista).
  const bool frameViewDependent =
    mFastRenderer.lastStats().viewDependent || (mMaximumFeatures > 0);

  FrameCache &cache = mFrameCaches[draft ? 1 : 0];
  cache.image = image; // comparte buffer: la cache se queda con la geometria limpia
  cache.extent = extent;
  cache.size = size;
  cache.epsg = mDestinationEpsg;
  cache.revision = contentRevision();
  cache.viewDependent = frameViewDependent;
  cache.valid = true;

  // Las etiquetas se dibujan despues de la geometria y sobre todo el lienzo.
  drawLabels(image, transform, size, draft);
  mLastRenderMs = timer.elapsed();
  return image;
}

void MapCanvas::paintEvent(QPaintEvent *event)
{
  Q_UNUSED(event)

  QPainter painter(this);

  if (mDraftMode && width() > 16 && height() > 16)
  {
    // Cuadro de borrador: se renderiza a media resolucion y se estira a la
    // ventana. Son cuatro veces menos pixeles que rellenar, y el ojo apenas lo
    // nota mientras el mapa se mueve.
    const QSize half(std::max(1, width() / 2), std::max(1, height() / 2));
    const QImage frame = renderToImage(half, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(rect(), frame);
  }
  else
  {
    painter.drawImage(0, 0, renderToImage(size(), false));
  }

  // Solo se pinta la ayuda cuando no hay ninguna capa (tampoco rasters): con un
  // raster cargado el lienzo ya tiene contenido.
  if (mLayers.empty() && mRasterLayers.empty())
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
  invalidateFrameCache();
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
  beginInteraction();
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

    // Se dibuja en modo borrador mientras dura el arrastre; al soltar, el
    // temporizador de calidad pide el cuadro definitivo.
    beginInteraction();

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
