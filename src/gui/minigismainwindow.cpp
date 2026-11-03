#include "minigismainwindow.h"

#include "minigisattributetablemodel.h"
#include "minigiserror.h"
#include "minigisgdalinit.h"
#include "minigisgeometrystore.h"
#include "minigislayerkind.h"
#include "minigislayerpanel.h"
#include "minigismapcanvas.h"
#include "minigislabeldialog.h"
#include "minigisqgisproject.h"
#include "minigisrasterlayer.h"
#include "minigisrasterstyledialog.h"
#include "minigisstyledialog.h"
#include "minigisvectorlayer.h"
#include "minigisvectorloader.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QEventLoop>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>

namespace minigis
{
namespace
{

constexpr int kMaximumRecentFiles = 8;
constexpr int kStatusTimeoutMs = 5000;

/**
 * Radio de busqueda de Identificar, en pixeles de pantalla. Se convierte con la
 * escala del lienzo, asi que "cerca" significa lo mismo a cualquier zoom.
 */
constexpr int kIdentifyRadiusPixels = 6;

/** Formatos que ofrece el dialogo de apertura: vectoriales y raster. */
const char *kOpenFilter =
  "Capas (*.shp *.geojson *.json *.gpkg *.kml *.gml *.gpx *.csv *.tab *.mif *.fgb *.sqlite *.db "
  "*.tif *.tiff *.img *.vrt *.jp2 *.png *.jpg *.jpeg *.bmp);;"
  "Vectoriales (*.shp *.geojson *.json *.gpkg *.kml *.gml *.gpx *.csv *.tab *.mif *.fgb *.sqlite *.db);;"
  "Raster (*.tif *.tiff *.img *.vrt *.jp2 *.png *.jpg *.jpeg *.bmp);;"
  "Shapefile (*.shp);;GeoTIFF (*.tif *.tiff);;Todos los archivos (*)";

/** Proyectos de QGIS: el .qgz comprimido es el habitual. */
const char *kProjectFilter =
  "Proyectos de QGIS (*.qgz *.qgs);;Proyecto comprimido (*.qgz);;Proyecto XML (*.qgs);;Todos los archivos (*)";

QString formatNumber(double value, int precision = 6)
{
  return QString::number(value, 'f', precision);
}

} // namespace

MainWindow::MainWindow(QWidget *parent, bool restoreSettings)
  : QMainWindow(parent)
  , mRestoreSettings(restoreSettings)
{
  setWindowTitle(QStringLiteral("MiniGIS"));
  resize(1150, 760);

  mCanvas = new MapCanvas(this);
  setCentralWidget(mCanvas);

  createActions();
  createMenusAndToolBar();
  createDocks();
  createStatusBar();

  connect(mCanvas, &MapCanvas::layersChanged, this, &MainWindow::onLayersChanged);
  connect(mCanvas, &MapCanvas::currentLayerChanged, this, &MainWindow::onCurrentLayerChanged);
  connect(mCanvas, &MapCanvas::cursorMoved, this, &MainWindow::onCursorMoved);
  connect(mCanvas, &MapCanvas::cursorLeft, this, &MainWindow::onCursorLeft);
  connect(mCanvas, &MapCanvas::scaleChanged, this, &MainWindow::onScaleChanged);
  connect(mCanvas, &MapCanvas::filesDropped, this, [this](const QStringList &paths) { openDroppedPaths(paths); });
  connect(mCanvas, &MapCanvas::statusMessage, this, &MainWindow::onStatusMessage);
  connect(mCanvas, &MapCanvas::identifyRequested, this, &MainWindow::onIdentifyRequested);
  connect(mCanvas, &MapCanvas::extentChanged, this, [this](const Rectangle &) {
    if (mProject && !mProjectLoadActive)
      updateWindowTitle();
  });

  // Carga en segundo plano: abrir y leer una capa pesada ya no bloquea la
  // ventana, y el progreso se puede mostrar mientras tanto.
  mLoader = new VectorLayerLoader(this);
  connect(mLoader, &VectorLayerLoader::progressChanged, this, &MainWindow::onLoadProgress);
  connect(mLoader, &VectorLayerLoader::layerReady, this, &MainWindow::onLayerLoaded);
  connect(mLoader, &VectorLayerLoader::layerFailed, this, &MainWindow::onLayerLoadFailed);
  connect(mLoader, &VectorLayerLoader::requestReady, this, &MainWindow::onProjectLayerReady);
  connect(mLoader, &VectorLayerLoader::requestFailed, this, &MainWindow::onProjectLayerFailed);
  connect(mLoader, &VectorLayerLoader::finished, this, &MainWindow::onLoadFinished);

  readSettings();
  updateRecentFilesMenu();
  updateWindowTitle();
  updateCrsLabel();

  statusBar()->showMessage(QStringLiteral("Listo. Abre un shapefile con Ctrl+O o arrastralo sobre el mapa."),
                           kStatusTimeoutMs);
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions()
{
  mOpenAction = new QAction(QStringLiteral("&Abrir capa..."), this);
  mOpenAction->setShortcut(QKeySequence::Open);
  mOpenAction->setStatusTip(QStringLiteral("Abrir una capa vectorial (shapefile, GeoJSON, GeoPackage...)"));
  connect(mOpenAction, &QAction::triggered, this, &MainWindow::openLayerDialog);

  mOpenProjectAction = new QAction(QStringLiteral("Abrir &proyecto..."), this);
  mOpenProjectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
  mOpenProjectAction->setStatusTip(QStringLiteral("Abrir un proyecto de QGIS (.qgz o .qgs)"));
  connect(mOpenProjectAction, &QAction::triggered, this, &MainWindow::openProjectDialog);

  mSaveProjectAction = new QAction(QStringLiteral("&Guardar proyecto"), this);
  mSaveProjectAction->setShortcut(QKeySequence::Save);
  mSaveProjectAction->setStatusTip(QStringLiteral("Guardar los cambios en el proyecto abierto"));
  connect(mSaveProjectAction, &QAction::triggered, this, &MainWindow::saveProject);
  mSaveProjectAction->setEnabled(false);

  mSaveProjectAsAction = new QAction(QStringLiteral("Guardar proyecto &como..."), this);
  mSaveProjectAsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
  connect(mSaveProjectAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);
  mSaveProjectAsAction->setEnabled(false);

  mSampleAction = new QAction(QStringLiteral("Cargar &ejemplo"), this);
  mSampleAction->setStatusTip(QStringLiteral("Carga los shapefiles de ejemplo incluidos en data/sample"));
  connect(mSampleAction, &QAction::triggered, this, &MainWindow::loadSampleData);

  mCloseAction = new QAction(QStringLiteral("&Cerrar capa"), this);
  mCloseAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
  connect(mCloseAction, &QAction::triggered, this, &MainWindow::closeCurrentLayer);

  mCloseAllAction = new QAction(QStringLiteral("Cerrar &todas"), this);
  mCloseAllAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
  connect(mCloseAllAction, &QAction::triggered, this, &MainWindow::closeAllLayers);

  mQuitAction = new QAction(QStringLiteral("&Salir"), this);
  mQuitAction->setShortcut(QKeySequence::Quit);
  connect(mQuitAction, &QAction::triggered, this, &QWidget::close);

  mZoomInAction = new QAction(QStringLiteral("Acercar"), this);
  mZoomInAction->setShortcut(QKeySequence::ZoomIn);
  connect(mZoomInAction, &QAction::triggered, mCanvas, &MapCanvas::zoomIn);

  mZoomOutAction = new QAction(QStringLiteral("Alejar"), this);
  mZoomOutAction->setShortcut(QKeySequence::ZoomOut);
  connect(mZoomOutAction, &QAction::triggered, mCanvas, &MapCanvas::zoomOut);

  mZoomFullAction = new QAction(QStringLiteral("Extension completa"), this);
  mZoomFullAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
  connect(mZoomFullAction, &QAction::triggered, mCanvas, &MapCanvas::zoomToFullExtent);

  mZoomLayerAction = new QAction(QStringLiteral("Zoom a la capa"), this);
  mZoomLayerAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
  connect(mZoomLayerAction, &QAction::triggered, this, &MainWindow::zoomToCurrentLayer);

  mStyleAction = new QAction(QStringLiteral("&Simbologia..."), this);
  mStyleAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Y")));
  connect(mStyleAction, &QAction::triggered, this, &MainWindow::editCurrentLayerStyle);

  mLabelAction = new QAction(QStringLiteral("&Etiquetas de la capa..."), this);
  mLabelAction->setShortcut(QKeySequence(Qt::Key_F7));
  mLabelAction->setToolTip(QStringLiteral("Campos, fuente, color, halo y escalas de las etiquetas"));
  connect(mLabelAction, &QAction::triggered, this, &MainWindow::editCurrentLayerLabels);

  mShowLabelsAction = new QAction(QStringLiteral("Mostrar &etiquetas"), this);
  mShowLabelsAction->setCheckable(true);
  mShowLabelsAction->setChecked(true);
  mShowLabelsAction->setToolTip(QStringLiteral("Apaga o enciende las etiquetas de todas las capas"));
  connect(mShowLabelsAction, &QAction::toggled, this, [this](bool visible) {
    mCanvas->setLabelsVisible(visible);
    statusBar()->showMessage(visible ? QStringLiteral("Etiquetas activadas.")
                                     : QStringLiteral("Etiquetas desactivadas."),
                             kStatusTimeoutMs);
  });

  mAttributeAction = new QAction(QStringLiteral("Tabla de atributos"), this);
  mAttributeAction->setShortcut(QKeySequence(QStringLiteral("F6")));
  mAttributeAction->setCheckable(true);
  connect(mAttributeAction, &QAction::toggled, this, &MainWindow::showAttributeTable);

  mIdentifyAction = new QAction(QStringLiteral("Identificar"), this);
  mIdentifyAction->setCheckable(true);
  mIdentifyAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+I")));
  mIdentifyAction->setStatusTip(QStringLiteral("Muestra la información del elemento bajo el clic"));
  connect(mIdentifyAction, &QAction::toggled, this, &MainWindow::showIdentifyPanel);

  mAboutAction = new QAction(QStringLiteral("Acerca de MiniGIS"), this);
  connect(mAboutAction, &QAction::triggered, this, &MainWindow::showAboutDialog);

  mSystemInfoAction = new QAction(QStringLiteral("Informacion del sistema"), this);
  mSystemInfoAction->setStatusTip(QStringLiteral("Versiones de Qt, GDAL, PROJ y rutas de datos resueltas"));
  connect(mSystemInfoAction, &QAction::triggered, this, &MainWindow::showSystemInfo);
}

void MainWindow::createMenusAndToolBar()
{
  QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&Archivo"));
  fileMenu->addAction(mOpenAction);
  fileMenu->addAction(mOpenProjectAction);
  mRecentMenu = fileMenu->addMenu(QStringLiteral("Abrir recientes"));
  fileMenu->addAction(mSampleAction);
  fileMenu->addSeparator();
  fileMenu->addAction(mSaveProjectAction);
  fileMenu->addAction(mSaveProjectAsAction);
  fileMenu->addSeparator();
  fileMenu->addAction(mCloseAction);
  fileMenu->addAction(mCloseAllAction);
  fileMenu->addSeparator();
  fileMenu->addAction(mQuitAction);

  QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&Ver"));
  viewMenu->addAction(mZoomInAction);
  viewMenu->addAction(mZoomOutAction);
  viewMenu->addAction(mZoomFullAction);
  viewMenu->addAction(mZoomLayerAction);
  viewMenu->addSeparator();
  viewMenu->addAction(mStyleAction);
  viewMenu->addAction(mLabelAction);
  viewMenu->addSeparator();
  viewMenu->addAction(mShowLabelsAction);
  viewMenu->addAction(mAttributeAction);
  viewMenu->addAction(mIdentifyAction);

  QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("A&yuda"));
  helpMenu->addAction(mSystemInfoAction);
  helpMenu->addAction(mAboutAction);

  QToolBar *toolBar = addToolBar(QStringLiteral("Principal"));
  toolBar->setObjectName(QStringLiteral("mainToolBar"));
  toolBar->setMovable(false);
  toolBar->addAction(mOpenAction);
  toolBar->addAction(mOpenProjectAction);
  toolBar->addAction(mSampleAction);
  toolBar->addSeparator();
  toolBar->addAction(mZoomOutAction);
  toolBar->addAction(mZoomInAction);
  toolBar->addAction(mZoomFullAction);
  toolBar->addAction(mZoomLayerAction);
  toolBar->addSeparator();
  toolBar->addAction(mStyleAction);
  toolBar->addAction(mLabelAction);
  toolBar->addAction(mAttributeAction);
}

void MainWindow::createDocks()
{
  mLayerDock = new QDockWidget(QStringLiteral("Capas"), this);
  mLayerDock->setObjectName(QStringLiteral("layerDock"));
  mLayerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

  mLayerPanel = new LayerPanel(mLayerDock);
  mLayerDock->setWidget(mLayerPanel);
  addDockWidget(Qt::LeftDockWidgetArea, mLayerDock);

  connect(mLayerPanel, &LayerPanel::addLayerRequested, this, &MainWindow::openLayerDialog);
  connect(mLayerPanel, &LayerPanel::currentLayerChanged, this, &MainWindow::onLayerPanelCurrentChanged);
  connect(mLayerPanel, &LayerPanel::visibilityChanged, this, &MainWindow::onLayerVisibilityChanged);
  connect(mLayerPanel, &LayerPanel::removeLayerRequested, this, [this](const LayerEntry &entry) {
    if (entry.kind == LayerEntry::Kind::Raster && entry.raster)
    {
      if (mCurrentRaster == entry.raster)
        mCurrentRaster.reset();
      mCanvas->removeRasterLayer(entry.raster);
    }
    else if (entry.vector)
    {
      mCanvas->removeLayer(entry.vector);
    }
  });
  // Zoom por tipo: `MapCanvas::zoomToLayer` esta sobrecargado y la eleccion
  // depende de la clase de la entrada seleccionada.
  connect(mLayerPanel, &LayerPanel::zoomToLayerRequested, this, [this](const LayerEntry &entry) {
    if (entry.kind == LayerEntry::Kind::Raster && entry.raster)
    {
      mCurrentRaster = entry.raster;
      mCanvas->setCurrentLayer(nullptr);
      mCanvas->zoomToLayer(entry.raster);
    }
    else if (entry.vector)
    {
      mCurrentRaster.reset();
      mCanvas->zoomToLayer(entry.vector);
    }
  });
  connect(mLayerPanel, &LayerPanel::styleRequested, this, [this](const LayerEntry &entry) {
    if (entry.kind == LayerEntry::Kind::Raster && entry.raster)
    {
      mCurrentRaster = entry.raster;
      mCanvas->setCurrentLayer(nullptr);
      editCurrentLayerStyle();
    }
    else if (entry.vector)
    {
      mCurrentRaster.reset();
      mCanvas->setCurrentLayer(entry.vector);
      editCurrentLayerStyle();
    }
  });
  connect(mLayerPanel, &LayerPanel::labelRequested, this, [this](const LayerEntry &entry) {
    if (entry.vector)
    {
      mCurrentRaster.reset();
      mCanvas->setCurrentLayer(entry.vector);
      editCurrentLayerLabels();
    }
  });
  connect(mLayerPanel, &LayerPanel::attributeTableRequested, this, [this](const LayerEntry &entry) {
    if (entry.vector)
    {
      mCurrentRaster.reset();
      mCanvas->setCurrentLayer(entry.vector);
      showAttributeTable(true);
    }
  });

  mAttributeDock = new QDockWidget(QStringLiteral("Tabla de atributos"), this);
  mAttributeDock->setObjectName(QStringLiteral("attributeDock"));

  mAttributeModel = new AttributeTableModel(this);
  mAttributeView = new QTableView(mAttributeDock);
  mAttributeView->setModel(mAttributeModel);
  mAttributeView->setAlternatingRowColors(true);
  mAttributeView->setSelectionBehavior(QAbstractItemView::SelectRows);
  mAttributeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
  mAttributeView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  mAttributeView->horizontalHeader()->setStretchLastSection(true);
  mAttributeView->verticalHeader()->setDefaultSectionSize(22);

  mAttributeDock->setWidget(mAttributeView);
  addDockWidget(Qt::BottomDockWidgetArea, mAttributeDock);

  // El panel de atributos empieza oculto: se abre con F6 o desde el panel de capas.
  mAttributeDock->hide();
  connect(mAttributeDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
    if (mAttributeAction->isChecked() != visible)
      mAttributeAction->setChecked(visible);
  });

  mIdentifyDock = new QDockWidget(QStringLiteral("Identificar"), this);
  mIdentifyDock->setObjectName(QStringLiteral("identifyDock"));
  mIdentifyDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

  mIdentifyPanel = new IdentifyPanel(mIdentifyDock);
  mIdentifyDock->setWidget(mIdentifyPanel);
  addDockWidget(Qt::RightDockWidgetArea, mIdentifyDock);

  // Empieza oculto: aparece con el primer clic, con el menu Ver o con
  // Ctrl+Shift+I. Cerrarlo no borra el ultimo resultado.
  mIdentifyDock->hide();
  connect(mIdentifyDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
    if (mIdentifyAction && mIdentifyAction->isChecked() != visible)
      mIdentifyAction->setChecked(visible);
  });
  connect(mIdentifyPanel, &IdentifyPanel::searchAllLayersChanged, this,
          &MainWindow::onSearchAllLayersChanged);
}

void MainWindow::createStatusBar()
{
  mCoordinateLabel = new QLabel(QStringLiteral("x: —   y: —"), this);
  mCoordinateLabel->setMinimumWidth(230);
  mCoordinateLabel->setToolTip(QStringLiteral("Coordenadas del cursor en el SRC de destino del mapa"));

  mScaleLabel = new QLabel(QStringLiteral("1 px = —"), this);
  mScaleLabel->setMinimumWidth(140);
  mScaleLabel->setToolTip(QStringLiteral("Unidades de mapa por pixel (escala actual)"));

  mCrsLabel = new QLabel(QStringLiteral("SRC: —"), this);
  mCrsLabel->setMinimumWidth(200);
  mCrsLabel->setToolTip(QStringLiteral("Sistema de referencia de la capa activa"));

  mRenderLabel = new QLabel(QStringLiteral("render: —"), this);
  mRenderLabel->setMinimumWidth(110);
  mRenderLabel->setToolTip(QStringLiteral("Tiempo del ultimo renderizado completo"));

  // Indicador de carga: barra de progreso porcentual y boton para abortar. Ambos
  // viven en la barra de estado y solo aparecen mientras hay una carga en curso.
  mLoadLabel = new QLabel(this);
  mLoadLabel->setMinimumWidth(260);

  mLoadProgress = new QProgressBar(this);
  mLoadProgress->setRange(0, 100);
  mLoadProgress->setValue(0);
  mLoadProgress->setFixedWidth(160);
  mLoadProgress->setTextVisible(true);
  mLoadProgress->setToolTip(QStringLiteral("Progreso de la lectura y del indice de dibujo"));

  mCancelLoadButton = new QToolButton(this);
  mCancelLoadButton->setText(QStringLiteral("Cancelar"));
  mCancelLoadButton->setToolTip(QStringLiteral("Interrumpe la carga en curso"));
  connect(mCancelLoadButton, &QToolButton::clicked, this, [this]() {
    if (mLoader)
      mLoader->cancel();
  });

  statusBar()->addWidget(mLoadLabel);
  statusBar()->addWidget(mLoadProgress);
  statusBar()->addWidget(mCancelLoadButton);
  statusBar()->addPermanentWidget(mRenderLabel);
  statusBar()->addPermanentWidget(mCrsLabel);
  statusBar()->addPermanentWidget(mScaleLabel);
  statusBar()->addPermanentWidget(mCoordinateLabel);

  setLoadIndicatorVisible(false);
}

void MainWindow::setLoadIndicatorVisible(bool visible)
{
  mLoadLabel->setVisible(visible);
  mLoadProgress->setVisible(visible);
  mCancelLoadButton->setVisible(visible);
}

bool MainWindow::isLoading() const
{
  return mLoader && mLoader->isRunning();
}

bool MainWindow::waitForLoading(int timeoutMs)
{
  if (!isLoading())
    return true;

  // Bucle de eventos anidado: la carga sigue en su hilo y sus senales se
  // entregan aqui, de modo que la incorporacion de capas al lienzo se hace
  // igual que en la aplicacion interactiva.
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);

  const QMetaObject::Connection finishedConnection =
    connect(mLoader, &VectorLayerLoader::finished, &loop, &QEventLoop::quit);
  connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(timeoutMs);
  loop.exec();
  disconnect(finishedConnection);

  return !isLoading();
}

void MainWindow::onLoadProgress(int percent, const QString &message)
{
  setLoadIndicatorVisible(true);
  mLoadProgress->setValue(std::clamp(percent, 0, 100));
  mLoadLabel->setText(message);
  mLoadLabel->setToolTip(message);
  updateWindowTitle();
}

void MainWindow::onLayerLoaded(const std::shared_ptr<VectorLayer> &layer, bool first)
{
  if (!layer)
    return;

  // El lienzo encuadra la primera capa de la tanda (o la primera de todas).
  const bool zoomToExtent = first || mCanvas->layers().empty();
  mCurrentRaster.reset();
  mCanvas->addLayer(layer, zoomToExtent);
  mLastDirectory = QFileInfo(layer->sourcePath()).absolutePath();

  // Un archivo con varias capas emite una senal por capa: se anota una sola vez
  // en los recientes para no reescribir la configuracion N veces.
  if (mRecentFiles.isEmpty() || mRecentFiles.first() != QFileInfo(layer->sourcePath()).absoluteFilePath())
    addRecentFile(layer->sourcePath());
  updateCrsLabel();

  // Si la capa no cabia en memoria se ha indexado por envolventes: se avisa,
  // porque la geometria se lee al acercarse y conviene que el usuario lo sepa.
  QString note;
  if (const std::shared_ptr<GeometryStore> store = layer->geometryStore())
  {
    if (store->mode() == GeometryStore::Mode::Envelopes)
    {
      note = QStringLiteral(" Capa indexada por envolventes (%1 elementos): el detalle se lee al acercarse.")
               .arg(store->stats().features);
    }
  }

  statusBar()->showMessage(QStringLiteral("Capa «%1» cargada: %2 elementos (%3).%4")
                             .arg(layer->name())
                             .arg(layer->featureCount())
                             .arg(layer->crsDescription(), note),
                           kStatusTimeoutMs);
}

void MainWindow::onLayerLoadFailed(const QString &path, const QString &message, const QString &hint)
{
  Error error {message, hint};
  showError(QStringLiteral("No se pudo abrir «%1»").arg(QFileInfo(path).fileName()), error);
}

void MainWindow::onLoadFinished(int loaded, int failed, bool cancelled)
{
  if (mProjectLoadActive)
  {
    finishProjectLoad();
    return;
  }

  setLoadIndicatorVisible(false);

  // Ahora que se sabe todo lo que aportaba la tanda se encuadra su union: si se
  // hubiera encuadrado solo la primera capa, las demas (otras capas del mismo
  // GeoPackage o los demas archivos) quedarian fuera de la vista.
  frameLoadedBatch();
  updateWindowTitle();

  QString summary;
  if (cancelled)
    summary = QStringLiteral("Carga cancelada. Se han cargado %1 capa(s).").arg(loaded);
  else if (failed > 0)
    summary = QStringLiteral("Carga terminada: %1 capa(s) cargada(s), %2 con error.").arg(loaded).arg(failed);
  else
    summary = QStringLiteral("Carga terminada: %1 capa(s).").arg(loaded);

  statusBar()->showMessage(summary, kStatusTimeoutMs);
}

void MainWindow::frameLoadedBatch()
{
  if (!mLoadBatchPending)
    return;
  mLoadBatchPending = false;

  Rectangle extent;
  const std::vector<std::shared_ptr<VectorLayer>> &layers = mCanvas->layers();
  for (size_t index = std::min(mLoadStartVectorLayers, layers.size()); index < layers.size(); ++index)
  {
    if (layers[index])
      extent.combine(layers[index]->extentInEpsg(mCanvas->destinationEpsg()));
  }

  const std::vector<std::shared_ptr<RasterLayer>> &rasters = mCanvas->rasterLayers();
  for (size_t index = std::min(mLoadStartRasterLayers, rasters.size()); index < rasters.size(); ++index)
  {
    if (rasters[index])
      extent.combine(rasters[index]->extentInEpsg(mCanvas->destinationEpsg()));
  }

  if (extent.isValid())
    mCanvas->zoomToExtent(extent);
}

std::shared_ptr<VectorLayer> MainWindow::currentLayer() const
{
  return mCanvas ? mCanvas->currentLayer() : nullptr;
}

bool MainWindow::openLayer(const QString &path, bool zoomToExtent)
{
  if (path.isEmpty())
    return false;

  Error error;
  const LayerKind kind = detectLayerKind(path, &error);

  if (kind == LayerKind::Raster)
    return openRasterLayer(path, zoomToExtent);

  if (kind == LayerKind::Unknown)
  {
    showError(QStringLiteral("No se pudo abrir la capa"), error);
    return false;
  }

  // Un archivo puede contener varias capas (un GeoPackage): esta apertura
  // sincrona tambien las anade todas, para no dejar un camino que reproduczca el
  // fallo de cargar solo la primera.
  const QStringList names = VectorLayer::layerNames(path, &error);
  if (names.isEmpty())
  {
    showError(QStringLiteral("No se pudo abrir la capa"), error);
    return false;
  }

  const size_t firstLayerIndex = mCanvas->layers().size();
  int added = 0;
  std::shared_ptr<VectorLayer> onlyLayer;
  for (int index = 0; index < names.size(); ++index)
  {
    Error layerError;
    const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, index, &layerError);
    if (!layer)
    {
      showError(QStringLiteral("No se pudo abrir la capa «%1»").arg(names.at(index)), layerError);
      continue;
    }

    if (names.size() == 1)
      onlyLayer = layer;

    mCurrentRaster.reset();
    mLastDirectory = QFileInfo(path).absolutePath();
    mCanvas->addLayer(layer, zoomToExtent && added == 0);
    ++added;
  }

  if (added == 0)
    return false;

  // Encuadrar la union de las capas anadidas: si el archivo tiene varias capas,
  // encuadrar solo la primera dejaria las demas fuera de la vista.
  if (zoomToExtent)
  {
    Rectangle extent;
    for (size_t index = std::min(firstLayerIndex, mCanvas->layers().size());
         index < mCanvas->layers().size(); ++index)
    {
      if (mCanvas->layers()[index])
        extent.combine(mCanvas->layers()[index]->extentInEpsg(mCanvas->destinationEpsg()));
    }
    if (extent.isValid())
      mCanvas->zoomToExtent(extent);
  }

  addRecentFile(path);

  if (onlyLayer)
  {
    statusBar()->showMessage(QStringLiteral("Capa «%1» cargada: %2 elementos (%3)")
                               .arg(onlyLayer->name())
                               .arg(onlyLayer->featureCount())
                               .arg(onlyLayer->crsDescription()),
                             kStatusTimeoutMs);
  }
  else
  {
    statusBar()->showMessage(QStringLiteral("%1 capas cargadas de «%2».")
                               .arg(added)
                               .arg(QFileInfo(path).fileName()),
                             kStatusTimeoutMs);
  }
  return true;
}

bool MainWindow::openRasterLayer(const QString &path, bool zoomToExtent)
{
  Error error;
  const std::shared_ptr<RasterLayer> raster = RasterLayer::open(path, &error);
  if (!raster)
  {
    showError(QStringLiteral("No se pudo abrir el raster"), error);
    return false;
  }

  const bool firstLayer = mCanvas->layers().empty() && mCanvas->rasterLayers().empty();

  mLastDirectory = QFileInfo(path).absolutePath();
  mCurrentRaster = raster;
  mCanvas->setCurrentLayer(nullptr);
  mCanvas->addRasterLayer(raster, zoomToExtent || firstLayer);
  addRecentFile(path);

  statusBar()->showMessage(QStringLiteral("Raster «%1» cargado: %2x%3, %4 bandas (%5)")
                             .arg(raster->name())
                             .arg(raster->width())
                             .arg(raster->height())
                             .arg(raster->bandCount())
                             .arg(raster->crsDescription()),
                           kStatusTimeoutMs);
  return true;
}

int MainWindow::loadPaths(const QStringList &paths)
{
  if (paths.isEmpty())
    return 0;

  // La carga vectorial es asincrona (abrir y leer una capa pesada no debe
  // bloquear la ventana); el raster se abre y se dibuja en el hilo de la GUI,
  // que es donde vive su dataset de GDAL.
  if (mLoader->isRunning())
  {
    statusBar()->showMessage(QStringLiteral("Ya hay una carga en curso; espera o pulsa Cancelar."),
                             kStatusTimeoutMs);
    return 0;
  }

  QStringList vectorPaths;
  int queued = 0;

  // Se anota donde empieza la tanda para encuadrar al final la union de todo lo
  // cargado (ver frameLoadedBatch). Los raster se anaden aqui mismo, antes de
  // arrancar el cargador vectorial, y tambien cuentan.
  mLoadStartVectorLayers = mCanvas->layers().size();
  mLoadStartRasterLayers = mCanvas->rasterLayers().size();
  mLoadBatchPending = true;

  for (const QString &path : paths)
  {
    Error error;
    const LayerKind kind = detectLayerKind(path, &error);

    if (kind == LayerKind::Raster)
    {
      if (openRasterLayer(path, mCanvas->layers().empty() && mCanvas->rasterLayers().empty()))
        ++queued;
      continue;
    }

    if (kind == LayerKind::Vector)
    {
      vectorPaths << path;
      continue;
    }

    showError(QStringLiteral("No se pudo abrir «%1»").arg(QFileInfo(path).fileName()), error);
  }

  if (!vectorPaths.isEmpty())
  {
    mLoader->setDestinationEpsg(mCanvas->destinationEpsg());
    setLoadIndicatorVisible(true);
    mLoadProgress->setValue(0);
    mLoadLabel->setText(QStringLiteral("Preparando la carga de %1 archivo(s)...").arg(vectorPaths.size()));
    mLoader->start(vectorPaths);
    queued += vectorPaths.size();
  }
  else
  {
    // Solo habia raster: no pasara por onLoadFinished.
    frameLoadedBatch();
    setLoadIndicatorVisible(false);
    updateWindowTitle();
  }

  return queued;
}

bool MainWindow::openProject(const QString &path)
{
  if (path.isEmpty())
    return false;

  if (mLoader && mLoader->isRunning())
  {
    statusBar()->showMessage(QStringLiteral("Ya hay una carga en curso; espera o pulsa Cancelar."), kStatusTimeoutMs);
    return false;
  }

  if (mProject && !maybeSaveProject())
    return false;

  Error error;
  std::unique_ptr<QgisProject> project;
  if (!QgisProject::load(path, &project, &error))
  {
    showError(QStringLiteral("No se pudo abrir el proyecto"), error);
    return false;
  }

  mProject = std::move(project);
  mProjectPath = path;
  mUnavailableProjectLayers.clear();
  mPendingProjectIds.clear();
  mProjectVectorLayers.clear();
  mProjectRasterLayers.clear();

  // El proyecto sustituye al mapa actual; el estado «modificado» y el aviso de
  // cambios sin guardar llegan con el ciclo de vida del proyecto.
  mCurrentRaster.reset();
  mCanvas->setCurrentLayer(nullptr);
  mCanvas->clearLayers();
  mCanvas->clearRasterLayers();
  mCanvas->setDestinationEpsg(mProject->canvasEpsg());

  for (const UnavailableProjectLayer &unavailable : mProject->unavailableLayers())
    mUnavailableProjectLayers << QStringLiteral("%1: %2").arg(unavailable.name, unavailable.reason);

  QVector<LayerRequest> requests;
  for (const ProjectLayer &layer : mProject->layers())
  {
    if (layer.type == ProjectLayer::Type::Vector)
    {
      LayerRequest request;
      request.path = layer.sourcePath;
      request.layerName = layer.layerName;
      requests.append(request);
      mPendingProjectIds << layer.id;
    }
    else if (layer.type == ProjectLayer::Type::Raster)
    {
      Error rasterError;
      const std::shared_ptr<RasterLayer> raster = RasterLayer::open(layer.sourcePath, &rasterError);
      if (!raster)
      {
        mUnavailableProjectLayers << QStringLiteral("%1: %2").arg(layer.name, rasterError.message);
        continue;
      }

      raster->setStyle(layer.raster);
      raster->setVisible(layer.visible);
      raster->setDisplayName(layer.name);
      mCanvas->addRasterLayer(raster, false, true);
      mProjectRasterLayers.insert(layer.id, raster);
    }
  }

  mLastDirectory = QFileInfo(path).absolutePath();
  addRecentFile(path);
  updateWindowTitle();

  if (!requests.isEmpty())
  {
    mProjectLoadActive = true;
    mLoader->setDestinationEpsg(mProject->canvasEpsg());
    setLoadIndicatorVisible(true);
    mLoadProgress->setValue(0);
    mLoadLabel->setText(QStringLiteral("Abriendo el proyecto «%1»...").arg(QFileInfo(path).fileName()));
    mLoader->startRequests(requests);
  }
  else
  {
    finishProjectLoad();
  }

  return true;
}

void MainWindow::openProjectDialog()
{
  const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Abrir proyecto de QGIS"), mLastDirectory,
                                                    QString::fromUtf8(kProjectFilter));
  if (!file.isEmpty())
    openProject(file);
}

void MainWindow::saveProject()
{
  if (!mProject)
    return;

  if (mProjectPath.isEmpty())
  {
    saveProjectAs();
    return;
  }

  const ProjectSnapshot snapshot = buildProjectSnapshot();
  Error error;
  if (!mProject->save(mProjectPath, snapshot, &error))
  {
    showError(QStringLiteral("No se pudo guardar el proyecto"), error);
    return;
  }

  mSavedProjectSignature = projectSnapshotSignature(snapshot);
  reloadProjectMetadata();
  addRecentFile(mProjectPath);
  updateWindowTitle();
  statusBar()->showMessage(QStringLiteral("Proyecto guardado en «%1».").arg(QDir::toNativeSeparators(mProjectPath)),
                           kStatusTimeoutMs);
}

void MainWindow::saveProjectAs()
{
  if (!mProject)
    return;

  const QString suggested = mProjectPath.isEmpty() ? QStringLiteral("proyecto.qgz") : mProjectPath;
  QString target = QFileDialog::getSaveFileName(this, QStringLiteral("Guardar proyecto como"), suggested,
                                                QString::fromUtf8(kProjectFilter));
  if (target.isEmpty())
    return;

  if (!QgisProject::isProjectPath(target))
    target += QStringLiteral(".qgz");

  const ProjectSnapshot snapshot = buildProjectSnapshot();
  Error error;
  if (!mProject->save(target, snapshot, &error))
  {
    showError(QStringLiteral("No se pudo guardar el proyecto"), error);
    return;
  }

  mProjectPath = target;
  mSavedProjectSignature = projectSnapshotSignature(snapshot);
  reloadProjectMetadata();
  mLastDirectory = QFileInfo(target).absolutePath();
  addRecentFile(target);
  updateWindowTitle();
  statusBar()->showMessage(QStringLiteral("Proyecto guardado en «%1».").arg(QDir::toNativeSeparators(target)),
                           kStatusTimeoutMs);
}

void MainWindow::openDroppedPaths(const QStringList &paths)
{
  QStringList projects;
  QStringList layerPaths;
  for (const QString &path : paths)
  {
    if (QgisProject::isProjectPath(path))
      projects << path;
    else
      layerPaths << path;
  }

  if (projects.size() > 1)
    statusBar()->showMessage(QStringLiteral("Solo se abre un proyecto a la vez; se usara el primero."),
                             kStatusTimeoutMs);
  if (!projects.isEmpty())
    openProject(projects.first());
  if (!layerPaths.isEmpty())
    loadPaths(layerPaths);
}

void MainWindow::finishProjectLoad()
{
  setLoadIndicatorVisible(false);

  if (mProject)
  {
    if (mProject->hasExtent() && mProject->extentEpsg() == mProject->canvasEpsg())
      mCanvas->setExtent(mProject->extent());
    else
      mCanvas->zoomToFullExtent();
    mCanvas->refreshMap();
  }

  mSavedProjectSignature = projectSnapshotSignature(buildProjectSnapshot());
  mProjectLoadActive = false;

  syncLayerPanel();
  replaceAttributeModel();
  updateWindowTitle();
  updateCrsLabel();

  const int available = static_cast<int>(mCanvas->layers().size() + mCanvas->rasterLayers().size());
  statusBar()->showMessage(QStringLiteral("Proyecto «%1» abierto: %2 capa(s)%3.")
                             .arg(QFileInfo(mProjectPath).fileName())
                             .arg(available)
                             .arg(mUnavailableProjectLayers.isEmpty()
                                    ? QString()
                                    : QStringLiteral(" (%1 no disponible(s))").arg(mUnavailableProjectLayers.size())),
                           kStatusTimeoutMs);

  showUnavailableProjectLayers();
}

const ProjectLayer *MainWindow::projectLayerById(const QString &id) const
{
  if (!mProject)
    return nullptr;
  for (const ProjectLayer &layer : mProject->layers())
  {
    if (layer.id == id)
      return &layer;
  }
  return nullptr;
}

int MainWindow::projectLayerCount() const
{
  return mProject ? static_cast<int>(mProject->layers().size()) : 0;
}

bool MainWindow::hasProject() const
{
  return mProject != nullptr;
}

ProjectSnapshot MainWindow::buildProjectSnapshot() const
{
  ProjectSnapshot snapshot;
  snapshot.crsEpsg = mCanvas->destinationEpsg();
  snapshot.extent = mCanvas->extent();
  snapshot.hasExtent = snapshot.extent.isValid();

  const auto vectorId = [this](const std::shared_ptr<VectorLayer> &layer) -> QString {
    for (auto iterator = mProjectVectorLayers.cbegin(); iterator != mProjectVectorLayers.cend(); ++iterator)
    {
      if (iterator.value() == layer)
        return iterator.key();
    }
    return QString();
  };
  const auto rasterId = [this](const std::shared_ptr<RasterLayer> &layer) -> QString {
    for (auto iterator = mProjectRasterLayers.cbegin(); iterator != mProjectRasterLayers.cend(); ++iterator)
    {
      if (iterator.value() == layer)
        return iterator.key();
    }
    return QString();
  };

  // Rasters primero: MiniGIS los dibuja siempre debajo del vectorial.
  for (const std::shared_ptr<RasterLayer> &raster : mCanvas->rasterLayers())
  {
    if (!raster)
      continue;

    ProjectLayer layer;
    const QString id = rasterId(raster);
    const ProjectLayer *entry = id.isEmpty() ? nullptr : projectLayerById(id);
    if (entry)
      layer = *entry;
    else
    {
      layer.type = ProjectLayer::Type::Raster;
      layer.name = raster->name();
      layer.provider = QStringLiteral("gdal");
      layer.sourcePath = raster->sourcePath();
      layer.crsEpsg = raster->crsEpsg();
      layer.crsWkt = raster->crsWkt();
    }

    layer.type = ProjectLayer::Type::Raster;
    layer.visible = raster->isVisible();
    layer.raster = raster->style();
    snapshot.layers.append(layer);
  }

  for (const std::shared_ptr<VectorLayer> &vector : mCanvas->layers())
  {
    if (!vector)
      continue;

    ProjectLayer layer;
    const QString id = vectorId(vector);
    const ProjectLayer *entry = id.isEmpty() ? nullptr : projectLayerById(id);
    if (entry)
      layer = *entry;
    else
    {
      layer.type = ProjectLayer::Type::Vector;
      layer.name = vector->name();
      layer.provider = QStringLiteral("ogr");
      layer.sourcePath = vector->sourcePath();
      layer.layerName = vector->layerCount() > 1 ? vector->name() : QString();
      layer.crsEpsg = vector->crsEpsg();
      layer.crsWkt = vector->crsWkt();
    }

    layer.type = ProjectLayer::Type::Vector;
    layer.visible = vector->isVisible();
    layer.style = vector->style();
    layer.labels = vector->labelStyle();
    layer.geometryKind = vector->geometryKind();
    layer.geometryName = vector->geometryTypeName();
    snapshot.layers.append(layer);
  }

  return snapshot;
}

bool MainWindow::isProjectModified() const
{
  if (!mProject || mProjectPath.isEmpty() || mProjectLoadActive || mSavedProjectSignature.isEmpty())
    return false;
  return projectSnapshotSignature(buildProjectSnapshot()) != mSavedProjectSignature;
}

void MainWindow::reloadProjectMetadata()
{
  if (!mProject || mProjectPath.isEmpty())
    return;

  Error error;
  std::unique_ptr<QgisProject> reloaded;
  if (!QgisProject::load(mProjectPath, &reloaded, &error))
    return;

  mProject = std::move(reloaded);

  const auto keyFor = [](const QString &path, const QString &name) {
    return path + QLatin1Char('\n') + name;
  };

  QHash<QString, QStringList> idsByKey;
  for (const ProjectLayer &layer : mProject->layers())
    idsByKey[keyFor(layer.sourcePath, layer.name)].append(layer.id);

  mProjectVectorLayers.clear();
  mProjectRasterLayers.clear();

  for (const std::shared_ptr<VectorLayer> &vector : mCanvas->layers())
  {
    if (!vector)
      continue;
    auto iterator = idsByKey.find(keyFor(vector->sourcePath(), vector->name()));
    if (iterator == idsByKey.end() || iterator->isEmpty())
      continue;
    mProjectVectorLayers.insert(iterator->takeFirst(), vector);
  }

  for (const std::shared_ptr<RasterLayer> &raster : mCanvas->rasterLayers())
  {
    if (!raster)
      continue;
    auto iterator = idsByKey.find(keyFor(raster->sourcePath(), raster->name()));
    if (iterator == idsByKey.end() || iterator->isEmpty())
      continue;
    mProjectRasterLayers.insert(iterator->takeFirst(), raster);
  }

  mSavedProjectSignature = projectSnapshotSignature(buildProjectSnapshot());
}

bool MainWindow::maybeSaveProject()
{
  if (!isProjectModified())
    return true;

  // En los modos sin ventana no se abren dialogos modales.
  if (!mRestoreSettings)
    return true;

  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(QStringLiteral("Cambios sin guardar"));
  box.setText(QStringLiteral("El proyecto «%1» tiene cambios sin guardar.").arg(QFileInfo(mProjectPath).fileName()));
  box.setInformativeText(QStringLiteral("¿Quieres guardarlos antes de continuar?"));
  box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  box.setDefaultButton(QMessageBox::Save);

  const int result = box.exec();
  if (result == QMessageBox::Save)
  {
    saveProject();
    return !isProjectModified();
  }
  if (result == QMessageBox::Discard)
    return true;

  return false;
}

void MainWindow::showUnavailableProjectLayers()
{
  if (mUnavailableProjectLayers.isEmpty())
    return;

  if (!mRestoreSettings)
  {
    statusBar()->showMessage(QStringLiteral("El proyecto tiene %1 capa(s) no disponible(s); se conservan en el archivo.")
                               .arg(mUnavailableProjectLayers.size()),
                             kStatusTimeoutMs);
    return;
  }

  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(QStringLiteral("Capas no disponibles"));
  box.setText(QStringLiteral("El proyecto declara %1 capa(s) que no se han podido abrir. "
                             "Se conservan en el archivo al guardar.")
                .arg(mUnavailableProjectLayers.size()));
  box.setDetailedText(mUnavailableProjectLayers.join(QLatin1Char('\n')));
  box.setStandardButtons(QMessageBox::Ok);
  box.exec();
}

void MainWindow::onProjectLayerReady(const std::shared_ptr<VectorLayer> &layer, int requestId)
{
  if (!mProjectLoadActive || requestId < 0 || requestId >= mPendingProjectIds.size() || !layer)
    return;

  const ProjectLayer *entry = projectLayerById(mPendingProjectIds.at(requestId));
  layer->setStyle(entry ? entry->style : layer->style());
  if (entry)
  {
    layer->setDisplayName(entry->name);
    layer->setLabelStyle(entry->labels);
    layer->setVisible(entry->visible);
    mProjectVectorLayers.insert(entry->id, layer);
  }

  mCanvas->addLayer(layer, false, true);
}

void MainWindow::onProjectLayerFailed(int requestId, const QString &message, const QString &hint)
{
  Q_UNUSED(hint);
  if (!mProjectLoadActive || requestId < 0 || requestId >= mPendingProjectIds.size())
    return;

  const ProjectLayer *entry = projectLayerById(mPendingProjectIds.at(requestId));
  const QString name = entry ? entry->name : QFileInfo(mPendingProjectIds.at(requestId)).fileName();
  mUnavailableProjectLayers << QStringLiteral("%1: %2").arg(name, message);
}

void MainWindow::openLayerDialog()
{
  const QStringList files = QFileDialog::getOpenFileNames(
    this, QStringLiteral("Abrir capa"), mLastDirectory, QString::fromUtf8(kOpenFilter));

  if (!files.isEmpty())
    loadPaths(files);
}

void MainWindow::openRecentLayer()
{
  auto *action = qobject_cast<QAction *>(sender());
  if (!action)
    return;

  const QString path = action->data().toString();
  if (!QFileInfo::exists(path))
  {
    QMessageBox::warning(this, QStringLiteral("Archivo no encontrado"),
                         QStringLiteral("«%1» ya no existe. Se quitara de la lista de recientes.")
                           .arg(QDir::toNativeSeparators(path)));
    mRecentFiles.removeAll(path);
    updateRecentFilesMenu();
    writeSettings();
    return;
  }

  // Se carga en segundo plano, igual que el resto de aperturas, para que una
  // capa pesada no bloquee la ventana al abrirla desde «Recientes».
  loadPaths(QStringList {path});
}

void MainWindow::loadSampleData()
{
  const QString sampleDir = QString::fromUtf8(MINIGIS_SAMPLE_DATA_DIR) + QStringLiteral("/shapefiles");
  const QStringList candidates = {sampleDir + QStringLiteral("/cities.shp"),
                                  sampleDir + QStringLiteral("/parcels.shp")};

  QStringList found;
  for (const QString &candidate : candidates)
  {
    if (QFileInfo::exists(candidate))
      found << candidate;
  }

  if (found.isEmpty())
  {
    QMessageBox::information(
      this, QStringLiteral("Datos de ejemplo no encontrados"),
      QStringLiteral("No se han encontrado los shapefiles de ejemplo en:\n%1\n\n"
                     "Genera los datos con:\n    scripts\\make_sample_data.bat")
        .arg(QDir::toNativeSeparators(sampleDir)));
    return;
  }

  loadPaths(found);
}

void MainWindow::closeCurrentLayer()
{
  if (mCurrentRaster)
  {
    mCanvas->removeRasterLayer(mCurrentRaster);
    mCurrentRaster.reset();
    return;
  }

  if (const std::shared_ptr<VectorLayer> layer = currentLayer())
    mCanvas->removeLayer(layer);
}

void MainWindow::closeAllLayers()
{
  if (mProject && !maybeSaveProject())
    return;

  mCurrentRaster.reset();
  mCanvas->clearLayers();
  mCanvas->clearRasterLayers();

  mProject.reset();
  mProjectPath.clear();
  mPendingProjectIds.clear();
  mProjectVectorLayers.clear();
  mProjectRasterLayers.clear();
  mUnavailableProjectLayers.clear();
  mSavedProjectSignature.clear();
  mProjectLoadActive = false;

  updateWindowTitle();
  updateCrsLabel();
}

void MainWindow::zoomToCurrentLayer()
{
  if (mCurrentRaster)
  {
    mCanvas->zoomToLayer(mCurrentRaster);
    return;
  }

  if (const std::shared_ptr<VectorLayer> layer = currentLayer())
    mCanvas->zoomToLayer(layer);
  else
    statusBar()->showMessage(QStringLiteral("No hay ninguna capa activa."), kStatusTimeoutMs);
}

void MainWindow::editCurrentLayerStyle()
{
  if (mCurrentRaster)
  {
    editCurrentRasterStyle();
    return;
  }

  const std::shared_ptr<VectorLayer> layer = currentLayer();
  if (!layer)
  {
    statusBar()->showMessage(QStringLiteral("Selecciona una capa para cambiar su simbologia."), kStatusTimeoutMs);
    return;
  }

  StyleDialog dialog(layer->name(), layer->style(), this);
  if (dialog.exec() == QDialog::Accepted)
  {
    layer->setStyle(dialog.style());

    // Redibujado inmediato: descarta el cuadro cacheado y repinta. El lienzo
    // tambien detecta el cambio por la revision de la capa, pero invalidar aqui
    // evita depender de ello.
    mCanvas->refreshMap();

    updateWindowTitle();
    statusBar()->showMessage(QStringLiteral("Simbologia actualizada en «%1».").arg(layer->name()), kStatusTimeoutMs);
  }
}

void MainWindow::editCurrentRasterStyle()
{
  if (!mCurrentRaster)
    return;

  RasterStyleDialog dialog(mCurrentRaster->name(), mCurrentRaster->bandLabels(), mCurrentRaster->style(), this);
  if (dialog.exec() == QDialog::Accepted)
  {
    mCurrentRaster->setStyle(dialog.style());

    // Redibujado inmediato: el lienzo tambien lo detecta por la revision, pero
    // invalidar aqui hace que el cambio se vea sin mover la vista.
    mCanvas->refreshMap();

    updateWindowTitle();
    statusBar()->showMessage(QStringLiteral("Estilo del raster «%1» actualizado.").arg(mCurrentRaster->name()),
                             kStatusTimeoutMs);
  }
}

void MainWindow::editCurrentLayerLabels()
{
  if (mCurrentRaster)
  {
    statusBar()->showMessage(QStringLiteral("Las etiquetas solo se aplican a capas vectoriales."), kStatusTimeoutMs);
    return;
  }

  const std::shared_ptr<VectorLayer> layer = currentLayer();
  if (!layer)
  {
    statusBar()->showMessage(QStringLiteral("Selecciona una capa para configurar sus etiquetas."), kStatusTimeoutMs);
    return;
  }

  if (layer->fields().isEmpty())
  {
    statusBar()->showMessage(QStringLiteral("La capa «%1» no tiene campos que mostrar.").arg(layer->name()),
                             kStatusTimeoutMs);
    return;
  }

  LabelDialog dialog(layer->name(), layer->fields(), layer->labelStyle(), mCanvas->mapUnitsPerPixel(), this);
  if (dialog.exec() != QDialog::Accepted)
    return;

  const LabelStyle style = dialog.labelStyle();
  layer->setLabelStyle(style);

  // Redibujado inmediato: descarta el cuadro cacheado y repinta. Las etiquetas se
  // dibujan despues de la geometria, asi que el cambio se ve al instante.
  mCanvas->refreshMap();
  updateWindowTitle();

  if (style.enabled && !style.fields.isEmpty())
  {
    const LabelRenderer::FrameStats &stats = mCanvas->labelFrameStats();
    statusBar()->showMessage(QStringLiteral("Etiquetas de «%1»: %2 campos (%3). %4 etiquetas en la vista.")
                               .arg(layer->name())
                               .arg(style.fields.size())
                               .arg(style.fields.join(style.separator))
                               .arg(stats.placed),
                             kStatusTimeoutMs);
  }
  else
  {
    statusBar()->showMessage(QStringLiteral("Etiquetas desactivadas en «%1».").arg(layer->name()), kStatusTimeoutMs);
  }
}

void MainWindow::showAttributeTable(bool visible)
{
  if (!mAttributeDock)
    return;

  if (visible && mCurrentRaster)
  {
    statusBar()->showMessage(QStringLiteral("La tabla de atributos solo existe para capas vectoriales."),
                             kStatusTimeoutMs);
    return;
  }

  mAttributeDock->setVisible(visible);
  if (visible)
  {
    mAttributeDock->raise();
    if (!mAttributeModel->layer())
      replaceAttributeModel();
  }
}

void MainWindow::onLayersChanged()
{
  syncLayerPanel();
  replaceAttributeModel();
  updateWindowTitle();
  updateCrsLabel();
}

void MainWindow::onCurrentLayerChanged()
{
  syncLayerPanel();
  replaceAttributeModel();
  updateWindowTitle();
  updateCrsLabel();
}

void MainWindow::onLayerPanelCurrentChanged(const LayerEntry &entry)
{
  if (mUpdatingUi)
    return;

  if (entry.kind == LayerEntry::Kind::Raster)
  {
    if (!entry.raster || mCurrentRaster == entry.raster)
      return;

    mCurrentRaster = entry.raster;
    if (mCanvas->currentLayer())
      mCanvas->setCurrentLayer(nullptr);

    syncLayerPanel();
    replaceAttributeModel();
    updateWindowTitle();
    updateCrsLabel();
    return;
  }

  if (!entry.vector)
    return;

  mCurrentRaster.reset();
  if (mCanvas->currentLayer() != entry.vector)
  {
    mCanvas->setCurrentLayer(entry.vector);
  }
  else
  {
    syncLayerPanel();
    updateWindowTitle();
    updateCrsLabel();
  }
}

void MainWindow::onLayerVisibilityChanged(const LayerEntry &entry, bool visible)
{
  QString name;

  if (entry.kind == LayerEntry::Kind::Raster && entry.raster)
  {
    entry.raster->setVisible(visible);
    name = entry.raster->name();
  }
  else if (entry.vector)
  {
    entry.vector->setVisible(visible);
    name = entry.vector->name();
  }
  else
  {
    return;
  }

  // El mapa se actualiza en el acto: descarta el cuadro cacheado y repinta.
  mCanvas->refreshMap();
  updateWindowTitle();

  statusBar()->showMessage(QStringLiteral("Capa «%1» %2.")
                             .arg(name, visible ? QStringLiteral("visible") : QStringLiteral("oculta")),
                           kStatusTimeoutMs);
}

void MainWindow::onCursorMoved(const QPointF &mapPoint, bool hasCrs)
{
  if (!hasCrs)
  {
    mCoordinateLabel->setText(QStringLiteral("x: %1   y: %2 (sin SRC)").arg(formatNumber(mapPoint.x()), formatNumber(mapPoint.y())));
    return;
  }

  mCoordinateLabel->setText(QStringLiteral("x: %1   y: %2").arg(formatNumber(mapPoint.x()), formatNumber(mapPoint.y())));
}

void MainWindow::onCursorLeft()
{
  mCoordinateLabel->setText(QStringLiteral("x: —   y: —"));
}

void MainWindow::onScaleChanged(double mapUnitsPerPixel)
{
  mScaleLabel->setText(QStringLiteral("1 px = %1").arg(QString::number(mapUnitsPerPixel, 'g', 4)));
  mRenderLabel->setText(QStringLiteral("render: %1 ms").arg(mCanvas->lastRenderMilliseconds()));
}

void MainWindow::onStatusMessage(const QString &message, int timeoutMs)
{
  statusBar()->showMessage(message, timeoutMs);
}

std::vector<LayerPanel::TreeRow> MainWindow::layerTreeRows() const
{
  return mLayerPanel ? mLayerPanel->treeRows() : std::vector<LayerPanel::TreeRow>();
}

bool MainWindow::identifyPanelVisible() const
{
  return mIdentifyDock && mIdentifyDock->isVisible();
}

std::vector<IdentifyPanel::Row> MainWindow::identifyRows() const
{
  return mIdentifyPanel ? mIdentifyPanel->rows() : std::vector<IdentifyPanel::Row>();
}

QString MainWindow::identifyHeaderText() const
{
  return mIdentifyPanel ? mIdentifyPanel->headerText() : QString();
}

void MainWindow::setSearchAllLayers(bool all)
{
  mSearchAllLayers = all;
  if (mIdentifyPanel)
    mIdentifyPanel->setSearchAllLayers(all);

  if (mHasLastIdentifyPoint)
    identifyAtMapPoint(mLastIdentifyPoint);
}

bool MainWindow::selectLayer(const QString &name)
{
  const QString wanted = name.trimmed();

  if (wanted.isEmpty())
  {
    mCurrentRaster.reset();
    mCanvas->setCurrentLayer(nullptr);
    syncLayerPanel();
    updateWindowTitle();
    updateCrsLabel();
    replaceAttributeModel();
    return true;
  }

  for (const std::shared_ptr<VectorLayer> &layer : mCanvas->layers())
  {
    if (layer && layer->name().compare(wanted, Qt::CaseInsensitive) == 0)
    {
      mCurrentRaster.reset();
      mCanvas->setCurrentLayer(layer);
      syncLayerPanel();
      updateWindowTitle();
      updateCrsLabel();
      replaceAttributeModel();
      return true;
    }
  }

  for (const std::shared_ptr<RasterLayer> &raster : mCanvas->rasterLayers())
  {
    if (raster && raster->name().compare(wanted, Qt::CaseInsensitive) == 0)
    {
      mCurrentRaster = raster;
      mCanvas->setCurrentLayer(nullptr);
      syncLayerPanel();
      updateWindowTitle();
      updateCrsLabel();
      replaceAttributeModel();
      return true;
    }
  }

  return false;
}

IdentifyResult MainWindow::identifyAtMapPoint(const QPointF &mapPoint)
{
  mLastIdentifyPoint = mapPoint;
  mHasLastIdentifyPoint = true;

  if (mCanvas)
    mCanvas->setIdentifyMarker(mapPoint);

  const int radiusPixels = kIdentifyRadiusPixels;
  const double tolerance = mCanvas ? mCanvas->mapUnitsPerPixel() * radiusPixels : 0.0;

  const std::shared_ptr<VectorLayer> selected = mCanvas ? mCanvas->currentLayer() : nullptr;

  std::vector<std::shared_ptr<VectorLayer>> visible;
  if (mCanvas)
  {
    for (const std::shared_ptr<VectorLayer> &layer : mCanvas->layers())
    {
      if (layer && layer->isVisible())
        visible.push_back(layer);
    }
  }

  std::vector<std::shared_ptr<VectorLayer>> scope;
  std::shared_ptr<VectorLayer> priority;
  QString scopeText;
  QStringList notes;

  if (selected)
  {
    if (!selected->isVisible())
    {
      // Ni se busca ni se da prioridad: una capa oculta no esta en el mapa.
      scopeText = QStringLiteral("Solo la capa «%1» (oculta)").arg(selected->name());
      notes << QStringLiteral("La capa seleccionada «%1» está oculta: no se busca nada.")
                 .arg(selected->name());
    }
    else if (!mSearchAllLayers)
    {
      priority = selected;
      scope.push_back(selected);
      scopeText = QStringLiteral("Solo la capa «%1» (seleccionada)").arg(selected->name());
    }
    else
    {
      priority = selected;
      scope = visible;
      scopeText = QStringLiteral("Capa «%1» primero; además todas las visibles (%2)")
                    .arg(selected->name())
                    .arg(visible.size());
    }
  }
  else if (mCurrentRaster)
  {
    scope = visible;
    scopeText = QStringLiteral("Todas las capas visibles (%1)").arg(visible.size());
    notes << QStringLiteral("La capa seleccionada es un ráster: se buscan las capas vectoriales visibles.");
  }
  else
  {
    scope = visible;
    scopeText = QStringLiteral("Todas las capas visibles (%1)").arg(visible.size());
  }

  if (scope.empty() && notes.isEmpty())
    notes << QStringLiteral("No hay capas vectoriales visibles.");

  IdentifyRequest request;
  request.mapPoint = mapPoint;
  request.destinationEpsg = mCanvas ? mCanvas->destinationEpsg() : 4326;
  request.layers = scope;
  request.priorityLayer = priority;
  request.options.tolerance = tolerance;

  const IdentifyResult result = identifyAtPoint(request);

  // El primer resultado es el objeto seleccionado: se resalta y, si la tabla
  // esta visible, se salta a su registro. Sin resultados se limpia.
  std::shared_ptr<VectorLayer> selectedLayer;
  qint64 selectedFid = -1;
  if (result.hits.isEmpty())
  {
    clearSelection();
  }
  else
  {
    selectedLayer = result.hits.first().layer;
    selectedFid = result.hits.first().fid;
    applySelection(selectedLayer, selectedFid);
  }

  if (result.hits.isEmpty() && notes.isEmpty())
  {
    QString note = QStringLiteral("No hay ningún elemento a menos de %1 px.").arg(radiusPixels);
    if (selected && !mSearchAllLayers)
      note += QStringLiteral(" Marca «Buscar en todas las capas» para consultar el resto.");
    notes << note;
  }

  if (mIdentifyPanel)
  {
    IdentifyPanel::View view;
    view.result = result;
    view.radiusPixels = radiusPixels;
    view.scopeText = scopeText;
    view.notes = notes;
    view.priorityLayer = priority;
    view.selectedLayer = selectedLayer;
    view.selectedFid = selectedFid;
    mIdentifyPanel->setResult(view);
  }

  if (mIdentifyDock)
  {
    if (!mIdentifyDock->isVisible())
      mIdentifyDock->show();
    mIdentifyDock->raise();
  }

  statusBar()->showMessage(result.hits.isEmpty()
                             ? QStringLiteral("Identificar: sin elementos.")
                             : QStringLiteral("Identificar: %1 elemento(s).").arg(result.hits.size()),
                           kStatusTimeoutMs);

  return result;
}

void MainWindow::onIdentifyRequested(const QPointF &mapPoint, const QPoint &pixel)
{
  Q_UNUSED(pixel)
  identifyAtMapPoint(mapPoint);
}

void MainWindow::showIdentifyPanel(bool visible)
{
  if (!mIdentifyDock)
    return;

  if (visible)
  {
    mIdentifyDock->show();
    mIdentifyDock->raise();
  }
  else
  {
    mIdentifyDock->hide();
  }
}

void MainWindow::onSearchAllLayersChanged(bool all)
{
  mSearchAllLayers = all;
  if (mHasLastIdentifyPoint)
    identifyAtMapPoint(mLastIdentifyPoint);
}

void MainWindow::syncLayerPanel()
{
  if (mUpdatingUi || !mLayerPanel)
    return;

  mUpdatingUi = true;

  // Orden de apilado: primero los rasters (fondo), despues las vectoriales.
  std::vector<LayerEntry> entries;
  entries.reserve(mCanvas->rasterLayers().size() + mCanvas->layers().size());

  for (const std::shared_ptr<RasterLayer> &raster : mCanvas->rasterLayers())
  {
    if (!raster)
      continue;

    LayerEntry entry;
    entry.kind = LayerEntry::Kind::Raster;
    entry.raster = raster;
    entry.name = raster->name();
    entry.sourcePath = raster->sourcePath();
    entry.kindText = raster->bandCount() == 1
                       ? QStringLiteral("Raster (1 banda)")
                       : QStringLiteral("Raster (%1 bandas)").arg(raster->bandCount());
    entry.countText = QStringLiteral("%1x%2").arg(raster->width()).arg(raster->height());
    entry.crsText = raster->crsDescription();
    entry.visible = raster->isVisible();
    entries.push_back(entry);
  }

  for (const std::shared_ptr<VectorLayer> &layer : mCanvas->layers())
  {
    if (!layer)
      continue;

    LayerEntry entry;
    entry.kind = LayerEntry::Kind::Vector;
    entry.vector = layer;
    entry.name = layer->name();
    entry.sourcePath = layer->sourcePath();
    // Solo se agrupa lo que viene de un archivo con varias capas: es lo que el
    // usuario reconoce como carpetas y evita una carpeta con una sola capa.
    if (layer->layerCount() > 1)
      entry.groupName = layer->containerName();
    entry.kindText = Style::kindName(layer->geometryKind());
    entry.countText = QString::number(layer->featureCount());
    entry.crsText = layer->crsDescription();
    entry.visible = layer->isVisible();
    entries.push_back(entry);
  }

  // Si la capa raster activa ya no esta (la quitaron), se deja de marcar.
  if (mCurrentRaster && !mCanvas->containsRaster(mCurrentRaster))
    mCurrentRaster.reset();

  const LayerEntry *current = nullptr;
  for (const LayerEntry &entry : entries)
  {
    if (mCurrentRaster && entry.raster == mCurrentRaster)
    {
      current = &entry;
      break;
    }
    if (!mCurrentRaster && entry.vector && entry.vector == mCanvas->currentLayer())
    {
      current = &entry;
      break;
    }
  }

  mLayerPanel->setLayers(entries, current);
  mUpdatingUi = false;
}

void MainWindow::replaceAttributeModel()
{
  setAttributeLayer(currentLayer());
}

void MainWindow::setAttributeLayer(const std::shared_ptr<VectorLayer> &layer)
{
  if (!mAttributeModel)
    return;

  mAttributeModel->setLayer(layer);

  if (!mAttributeDock)
    return;

  if (!layer)
  {
    mAttributeDock->setWindowTitle(QStringLiteral("Tabla de atributos"));
    return;
  }

  if (mAttributeModel->isTruncated())
  {
    mAttributeDock->setWindowTitle(QStringLiteral("Tabla de atributos — %1 (mostrando %2 de %3 elementos)")
                                     .arg(layer->name())
                                     .arg(mAttributeModel->rowCount())
                                     .arg(mAttributeModel->totalRows()));
  }
  else
  {
    mAttributeDock->setWindowTitle(QStringLiteral("Tabla de atributos — %1 (%2 elementos)")
                                     .arg(layer->name())
                                     .arg(mAttributeModel->totalRows()));
  }

  mAttributeView->resizeColumnsToContents();
}

void MainWindow::applySelection(const std::shared_ptr<VectorLayer> &layer, qint64 fid)
{
  if (!mCanvas || !layer || fid < 0)
  {
    clearSelection();
    return;
  }

  mCanvas->setSelectedFeature(layer, fid);

  if (mAttributeDock && mAttributeDock->isVisible())
    showAttributeSelection(layer, fid);
}

void MainWindow::clearSelection()
{
  if (mCanvas)
    mCanvas->clearSelectedFeature();
}

void MainWindow::showAttributeSelection(const std::shared_ptr<VectorLayer> &layer, qint64 fid)
{
  if (!mAttributeModel || !mAttributeView || !layer || fid < 0)
    return;

  if (mAttributeModel->layer() != layer)
    setAttributeLayer(layer);

  const int row = static_cast<int>(fid);
  if (row >= mAttributeModel->rowCount())
  {
    statusBar()->showMessage(QStringLiteral("El registro %1 no está en la tabla: se muestran %2 de %3.")
                               .arg(fid)
                               .arg(mAttributeModel->rowCount())
                               .arg(mAttributeModel->totalRows()),
                             kStatusTimeoutMs);
    return;
  }

  const QModelIndex index = mAttributeModel->index(row, 0);
  // Sin foco: el usuario sigue identificando con el mapa.
  mAttributeView->setCurrentIndex(index);
  mAttributeView->selectRow(row);
  mAttributeView->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

QString MainWindow::selectedLayerName() const
{
  if (!mCanvas || !mCanvas->selectedFeatureLayer())
    return {};
  return mCanvas->selectedFeatureLayer()->name();
}

qint64 MainWindow::selectedFeatureId() const
{
  return mCanvas ? mCanvas->selectedFeatureId() : -1;
}

bool MainWindow::hasSelectedFeature() const
{
  return mCanvas && mCanvas->hasSelectedFeature();
}

QString MainWindow::attributeTableLayerName() const
{
  if (!mAttributeModel || !mAttributeModel->layer())
    return {};
  return mAttributeModel->layer()->name();
}

int MainWindow::attributeTableCurrentRow() const
{
  if (!mAttributeView)
    return -1;
  const QModelIndex index = mAttributeView->currentIndex();
  return index.isValid() ? index.row() : -1;
}

void MainWindow::updateWindowTitle()
{
  const std::shared_ptr<RasterLayer> raster = mCurrentRaster;
  const std::shared_ptr<VectorLayer> layer = currentLayer();
  const int layerCount = static_cast<int>(mCanvas->layers().size() + mCanvas->rasterLayers().size());

  QString title = QStringLiteral("MiniGIS %1").arg(QString::fromUtf8(MINIGIS_VERSION));
  const bool modified = mProject != nullptr && !mProjectPath.isEmpty() && isProjectModified();
  if (!mProjectPath.isEmpty())
    title += QStringLiteral(" — %1%2").arg(QFileInfo(mProjectPath).fileName(),
                                           modified ? QStringLiteral("*") : QString());
  if (raster)
  {
    title += QStringLiteral(" — %1 (%2x%3, %4 bandas) — %5 capa(s)")
               .arg(raster->name())
               .arg(raster->width())
               .arg(raster->height())
               .arg(raster->bandCount())
               .arg(layerCount);
  }
  else if (layer)
  {
    title += QStringLiteral(" — %1 (%2 elementos) — %3 capa(s)").arg(layer->name()).arg(layer->featureCount()).arg(layerCount);
  }
  else if (layerCount > 0)
  {
    title += QStringLiteral(" — %1 capa(s)").arg(layerCount);
  }

  setWindowTitle(title);

  const bool hasCurrent = raster != nullptr || layer != nullptr;
  mCloseAction->setEnabled(hasCurrent);
  mStyleAction->setEnabled(hasCurrent);
  mZoomLayerAction->setEnabled(hasCurrent);
  mSaveProjectAction->setEnabled(mProject != nullptr && !mProjectPath.isEmpty() && modified);
  mSaveProjectAsAction->setEnabled(mProject != nullptr);

  // Etiquetas y tabla de atributos solo tienen sentido en capas vectoriales.
  const bool hasVector = layer != nullptr && raster == nullptr;
  mLabelAction->setEnabled(hasVector);
  mAttributeAction->setEnabled(hasVector);
}

void MainWindow::updateCrsLabel()
{
  if (mCurrentRaster)
  {
    mCrsLabel->setText(QStringLiteral("SRC: %1").arg(mCurrentRaster->crsDescription()));
    return;
  }

  const std::shared_ptr<VectorLayer> layer = currentLayer();
  if (!layer)
  {
    mCrsLabel->setText(QStringLiteral("SRC: —"));
    return;
  }

  mCrsLabel->setText(QStringLiteral("SRC: %1").arg(layer->crsDescription()));
}

void MainWindow::updateRecentFilesMenu()
{
  if (!mRecentMenu)
    return;

  mRecentMenu->clear();
  if (mRecentFiles.isEmpty())
  {
    QAction *empty = mRecentMenu->addAction(QStringLiteral("(vacio)"));
    empty->setEnabled(false);
    return;
  }

  for (const QString &path : mRecentFiles)
  {
    QAction *action = mRecentMenu->addAction(QFileInfo(path).fileName());
    action->setToolTip(QDir::toNativeSeparators(path));
    action->setData(path);
    connect(action, &QAction::triggered, this, &MainWindow::openRecentLayer);
  }

  mRecentMenu->addSeparator();
  QAction *clearAction = mRecentMenu->addAction(QStringLiteral("Limpiar la lista"));
  connect(clearAction, &QAction::triggered, this, [this] {
    mRecentFiles.clear();
    updateRecentFilesMenu();
    writeSettings();
  });
}

void MainWindow::addRecentFile(const QString &path)
{
  const QString absolutePath = QFileInfo(path).absoluteFilePath();
  mRecentFiles.removeAll(absolutePath);
  mRecentFiles.prepend(absolutePath);
  while (mRecentFiles.size() > kMaximumRecentFiles)
    mRecentFiles.removeLast();

  updateRecentFilesMenu();
  writeSettings();
}

void MainWindow::showError(const QString &title, const Error &error)
{
  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(title);
  box.setText(error.message.isEmpty() ? QStringLiteral("Error desconocido.") : error.message);
  if (!error.hint.isEmpty())
    box.setInformativeText(error.hint);
  box.setStandardButtons(QMessageBox::Ok);
  box.exec();
}

void MainWindow::showAboutDialog()
{
  const RuntimeInfo info = initializeGdal();

  const QString text =
    QStringLiteral("<h3>MiniGIS %1</h3>"
                   "<p>Visor SIG mínimo para cargar shapefiles, dibujarlos y consultar sus atributos.</p>"
                   "<p>Construido en C++ con Qt 6 y GDAL, siguiendo las convenciones de compilacion de QGIS "
                   "(CMake + vcpkg + capas core/gui/app).</p>"
                   "<p><b>Qt</b>: %2<br><b>GDAL</b>: %3<br><b>PROJ</b>: %4</p>"
                   "<p>Los formatos vectoriales los aporta GDAL/OGR; no se reutiliza código de QGIS.</p>")
      .arg(QString::fromUtf8(MINIGIS_VERSION), QString::fromUtf8(qVersion()), info.gdalVersion, info.projVersion);

  QMessageBox::about(this, QStringLiteral("Acerca de MiniGIS"), text);
}

void MainWindow::showSystemInfo()
{
  const RuntimeInfo info = initializeGdal();

  const QString text =
    QStringLiteral("MiniGIS      : %1\n"
                   "Compilado    : %2\n"
                   "Compilador   : %3\n"
                   "Qt           : %4\n"
                   "%5\n"
                   "Ejecutable   : %6\n"
                   "Datos ejemplo: %7\n"
                   "Drivers vec. : %8")
      .arg(QString::fromUtf8(MINIGIS_VERSION),
           QString::fromUtf8(MINIGIS_BUILD_TYPE),
           QStringLiteral("%1 %2").arg(QString::fromUtf8(MINIGIS_COMPILER_ID), QString::fromUtf8(MINIGIS_COMPILER_VERSION)),
           QString::fromUtf8(qVersion()),
           info.summary(),
           QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
           QDir::toNativeSeparators(QString::fromUtf8(MINIGIS_SAMPLE_DATA_DIR)))
      .arg(vectorDriverNames().size());

  QMessageBox box(this);
  box.setIcon(QMessageBox::Information);
  box.setWindowTitle(QStringLiteral("Informacion del sistema"));
  box.setText(QStringLiteral("Entorno de ejecucion de MiniGIS"));
  box.setDetailedText(text);
  box.setStandardButtons(QMessageBox::Ok);
  box.exec();
}

void MainWindow::readSettings()
{
  if (!mRestoreSettings)
  {
    mLastDirectory = QDir::homePath();
    return;
  }

  QSettings settings;
  const QByteArray geometry = settings.value(QStringLiteral("mainWindow/geometry")).toByteArray();
  if (!geometry.isEmpty())
    restoreGeometry(geometry);

  const QByteArray state = settings.value(QStringLiteral("mainWindow/state")).toByteArray();
  if (!state.isEmpty())
    restoreState(state);

  mRecentFiles = settings.value(QStringLiteral("recentFiles")).toStringList();
  mLastDirectory = settings.value(QStringLiteral("lastDirectory"), QDir::homePath()).toString();
}

void MainWindow::writeSettings()
{
  // En las ejecuciones de captura no se toca la configuracion del usuario
  // (ni su geometria de ventana ni la lista de archivos recientes).
  if (!mRestoreSettings)
    return;

  QSettings settings;
  settings.setValue(QStringLiteral("mainWindow/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("mainWindow/state"), saveState());
  settings.setValue(QStringLiteral("recentFiles"), mRecentFiles);
  settings.setValue(QStringLiteral("lastDirectory"), mLastDirectory);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
  if (!maybeSaveProject())
  {
    event->ignore();
    return;
  }

  writeSettings();
  QMainWindow::closeEvent(event);
}

} // namespace minigis
