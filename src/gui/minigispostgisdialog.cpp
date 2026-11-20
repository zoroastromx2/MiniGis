#include "minigispostgisdialog.h"

#include "minigispgcredentials.h"
#include "minigispgsession.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace minigis
{
namespace
{

/** Por encima de este numero de elementos se pregunta antes de indexar. */
constexpr qint64 kBigTableFeatures = 1000000;

/** Eleccion recordada durante la sesion, por URI saneada de la tabla. */
QHash<QString, bool> &bigTableChoices()
{
  static QHash<QString, bool> choices;
  return choices;
}

} // namespace

PostgisDialog::PostgisDialog(QWidget *parent, bool autoConnect)
  : QDialog(parent)
  , mAutoConnect(autoConnect)
{
  buildUi();
  loadCatalog();

  // La reconexion se hace en el bucle de eventos, ya con el dialogo pintado,
  // para que el cursor de espera y el mensaje se vean desde el principio.
  if (mAutoConnect)
    QTimer::singleShot(0, this, &PostgisDialog::restoreSession);
}

void PostgisDialog::buildUi()
{
  setWindowTitle(QStringLiteral("Abrir conexión PostGIS"));
  setModal(true);

  auto *layout = new QVBoxLayout(this);

  auto *connectionRow = new QHBoxLayout;
  connectionRow->addWidget(new QLabel(QStringLiteral("Conexión:"), this));
  mConnectionCombo = new QComboBox(this);
  mConnectionCombo->setMinimumWidth(340);
  connectionRow->addWidget(mConnectionCombo, 1);
  mBrowseButton = new QPushButton(QStringLiteral("Examinar…"), this);
  mBrowseButton->setToolTip(QStringLiteral("Elegir otro Connections.xml de Mapa Digital"));
  connectionRow->addWidget(mBrowseButton);
  layout->addLayout(connectionRow);

  mServerLabel = new QLabel(this);
  mServerLabel->setWordWrap(true);
  mServerLabel->setTextFormat(Qt::PlainText);
  mServerLabel->setStyleSheet(QStringLiteral("color:#555;"));
  layout->addWidget(mServerLabel);

  auto *passwordRow = new QHBoxLayout;
  passwordRow->addWidget(new QLabel(QStringLiteral("Contraseña:"), this));
  mPasswordEdit = new QLineEdit(this);
  mPasswordEdit->setEchoMode(QLineEdit::Password);
  mPasswordEdit->setPlaceholderText(
    QStringLiteral("La de Connections.xml está cifrada; escríbela aquí"));
  passwordRow->addWidget(mPasswordEdit, 1);
  mRememberCheck = new QCheckBox(QStringLiteral("Recordar"), this);
  mRememberCheck->setChecked(true);
  mRememberCheck->setToolTip(
    QStringLiteral("Guardarla cifrada con DPAPI en tu perfil de Windows (por defecto)"));
  passwordRow->addWidget(mRememberCheck);
  mConnectButton = new QPushButton(QStringLiteral("Conectar"), this);
  passwordRow->addWidget(mConnectButton);
  layout->addLayout(passwordRow);

  auto *schemaRow = new QHBoxLayout;
  schemaRow->addWidget(new QLabel(QStringLiteral("Esquema:"), this));
  mSchemaCombo = new QComboBox(this);
  mSchemaCombo->setEnabled(false);
  mSchemaCombo->setMinimumWidth(220);
  schemaRow->addWidget(mSchemaCombo);
  mFilterEdit = new QLineEdit(this);
  mFilterEdit->setPlaceholderText(QStringLiteral("Filtrar tablas…"));
  mFilterEdit->setEnabled(false);
  schemaRow->addWidget(mFilterEdit, 1);
  mRefreshButton = new QPushButton(QStringLiteral("Actualizar"), this);
  mRefreshButton->setToolTip(
    QStringLiteral("Reconecta y vuelve a leer esquemas y tablas (por si han cambiado)"));
  mRefreshButton->setEnabled(false);
  schemaRow->addWidget(mRefreshButton);
  layout->addLayout(schemaRow);

  mTableTree = new QTreeWidget(this);
  mTableTree->setColumnCount(3);
  mTableTree->setHeaderLabels(
    {QStringLiteral("Tabla"), QStringLiteral("Geometría"), QStringLiteral("Elementos")});
  mTableTree->setRootIsDecorated(false);
  mTableTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  mTableTree->setAlternatingRowColors(true);
  mTableTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  mTableTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  mTableTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  layout->addWidget(mTableTree, 1);

  mStatusLabel = new QLabel(this);
  mStatusLabel->setWordWrap(true);
  mStatusLabel->setTextFormat(Qt::PlainText);
  layout->addWidget(mStatusLabel);

  auto *buttons = new QHBoxLayout;
  buttons->addStretch(1);
  mAddButton = new QPushButton(QStringLiteral("Añadir capas"), this);
  mAddButton->setDefault(true);
  mAddButton->setEnabled(false);
  buttons->addWidget(mAddButton);
  auto *cancelButton = new QPushButton(QStringLiteral("Cancelar"), this);
  buttons->addWidget(cancelButton);
  layout->addLayout(buttons);

  connect(mBrowseButton, &QPushButton::clicked, this, &PostgisDialog::browseCatalog);
  connect(mConnectButton, &QPushButton::clicked, this, &PostgisDialog::connectToServer);
  connect(mConnectionCombo, &QComboBox::currentIndexChanged, this,
          &PostgisDialog::updateConnectionInfo);
  connect(mSchemaCombo, &QComboBox::currentIndexChanged, this, &PostgisDialog::reloadTables);
  connect(mFilterEdit, &QLineEdit::textChanged, this, &PostgisDialog::applyFilter);
  connect(mTableTree, &QTreeWidget::itemSelectionChanged, this, &PostgisDialog::updateAddButton);
  connect(mAddButton, &QPushButton::clicked, this, &PostgisDialog::accept);
  connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
  connect(mPasswordEdit, &QLineEdit::returnPressed, this, &PostgisDialog::connectToServer);
  connect(mRefreshButton, &QPushButton::clicked, this, [this]() {
    if (!mConnected.isValid())
      return;
    // Cierra la conexion viva y vuelve a leer el catalogo desde el servidor.
    PgCatalog::closeSession();
    connectToServer();
  });

  resize(820, 600);
}

void PostgisDialog::setCatalogPath(const QString &path)
{
  if (path == mCatalogPath)
    return;
  mCatalogPath = path;
  loadCatalog();
}

void PostgisDialog::loadCatalog()
{
  if (mCatalogPath.isEmpty())
    mCatalogPath = PgConnectionCatalog::resolvePath(PgConnectionCatalog::explicitPath());

  mConnectionCombo->blockSignals(true);
  mConnectionCombo->clear();
  mConnectionCombo->blockSignals(false);

  mConnected = PgConnection();
  mConnectedPassword.clear();
  mTables.clear();
  mTableTree->clear();
  mSchemaCombo->clear();
  mSchemaCombo->setEnabled(false);
  mFilterEdit->setEnabled(false);
  mAddButton->setEnabled(false);
  mConnectButton->setEnabled(false);
  mServerLabel->clear();

  if (mCatalogPath.isEmpty())
  {
    showStatus(QStringLiteral("Selecciona el archivo Connections.xml de Mapa Digital con "
                              "«Examinar…»."),
               true);
    return;
  }

  Error error;
  if (!PgConnectionCatalog::load(mCatalogPath, &mCatalog, &error))
  {
    showStatus(error.toText(), true);
    return;
  }

  for (const PgConnection &connection : mCatalog.connections())
    mConnectionCombo->addItem(connection.displayName(), connection.name);

  mConnectButton->setEnabled(true);
  showStatus(QStringLiteral("%1 conexión(es) PostGIS en %2")
               .arg(mCatalog.connections().size())
               .arg(mCatalogPath),
             false);
  updateConnectionInfo();
}

void PostgisDialog::browseCatalog()
{
  const QString start = mCatalogPath.isEmpty() ? QString() : mCatalogPath;
  const QString path = QFileDialog::getOpenFileName(
    this, QStringLiteral("Catálogo de conexiones de Mapa Digital"), start,
    QStringLiteral("Connections (*.xml);;Todos los archivos (*)"));
  if (path.isEmpty())
    return;

  mCatalogPath = path;
  loadCatalog();

  if (!mCatalog.connections().isEmpty())
  {
    QSettings settings;
    settings.setValue(QStringLiteral("postgis/xmlPath"), path);
  }
}

void PostgisDialog::updateConnectionInfo()
{
  const PgConnection *connection = currentConnection();
  if (!connection)
  {
    mServerLabel->clear();
    return;
  }

  mServerLabel->setText(QStringLiteral("%1 · %2:%3/%4 · usuario %5")
                          .arg(connection->name, connection->server,
                               QString::number(connection->port), connection->database,
                               connection->user));
}

const PgConnection *PostgisDialog::currentConnection() const
{
  if (mConnectionCombo->count() == 0)
    return nullptr;
  const QString name = mConnectionCombo->currentData().toString();
  return mCatalog.find(name);
}

void PostgisDialog::connectToServer()
{
  const PgConnection *connection = currentConnection();
  if (!connection)
  {
    showStatus(QStringLiteral("Elige una conexión antes de conectar."), true);
    return;
  }

  QString password = mPasswordEdit->text();
  if (password.isEmpty())
    password = PgCredentials::instance().passwordFor(*connection);
  if (password.isEmpty())
  {
    showStatus(QStringLiteral("Escribe la contraseña de «%1».").arg(connection->name), true);
    return;
  }

  setBusy(true);
  QStringList schemas;
  PgFailure failure = PgFailure::None;
  Error error;
  const bool ok = PgCatalog::listSchemas(*connection, password, &schemas, &failure, &error);
  setBusy(false);

  if (!ok)
  {
    showStatus(error.toText(), true);
    return;
  }

  mConnected = *connection;
  mConnectedPassword = password;
  PgSession::instance().activate(*connection, password);

  Error rememberError;
  if (mRememberCheck->isChecked()
      && !PgCredentials::instance().remember(*connection, password, true, &rememberError))
  {
    showStatus(rememberError.toText(), false);
  }
  else
  {
    PgCredentials::instance().remember(*connection, password, false, nullptr);
  }

  mSchemaCombo->blockSignals(true);
  mSchemaCombo->clear();
  for (const QString &schema : schemas)
    mSchemaCombo->addItem(schema);
  mSchemaCombo->blockSignals(false);

  QSettings settings;
  int index = mSchemaCombo->findText(settings.value(QStringLiteral("postgis/lastSchema")).toString());
  if (index < 0)
    index = mSchemaCombo->findText(QStringLiteral("public"));
  if (index < 0 && mSchemaCombo->count() > 0)
    index = 0;
  mSchemaCombo->setCurrentIndex(index);
  PgSession::instance().setSchema(mSchemaCombo->currentText());

  mSchemaCombo->setEnabled(mSchemaCombo->count() > 0);
  mFilterEdit->setEnabled(true);
  mRefreshButton->setEnabled(true);

  showStatus(QStringLiteral("Conectado a %1 · %2 esquemas")
               .arg(connection->displayName())
               .arg(schemas.size()),
             false);

  reloadTables();
}

void PostgisDialog::reloadTables()
{
  if (!mConnected.isValid())
    return;

  mTables.clear();
  mTableTree->clear();
  mAddButton->setEnabled(false);

  const QString schema = mSchemaCombo->currentText();
  if (schema.isEmpty())
    return;

  setBusy(true);
  PgFailure failure = PgFailure::None;
  Error error;
  const bool ok = PgCatalog::listTables(mConnected, mConnectedPassword, schema, &mTables, &failure,
                                        &error);
  setBusy(false);

  if (!ok)
  {
    showStatus(error.toText(), true);
    return;
  }

  fillTables();
  if (PgSession::instance().isActive())
    PgSession::instance().setSchema(schema);
  showStatus(QStringLiteral("%1 tabla(s) con geometría en «%2»").arg(mTables.size()).arg(schema),
             false);
}

void PostgisDialog::fillTables()
{
  for (const PgTableInfo &table : mTables)
  {
    auto *item = new QTreeWidgetItem(mTableTree);
    item->setText(0, table.name);
    item->setText(1, table.geometryKind);
    item->setText(2, QString::number(table.features));
    item->setData(0, Qt::UserRole, table.name);
    item->setToolTip(0, QStringLiteral("%1 · EPSG:%2").arg(table.geometryName).arg(table.srid));
  }
}

void PostgisDialog::applyFilter(const QString &text)
{
  const QString needle = text.trimmed();
  for (int index = 0; index < mTableTree->topLevelItemCount(); ++index)
  {
    QTreeWidgetItem *item = mTableTree->topLevelItem(index);
    item->setHidden(!needle.isEmpty()
                    && !item->text(0).contains(needle, Qt::CaseInsensitive));
  }
  updateAddButton();
}

void PostgisDialog::updateAddButton()
{
  mAddButton->setEnabled(!mBusy && !mTableTree->selectedItems().isEmpty());
}

void PostgisDialog::setBusy(bool busy)
{
  mBusy = busy;
  mConnectionCombo->setEnabled(!busy);
  mBrowseButton->setEnabled(!busy);
  mConnectButton->setEnabled(!busy);
  mSchemaCombo->setEnabled(!busy && mSchemaCombo->count() > 0);
  mFilterEdit->setEnabled(!busy && mConnected.isValid());
  mTableTree->setEnabled(!busy);
  mRefreshButton->setEnabled(!busy && mConnected.isValid());
  updateAddButton();

  if (busy)
    QApplication::setOverrideCursor(Qt::WaitCursor);
  else
    QApplication::restoreOverrideCursor();
}

void PostgisDialog::showStatus(const QString &message, bool error)
{
  mStatusLabel->setText(message);
  mStatusLabel->setStyleSheet(error ? QStringLiteral("color:#b00020;")
                                    : QStringLiteral("color:#333;"));
}

QVector<PgLayerSelection> PostgisDialog::collectSelections()
{
  QVector<PgLayerSelection> result;
  if (!mConnected.isValid())
    return result;

  const QString schema = mSchemaCombo->currentText();
  for (QTreeWidgetItem *item : mTableTree->selectedItems())
  {
    if (!item || item->isHidden())
      continue;

    PgLayerSelection selection;
    selection.connection = mConnected;
    selection.schema = schema;
    selection.table = item->data(0, Qt::UserRole).toString();

    qint64 features = -1;
    for (const PgTableInfo &table : mTables)
    {
      if (table.name == selection.table)
        features = table.features;
    }

    if (features > kBigTableFeatures)
    {
      PgSource source;
      source.connection = selection.connection.name;
      source.schema = selection.schema;
      source.table = selection.table;
      const QString key = source.uri();

      if (!bigTableChoices().contains(key))
      {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("Tabla grande"));
        box.setText(QStringLiteral("«%1» tiene %2 elementos. Preparar el índice de dibujo puede "
                                   "tardar varios minutos.")
                      .arg(selection.table)
                      .arg(features));
        QPushButton *indexButton = box.addButton(QStringLiteral("Preparar índice"),
                                                 QMessageBox::AcceptRole);
        QPushButton *demandButton = box.addButton(QStringLiteral("Dibujar bajo demanda"),
                                                  QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();

        const QAbstractButton *clicked = box.clickedButton();
        if (!clicked || clicked == box.button(QMessageBox::Cancel))
          return QVector<PgLayerSelection>();

        bigTableChoices().insert(key, clicked == indexButton);
        Q_UNUSED(demandButton);
      }

      selection.buildStore = bigTableChoices().value(key);
    }

    result.append(selection);
  }

  return result;
}

void PostgisDialog::restoreSession()
{
  if (!mAutoConnect || mAutoConnectTried || mCatalog.connections().isEmpty())
    return;
  mAutoConnectTried = true;

  PgSession &session = PgSession::instance();
  QString connectionName = session.isActive() ? session.connection().name : QString();
  QString password = session.isActive() ? session.password() : QString();
  QString schema = session.isActive() ? session.schema() : QString();

  if (connectionName.isEmpty())
  {
    QSettings settings;
    connectionName = settings.value(QStringLiteral("postgis/lastConnection")).toString();
    schema = settings.value(QStringLiteral("postgis/lastSchema")).toString();
  }

  const PgConnection *connection = mCatalog.find(connectionName);
  if (!connection)
    return;

  const int index = mConnectionCombo->findData(connection->name);
  if (index >= 0)
  {
    mConnectionCombo->blockSignals(true);
    mConnectionCombo->setCurrentIndex(index);
    mConnectionCombo->blockSignals(false);
    updateConnectionInfo();
  }

  // Sin contrasena recordada solo se preselecciona la conexion: el usuario
  // escribe la clave y pulsa Conectar. Con ella, se conecta solo.
  if (password.isEmpty())
  {
    password = PgCredentials::instance().passwordFor(*connection);
    if (password.isEmpty())
      return;
  }

  mPasswordEdit->setText(password);
  mRememberCheck->setChecked(true);
  connectToServer();

  if (!mConnected.isValid() || schema.isEmpty())
    return;

  const int schemaIndex = mSchemaCombo->findText(schema);
  if (schemaIndex >= 0)
    mSchemaCombo->setCurrentIndex(schemaIndex);
}

void PostgisDialog::accept()
{
  const QVector<PgLayerSelection> chosen = collectSelections();
  if (chosen.isEmpty())
  {
    showStatus(QStringLiteral("Selecciona al menos una tabla."), true);
    return;
  }

  mSelections = chosen;
  PgSession::instance().setSchema(mSchemaCombo->currentText());

  QSettings settings;
  settings.setValue(QStringLiteral("postgis/xmlPath"), mCatalogPath);
  settings.setValue(QStringLiteral("postgis/lastConnection"), mConnected.name);
  settings.setValue(QStringLiteral("postgis/lastSchema"), mSchemaCombo->currentText());

  QDialog::accept();
}

} // namespace minigis
