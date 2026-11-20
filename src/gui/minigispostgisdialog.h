#pragma once

#include <QDialog>
#include <QVector>

#include "minigisconnections.h"
#include "minigispgcatalog.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace minigis
{

/** Una tabla elegida en el dialogo PostGIS, lista para el cargador. */
struct PgLayerSelection
{
  PgConnection connection;
  QString schema;
  QString table;
  /** False = dibujar bajo demanda, sin construir el almacen (tablas enormes). */
  bool buildStore = true;
};

/**
 * Dialogo "Abrir conexion PostGIS".
 *
 * Pide la contrasena (la de Connections.xml esta cifrada con un esquema
 * propietario y no se puede descifrar), lista esquemas y tablas y devuelve las
 * seleccionadas. No abre capas: eso lo hace el cargador en segundo plano.
 */
class PostgisDialog : public QDialog
{
  Q_OBJECT

public:
  /**
   * `autoConnect` conecta solo si hay una sesion activa o una contrasena
   * recordada. La captura sin ventana lo desactiva para no tocar la red.
   */
  explicit PostgisDialog(QWidget *parent = nullptr, bool autoConnect = true);

  /** Ruta de Connections.xml; si esta vacia se resuelve como la CLI. */
  void setCatalogPath(const QString &path);

  QVector<PgLayerSelection> selections() const { return mSelections; }

private slots:
  void browseCatalog();
  void connectToServer();
  void reloadTables();
  void updateConnectionInfo();
  void updateAddButton();
  void applyFilter(const QString &text);
  /** Reconecta con la sesion o la contrasena recordada, sin pedir nada. */
  void restoreSession();

private:
  void buildUi();
  void loadCatalog();
  void setBusy(bool busy);
  void showStatus(const QString &message, bool error);
  void fillTables();
  const PgConnection *currentConnection() const;
  QVector<PgLayerSelection> collectSelections();

  void accept() override;

  PgConnectionCatalog mCatalog;
  PgConnection mConnected;
  QString mConnectedPassword;
  QString mCatalogPath;
  QVector<PgTableInfo> mTables;
  QVector<PgLayerSelection> mSelections;

  QComboBox *mConnectionCombo = nullptr;
  QLabel *mServerLabel = nullptr;
  QLineEdit *mPasswordEdit = nullptr;
  QCheckBox *mRememberCheck = nullptr;
  QPushButton *mBrowseButton = nullptr;
  QPushButton *mConnectButton = nullptr;
  QComboBox *mSchemaCombo = nullptr;
  QLineEdit *mFilterEdit = nullptr;
  QPushButton *mRefreshButton = nullptr;
  QTreeWidget *mTableTree = nullptr;
  QLabel *mStatusLabel = nullptr;
  QPushButton *mAddButton = nullptr;
  bool mBusy = false;
  bool mAutoConnect = true;
  bool mAutoConnectTried = false;
};

} // namespace minigis
