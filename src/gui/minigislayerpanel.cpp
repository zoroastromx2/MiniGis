#include "minigislayerpanel.h"

#include "minigisrasterlayer.h"
#include "minigisvectorlayer.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace minigis
{
namespace
{

constexpr int kLayerIndexRole = Qt::UserRole + 1;

bool sameEntry(const LayerEntry &left, const LayerEntry &right)
{
  if (left.kind != right.kind)
    return false;
  return left.kind == LayerEntry::Kind::Vector ? left.vector == right.vector : left.raster == right.raster;
}

} // namespace

LayerPanel::LayerPanel(QWidget *parent)
  : QWidget(parent)
{
  buildUi();
}

void LayerPanel::buildUi()
{
  mTree = new QTreeWidget(this);
  mTree->setColumnCount(ColumnCount);
  mTree->setHeaderLabels({QStringLiteral("Capa"), QStringLiteral("Tipo"),
                          QStringLiteral("Tamano"), QStringLiteral("SRC")});
  mTree->setRootIsDecorated(false);
  mTree->setAlternatingRowColors(true);
  mTree->setSelectionMode(QAbstractItemView::SingleSelection);
  mTree->setSelectionBehavior(QAbstractItemView::SelectRows);
  mTree->header()->setSectionResizeMode(ColumnName, QHeaderView::Stretch);
  mTree->header()->setSectionResizeMode(ColumnGeometry, QHeaderView::ResizeToContents);
  mTree->header()->setSectionResizeMode(ColumnFeatures, QHeaderView::ResizeToContents);
  mTree->header()->setSectionResizeMode(ColumnCrs, QHeaderView::ResizeToContents);

  connect(mTree, &QTreeWidget::itemChanged, this, &LayerPanel::onItemChanged);
  connect(mTree, &QTreeWidget::itemSelectionChanged, this, &LayerPanel::onSelectionChanged);
  connect(mTree, &QTreeWidget::itemDoubleClicked, this, &LayerPanel::onItemDoubleClicked);

  const QStyle *widgetStyle = style();

  auto *addButton = new QPushButton(widgetStyle->standardIcon(QStyle::SP_DialogOpenButton),
                                    QStringLiteral("Abrir"), this);
  addButton->setToolTip(QStringLiteral("Abrir una capa vectorial o raster (Ctrl+O)"));
  mRemoveButton = new QPushButton(widgetStyle->standardIcon(QStyle::SP_TrashIcon),
                                  QStringLiteral("Quitar"), this);
  mRemoveButton->setToolTip(QStringLiteral("Quitar la capa seleccionada del mapa"));
  mZoomButton = new QPushButton(widgetStyle->standardIcon(QStyle::SP_FileDialogContentsView),
                                QStringLiteral("Zoom"), this);
  mZoomButton->setToolTip(QStringLiteral("Encuadrar la capa seleccionada"));
  mStyleButton = new QPushButton(QStringLiteral("Estilo"), this);
  mStyleButton->setToolTip(QStringLiteral("Cambiar la simbologia (o el contraste) de la capa seleccionada"));
  mLabelButton = new QPushButton(QStringLiteral("Etiquetas"), this);
  mLabelButton->setToolTip(QStringLiteral("Configurar las etiquetas de la capa vectorial (F7)"));
  mTableButton = new QPushButton(QStringLiteral("Tabla"), this);
  mTableButton->setToolTip(QStringLiteral("Ver la tabla de atributos de la capa vectorial (F6)"));

  auto *buttonsRow = new QHBoxLayout;
  buttonsRow->setContentsMargins(0, 0, 0, 0);
  buttonsRow->addWidget(addButton);
  buttonsRow->addWidget(mRemoveButton);
  buttonsRow->addWidget(mZoomButton);
  buttonsRow->addWidget(mStyleButton);
  buttonsRow->addWidget(mLabelButton);
  buttonsRow->addWidget(mTableButton);
  buttonsRow->addStretch(1);

  auto *emptyHint = new QLabel(
    QStringLiteral("Cada fila es una capa cargada. La casilla controla su visibilidad; "
                   "el raster se dibuja debajo del vectorial."),
    this);
  emptyHint->setWordWrap(true);
  emptyHint->setStyleSheet(QStringLiteral("color: palette(mid);"));

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->addWidget(mTree, 1);
  layout->addLayout(buttonsRow);
  layout->addWidget(emptyHint);

  connect(addButton, &QPushButton::clicked, this, &LayerPanel::addLayerRequested);
  connect(mRemoveButton, &QPushButton::clicked, this, [this] {
    const LayerEntry entry = entryForItem(mTree->currentItem());
    if (entry.vector || entry.raster)
      emit removeLayerRequested(entry);
  });
  connect(mZoomButton, &QPushButton::clicked, this, [this] {
    const LayerEntry entry = entryForItem(mTree->currentItem());
    if (entry.vector || entry.raster)
      emit zoomToLayerRequested(entry);
  });
  connect(mStyleButton, &QPushButton::clicked, this, [this] {
    const LayerEntry entry = entryForItem(mTree->currentItem());
    if (entry.vector || entry.raster)
      emit styleRequested(entry);
  });
  connect(mLabelButton, &QPushButton::clicked, this, [this] {
    const LayerEntry entry = entryForItem(mTree->currentItem());
    if (entry.vector)
      emit labelRequested(entry);
  });
  connect(mTableButton, &QPushButton::clicked, this, [this] {
    const LayerEntry entry = entryForItem(mTree->currentItem());
    if (entry.vector)
      emit attributeTableRequested(entry);
  });
}

void LayerPanel::setLayers(const std::vector<LayerEntry> &layers, const LayerEntry *current)
{
  mLayers = layers;

  mUpdating = true;
  mTree->clear();

  for (size_t index = 0; index < mLayers.size(); ++index)
  {
    const LayerEntry &entry = mLayers[index];

    auto *item = new QTreeWidgetItem(mTree);
    item->setText(ColumnName, entry.name);
    item->setToolTip(ColumnName, entry.sourcePath);
    item->setText(ColumnGeometry, entry.kindText);
    item->setText(ColumnFeatures, entry.countText);
    item->setText(ColumnCrs, entry.crsText);
    item->setCheckState(ColumnName, entry.visible ? Qt::Checked : Qt::Unchecked);
    item->setData(ColumnName, kLayerIndexRole, static_cast<int>(index));

    if (current && sameEntry(entry, *current))
      mTree->setCurrentItem(item);
  }

  mUpdating = false;
  updateButtons();
}

void LayerPanel::updateButtons()
{
  const LayerEntry entry = entryForItem(mTree->currentItem());
  const bool hasSelection = entry.vector != nullptr || entry.raster != nullptr;
  const bool isVector = entry.kind == LayerEntry::Kind::Vector && entry.vector != nullptr;

  mRemoveButton->setEnabled(hasSelection);
  mZoomButton->setEnabled(hasSelection);
  mStyleButton->setEnabled(hasSelection);
  mLabelButton->setEnabled(isVector);
  mTableButton->setEnabled(isVector);
}

LayerEntry LayerPanel::entryForItem(QTreeWidgetItem *item) const
{
  if (!item)
    return {};

  const QVariant indexValue = item->data(ColumnName, kLayerIndexRole);
  if (!indexValue.isValid())
    return {};

  const int index = indexValue.toInt();
  if (index < 0 || index >= static_cast<int>(mLayers.size()))
    return {};

  return mLayers[static_cast<size_t>(index)];
}

void LayerPanel::onItemChanged(QTreeWidgetItem *item, int column)
{
  if (mUpdating || column != ColumnName)
    return;

  const LayerEntry entry = entryForItem(item);
  if (entry.vector || entry.raster)
    emit visibilityChanged(entry, item->checkState(ColumnName) == Qt::Checked);
}

void LayerPanel::onSelectionChanged()
{
  if (mUpdating)
    return;

  updateButtons();

  const LayerEntry entry = entryForItem(mTree->currentItem());
  if (entry.vector || entry.raster)
    emit currentLayerChanged(entry);
}

void LayerPanel::onItemDoubleClicked(QTreeWidgetItem *item, int /*column*/)
{
  const LayerEntry entry = entryForItem(item);
  if (entry.vector || entry.raster)
    emit zoomToLayerRequested(entry);
}

} // namespace minigis
