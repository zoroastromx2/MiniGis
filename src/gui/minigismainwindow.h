#pragma once

#include "minigislayerpanel.h"

#include <QMainWindow>
#include <QStringList>

#include <memory>
#include <vector>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QProgressBar;
class QTableView;
class QToolButton;

namespace minigis
{

struct Error;
struct LayerEntry;
class AttributeTableModel;
class LayerPanel;
class MapCanvas;
class RasterLayer;
class Rectangle;
class VectorLayer;
class VectorLayerLoader;

/**
 * Ventana principal (equivalente simplificado a QgisApp).
 *
 * Reune el lienzo, el panel de capas, la tabla de atributos y las acciones de
 * menu/barra de herramientas. Toda la logica de datos vive en minigis_core.
 */
class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  /**
   * @param restoreSettings  si es false, la ventana no lee ni escribe QSettings
   *        (se usa en las capturas de --snapshot-window para que el tamano sea
   *        el pedido y no se altere la configuracion del usuario)
   */
  explicit MainWindow(QWidget *parent = nullptr, bool restoreSettings = true);
  ~MainWindow() override;

  MapCanvas *canvas() const { return mCanvas; }

  /** Abre un archivo y anade todas sus capas al lienzo; muestra un dialogo si falla. */
  bool openLayer(const QString &path, bool zoomToExtent = false);

  /**
   * Abre varios archivos (linea de comandos o arrastrar y soltar); de cada uno se
   * cargan todas sus capas. Devuelve cuantos archivos se encolaron.
   */
  int loadPaths(const QStringList &paths);

  /** True mientras haya una carga en segundo plano en curso. */
  bool isLoading() const;

  /**
   * Espera a que termine la carga en curso procesando eventos (bucle de eventos
   * anidado, no una espera bloqueante): los modos de captura lo usan para
   * trabajar con el mismo camino asincrono que la aplicacion real.
   */
  bool waitForLoading(int timeoutMs = 120000);

  void showAttributeTable(bool visible);

  /**
   * Filas del panel de capas tal como las ve el usuario. La usa el modo
   * `--check-layers` para comprobar, sin ventana, que un archivo con varias
   * capas se muestra entero y agrupado bajo su nombre.
   */
  std::vector<LayerPanel::TreeRow> layerTreeRows() const;

protected:
  void closeEvent(QCloseEvent *event) override;

private slots:
  void openLayerDialog();
  void openRecentLayer();
  void loadSampleData();
  void closeCurrentLayer();
  void closeAllLayers();
  void zoomToCurrentLayer();
  void editCurrentLayerStyle();
  /** Configura el contraste/bandas de la capa raster actual. */
  void editCurrentRasterStyle();
  /** Configura las etiquetas de la capa actual (campos, fuente, halo, escala). */
  void editCurrentLayerLabels();
  void showAboutDialog();
  void showSystemInfo();

  void onLayersChanged();
  void onCurrentLayerChanged();
  void onLayerPanelCurrentChanged(const LayerEntry &entry);
  void onLayerVisibilityChanged(const LayerEntry &entry, bool visible);
  void onCursorMoved(const QPointF &mapPoint, bool hasCrs);
  void onCursorLeft();
  void onScaleChanged(double mapUnitsPerPixel);
  void onStatusMessage(const QString &message, int timeoutMs);

  // Carga en segundo plano.
  void onLoadProgress(int percent, const QString &message);
  void onLayerLoaded(const std::shared_ptr<VectorLayer> &layer, bool first);
  void onLayerLoadFailed(const QString &path, const QString &message, const QString &hint);
  void onLoadFinished(int loaded, int failed, bool cancelled);

private:
  void createActions();
  void createMenusAndToolBar();
  void createDocks();
  void createStatusBar();

  void readSettings();
  void writeSettings();

  void syncLayerPanel();
  void replaceAttributeModel();
  void updateWindowTitle();
  void updateCrsLabel();
  void updateRecentFilesMenu();
  void addRecentFile(const QString &path);

  std::shared_ptr<VectorLayer> currentLayer() const;
  std::shared_ptr<RasterLayer> currentRaster() const { return mCurrentRaster; }
  void showError(const QString &title, const Error &error);

  /** Abre un raster ya detectado y lo añade al lienzo. */
  bool openRasterLayer(const QString &path, bool zoomToExtent);

  /** Muestra u oculta el indicador de carga de la barra de estado. */
  void setLoadIndicatorVisible(bool visible);

  /**
   * Encuadra la union de las capas y raster abiertos en la tanda actual. Se hace
   * al terminar (no capa a capa) para que un archivo con varias capas, o una
   * tanda de archivos, se vea entero: encuadrar solo la primera capa dejaria las
   * demas fuera de la vista aunque el panel las liste.
   */
  void frameLoadedBatch();

  MapCanvas *mCanvas = nullptr;
  LayerPanel *mLayerPanel = nullptr;
  AttributeTableModel *mAttributeModel = nullptr;
  QTableView *mAttributeView = nullptr;
  QDockWidget *mLayerDock = nullptr;
  QDockWidget *mAttributeDock = nullptr;
  VectorLayerLoader *mLoader = nullptr;
  std::shared_ptr<RasterLayer> mCurrentRaster;

  QLabel *mCoordinateLabel = nullptr;
  QLabel *mScaleLabel = nullptr;
  QLabel *mCrsLabel = nullptr;
  QLabel *mRenderLabel = nullptr;
  QLabel *mLoadLabel = nullptr;
  QProgressBar *mLoadProgress = nullptr;
  QToolButton *mCancelLoadButton = nullptr;

  QMenu *mRecentMenu = nullptr;
  QAction *mOpenAction = nullptr;
  QAction *mSampleAction = nullptr;
  QAction *mCloseAction = nullptr;
  QAction *mCloseAllAction = nullptr;
  QAction *mQuitAction = nullptr;
  QAction *mZoomInAction = nullptr;
  QAction *mZoomOutAction = nullptr;
  QAction *mZoomFullAction = nullptr;
  QAction *mZoomLayerAction = nullptr;
  QAction *mStyleAction = nullptr;
  QAction *mLabelAction = nullptr;
  QAction *mShowLabelsAction = nullptr;
  QAction *mAttributeAction = nullptr;
  QAction *mAboutAction = nullptr;
  QAction *mSystemInfoAction = nullptr;

  QStringList mRecentFiles;
  QString mLastDirectory;
  bool mUpdatingUi = false;
  bool mRestoreSettings = true;

  /** Donde empieza la tanda de carga en curso y si hay que encuadrarla al final. */
  size_t mLoadStartVectorLayers = 0;
  size_t mLoadStartRasterLayers = 0;
  bool mLoadBatchPending = false;
};

} // namespace minigis
