#pragma once

#include <QMainWindow>
#include <QStringList>

#include <memory>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QTableView;

namespace minigis
{

struct Error;
class AttributeTableModel;
class LayerPanel;
class MapCanvas;
class Rectangle;
class VectorLayer;

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

  /** Abre una capa y la anade al lienzo; muestra un dialogo si falla. */
  bool openLayer(const QString &path, bool zoomToExtent = false);

  /** Abre varias capas (linea de comandos o arrastrar y soltar). Devuelve cuantas se abrieron. */
  int loadPaths(const QStringList &paths);

  void showAttributeTable(bool visible);

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
  void showAboutDialog();
  void showSystemInfo();

  void onLayersChanged();
  void onCurrentLayerChanged();
  void onLayerPanelCurrentChanged(const std::shared_ptr<VectorLayer> &layer);
  void onLayerVisibilityChanged(const std::shared_ptr<VectorLayer> &layer, bool visible);
  void onCursorMoved(const QPointF &mapPoint, bool hasCrs);
  void onCursorLeft();
  void onScaleChanged(double mapUnitsPerPixel);
  void onStatusMessage(const QString &message, int timeoutMs);

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
  void showError(const QString &title, const Error &error);

  MapCanvas *mCanvas = nullptr;
  LayerPanel *mLayerPanel = nullptr;
  AttributeTableModel *mAttributeModel = nullptr;
  QTableView *mAttributeView = nullptr;
  QDockWidget *mLayerDock = nullptr;
  QDockWidget *mAttributeDock = nullptr;

  QLabel *mCoordinateLabel = nullptr;
  QLabel *mScaleLabel = nullptr;
  QLabel *mCrsLabel = nullptr;
  QLabel *mRenderLabel = nullptr;

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
  QAction *mAttributeAction = nullptr;
  QAction *mAboutAction = nullptr;
  QAction *mSystemInfoAction = nullptr;

  QStringList mRecentFiles;
  QString mLastDirectory;
  bool mUpdatingUi = false;
  bool mRestoreSettings = true;
};

} // namespace minigis
