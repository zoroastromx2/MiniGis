#include "minigisaboutdialog.h"

#include "minigisgdalinit.h"
#include "minigisruntimedlls.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace minigis
{

AboutDialog::AboutDialog(QWidget *parent)
  : QDialog(parent)
{
  setWindowTitle(QStringLiteral("Acerca de MiniGIS"));
  setModal(true);
  resize(820, 640);

  const RuntimeInfo info = initializeGdal();
  const QList<RuntimeDllInfo> dlls = loadedRuntimeDlls();

  auto *title = new QLabel(
    QStringLiteral("<h3>MiniGIS %1</h3>"
                   "<p>Visor SIG mínimo para cargar capas vectoriales y ráster, dibujarlas y "
                   "consultar sus atributos.</p>"
                   "<p>Construido en C++ con Qt 6 y GDAL, siguiendo las convenciones de compilación "
                   "de QGIS (CMake + vcpkg + capas core/gui/app). Los formatos los aporta GDAL/OGR; "
                   "no se reutiliza código de QGIS.</p>")
      .arg(QString::fromUtf8(MINIGIS_VERSION)),
    this);
  title->setTextFormat(Qt::RichText);
  title->setWordWrap(true);

  // Resumen del motor: es lo primero que se mira al comparar dos instalaciones.
  QString components =
    QStringLiteral("<p><b>Qt</b>: %1 &nbsp;&nbsp; <b>GDAL</b>: %2 &nbsp;&nbsp; <b>PROJ</b>: %3")
      .arg(QString::fromUtf8(qVersion()), info.gdalVersion, info.projVersion);

  // Arrow y Parquet no llevan version en el PE: se toma del puerto de vcpkg que
  // instala las DLLs, que es la unica forma de saber que build de Parquet hay.
  const QString arrow = loadedRuntimeDllVersion(QStringLiteral("arrow.dll"));
  const QString parquet = loadedRuntimeDllVersion(QStringLiteral("parquet.dll"));
  if (!arrow.isEmpty())
    components += QStringLiteral(" &nbsp;&nbsp; <b>Arrow</b>: %1").arg(arrow);
  if (!parquet.isEmpty())
    components += QStringLiteral(" &nbsp;&nbsp; <b>Parquet</b>: %1").arg(parquet);
  components += QStringLiteral("</p>");

  components += QStringLiteral("<p><b>Compilador</b>: %1 %2 &nbsp;&nbsp; <b>Configuración</b>: %3</p>")
                  .arg(QString::fromUtf8(MINIGIS_COMPILER_ID),
                       QString::fromUtf8(MINIGIS_COMPILER_VERSION),
                       QString::fromUtf8(MINIGIS_BUILD_TYPE));

  auto *componentsLabel = new QLabel(components, this);
  componentsLabel->setTextFormat(Qt::RichText);
  componentsLabel->setWordWrap(true);

  auto *dllTitle = new QLabel(
    QStringLiteral("<b>DLLs cargadas desde la carpeta del ejecutable</b> (%1)").arg(dlls.size()), this);
  dllTitle->setTextFormat(Qt::RichText);

  auto *dllNote = new QLabel(
    QStringLiteral("La versión sale del recurso del archivo; si no lo lleva (Arrow, Parquet, "
                   "PROJ, GEOS…), del puerto de vcpkg que la instala."),
    this);
  dllNote->setWordWrap(true);

  mDllTable = new QTableWidget(static_cast<int>(dlls.size()), 2, this);
  mDllTable->setHorizontalHeaderLabels({QStringLiteral("DLL"), QStringLiteral("Versión")});
  mDllTable->verticalHeader()->setVisible(false);
  mDllTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  mDllTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  mDllTable->setAlternatingRowColors(true);
  mDllTable->setWordWrap(false);
  mDllTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  mDllTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  mDllTable->setSortingEnabled(false);

  for (int row = 0; row < dlls.size(); ++row)
  {
    const RuntimeDllInfo &dll = dlls.at(row);

    auto *nameItem = new QTableWidgetItem(dll.path);
    QString nameToolTip = dll.path;
    if (!dll.component.isEmpty())
      nameToolTip += QStringLiteral("\nPuerto vcpkg: %1").arg(dll.component);
    nameItem->setToolTip(nameToolTip);
    mDllTable->setItem(row, 0, nameItem);

    const bool known = !dll.version.isEmpty();
    auto *versionItem = new QTableWidgetItem(known ? dll.version : QStringLiteral("(sin versión)"));
    versionItem->setToolTip(known ? dll.version : QStringLiteral("La DLL no lleva recurso de versión"));
    mDllTable->setItem(row, 1, versionItem);
  }

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(title);
  layout->addWidget(componentsLabel);
  layout->addSpacing(6);
  layout->addWidget(dllTitle);
  layout->addWidget(dllNote);
  layout->addWidget(mDllTable, 1);
  layout->addWidget(buttons);
}

} // namespace minigis
