#include "minigismainwindow.h"

#include "minigisattributetablemodel.h"
#include "minigiserror.h"
#include "minigisgdalinit.h"
#include "minigisgeometrystore.h"
#include "minigislayerpanel.h"
#include "minigismapcanvas.h"
#include "minigislabeldialog.h"
#include "minigisstyledialog.h"
#include "minigisvectorlayer.h"
#include "minigisvectorloader.h"

#include <QAction>
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

/** Formatos vectoriales que ofrece el dialogo de apertura. */
const char *kOpenFilter =
  "Capas vectoriales (*.shp *.geojson *.json *.gpkg *.kml *.gml *.gpx *.csv *.tab *.mif *.fgb *.sqlite *.db);;"
  "Shapefile (*.shp);;GeoJSON (*.geojson *.json);;GeoPackage (*.gpkg);;Todos los archivos (*)";

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
  connect(mCanvas, &MapCanvas::filesDropped, this, [this](const QStringList &paths) { loadPaths(paths); });
  connect(mCanvas, &MapCanvas::statusMessage, this, &MainWindow::onStatusMessage);

  // Carga en segundo plano: abrir y leer una capa pesada ya no bloquea la
  // ventana, y el progreso se puede mostrar mientras tanto.
  mLoader = new VectorLayerLoader(this);
  connect(mLoader, &VectorLayerLoader::progressChanged, this, &MainWindow::onLoadProgress);
  connect(mLoader, &VectorLayerLoader::layerReady, this, &MainWindow::onLayerLoaded);
  connect(mLoader, &VectorLayerLoader::layerFailed, this, &MainWindow::onLayerLoadFailed);
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
  mRecentMenu = fileMenu->addMenu(QStringLiteral("Abrir recientes"));
  fileMenu->addAction(mSampleAction);
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

  QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("A&yuda"));
  helpMenu->addAction(mSystemInfoAction);
  helpMenu->addAction(mAboutAction);

  QToolBar *toolBar = addToolBar(QStringLiteral("Principal"));
  toolBar->setObjectName(QStringLiteral("mainToolBar"));
  toolBar->setMovable(false);
  toolBar->addAction(mOpenAction);
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
  connect(mLayerPanel, &LayerPanel::removeLayerRequested, this, [this](const std::shared_ptr<VectorLayer> &layer) {
    mCanvas->removeLayer(layer);
  });
  connect(mLayerPanel, &LayerPanel::zoomToLayerRequested, mCanvas, &MapCanvas::zoomToLayer);
  connect(mLayerPanel, &LayerPanel::styleRequested, this, [this](const std::shared_ptr<VectorLayer> &layer) {
    mCanvas->setCurrentLayer(layer);
    editCurrentLayerStyle();
  });
  connect(mLayerPanel, &LayerPanel::labelRequested, this, [this](const std::shared_ptr<VectorLayer> &layer) {
    if (layer)
      mCanvas->setCurrentLayer(layer);
    editCurrentLayerLabels();
  });
  connect(mLayerPanel, &LayerPanel::attributeTableRequested, this, [this](const std::shared_ptr<VectorLayer> &layer) {
    mCanvas->setCurrentLayer(layer);
    showAttributeTable(true);
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
  mCanvas->addLayer(layer, zoomToExtent);
  mLastDirectory = QFileInfo(layer->sourcePath()).absolutePath();
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
  setLoadIndicatorVisible(false);
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

std::shared_ptr<VectorLayer> MainWindow::currentLayer() const
{
  return mCanvas ? mCanvas->currentLayer() : nullptr;
}

bool MainWindow::openLayer(const QString &path, bool zoomToExtent)
{
  if (path.isEmpty())
    return false;

  Error error;
  const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, &error);
  if (!layer)
  {
    showError(QStringLiteral("No se pudo abrir la capa"), error);
    return false;
  }

  mLastDirectory = QFileInfo(path).absolutePath();
  mCanvas->addLayer(layer, zoomToExtent);
  addRecentFile(path);

  statusBar()->showMessage(QStringLiteral("Capa «%1» cargada: %2 elementos (%3)")
                             .arg(layer->name())
                             .arg(layer->featureCount())
                             .arg(layer->crsDescription()),
                           kStatusTimeoutMs);
  return true;
}

int MainWindow::loadPaths(const QStringList &paths)
{
  if (paths.isEmpty())
    return 0;

  // A partir de aqui la carga es asincrona: se devuelve cuantas se han encolado
  // y la interfaz se actualiza desde las senales del cargador (barra de progreso
  // y, por cada capa lista, su incorporacion al lienzo).
  if (mLoader->isRunning())
  {
    statusBar()->showMessage(QStringLiteral("Ya hay una carga en curso; espera o pulsa Cancelar."),
                             kStatusTimeoutMs);
    return 0;
  }

  mLoader->setDestinationEpsg(mCanvas->destinationEpsg());
  setLoadIndicatorVisible(true);
  mLoadProgress->setValue(0);
  mLoadLabel->setText(QStringLiteral("Preparando la carga de %1 archivo(s)...").arg(paths.size()));
  mLoader->start(paths);
  return paths.size();
}

void MainWindow::openLayerDialog()
{
  const QStringList files = QFileDialog::getOpenFileNames(
    this, QStringLiteral("Abrir capa vectorial"), mLastDirectory, QString::fromUtf8(kOpenFilter));

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
  if (const std::shared_ptr<VectorLayer> layer = currentLayer())
    mCanvas->removeLayer(layer);
}

void MainWindow::closeAllLayers()
{
  mCanvas->clearLayers();
}

void MainWindow::zoomToCurrentLayer()
{
  if (const std::shared_ptr<VectorLayer> layer = currentLayer())
    mCanvas->zoomToLayer(layer);
  else
    statusBar()->showMessage(QStringLiteral("No hay ninguna capa activa."), kStatusTimeoutMs);
}

void MainWindow::editCurrentLayerStyle()
{
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

    statusBar()->showMessage(QStringLiteral("Simbologia actualizada en «%1».").arg(layer->name()), kStatusTimeoutMs);
  }
}

void MainWindow::editCurrentLayerLabels()
{
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

void MainWindow::onLayerPanelCurrentChanged(const std::shared_ptr<VectorLayer> &layer)
{
  if (mUpdatingUi || !layer || mCanvas->currentLayer() == layer)
    return;

  mCanvas->setCurrentLayer(layer);
}

void MainWindow::onLayerVisibilityChanged(const std::shared_ptr<VectorLayer> &layer, bool visible)
{
  if (!layer)
    return;

  layer->setVisible(visible);

  // El mapa se actualiza en el acto: descarta el cuadro cacheado y repinta.
  mCanvas->refreshMap();

  statusBar()->showMessage(QStringLiteral("Capa «%1» %2.")
                             .arg(layer->name(), visible ? QStringLiteral("visible") : QStringLiteral("oculta")),
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

void MainWindow::syncLayerPanel()
{
  if (mUpdatingUi || !mLayerPanel)
    return;

  mUpdatingUi = true;
  mLayerPanel->setLayers(mCanvas->layers(), mCanvas->currentLayer());
  mUpdatingUi = false;
}

void MainWindow::replaceAttributeModel()
{
  const std::shared_ptr<VectorLayer> layer = currentLayer();
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

void MainWindow::updateWindowTitle()
{
  const std::shared_ptr<VectorLayer> layer = currentLayer();
  const int layerCount = static_cast<int>(mCanvas->layers().size());

  QString title = QStringLiteral("MiniGIS %1").arg(QString::fromUtf8(MINIGIS_VERSION));
  if (layer)
    title += QStringLiteral(" — %1 (%2 elementos) — %3 capa(s)").arg(layer->name()).arg(layer->featureCount()).arg(layerCount);
  else if (layerCount > 0)
    title += QStringLiteral(" — %1 capa(s)").arg(layerCount);

  setWindowTitle(title);

  const bool hasLayer = layer != nullptr;
  mCloseAction->setEnabled(hasLayer);
  mStyleAction->setEnabled(hasLayer);
  mLabelAction->setEnabled(hasLayer);
  mZoomLayerAction->setEnabled(hasLayer);
}

void MainWindow::updateCrsLabel()
{
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
                   "<p>Visor SIG minimo para cargar shapefiles, dibujarlos y consultar sus atributos.</p>"
                   "<p>Construido en C++ con Qt 6 y GDAL, siguiendo las convenciones de compilacion de QGIS "
                   "(CMake + vcpkg + capas core/gui/app).</p>"
                   "<p><b>Qt</b>: %2<br><b>GDAL</b>: %3<br><b>PROJ</b>: %4</p>"
                   "<p>Los formatos vectoriales los aporta GDAL/OGR; no se reutiliza codigo de QGIS.</p>")
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
  writeSettings();
  QMainWindow::closeEvent(event);
}

} // namespace minigis
