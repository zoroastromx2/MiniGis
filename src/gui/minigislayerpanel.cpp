#include "minigislayerpanel.h"

#include "minigisstyle.h"
#include "minigisvectorlayer.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
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

QString formatFeatureCount(qint64 count)
{
  return QLocale().toString(count);
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
  mTree->setHeaderLabels({QStringLiteral("Capa"), QStringLiteral("Geometria"),
                          QStringLiteral("Elementos"), QStringLiteral("SRC")});
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
  addButton->setToolTip(QStringLiteral("Abrir un archivo vectorial (Ctrl+O)"));
  mRemoveButton = new QPushButton(widgetStyle->standardIcon(QStyle::SP_TrashIcon),
                                  QStringLiteral("Quitar"), this);
  mRemoveButton->setToolTip(QStringLiteral("Quitar la capa seleccionada del mapa"));
  mZoomButton = new QPushButton(widgetStyle->standardIcon(QStyle::SP_FileDialogContentsView),
                                QStringLiteral("Zoom"), this);
  mZoomButton->setToolTip(QStringLiteral("Encuadrar la capa seleccionada"));
  mStyleButton = new QPushButton(QStringLiteral("Estilo"), this);
  mStyleButton->setToolTip(QStringLiteral("Cambiar el color y el grosor de la capa seleccionada"));
  mTableButton = new QPushButton(QStringLiteral("Tabla"), this);
  mTableButton->setToolTip(QStringLiteral("Ver la tabla de atributos de la capa seleccionada (F6)"));

  auto *buttonsRow = new QHBoxLayout;
  buttonsRow->setContentsMargins(0, 0, 0, 0);
  buttonsRow->addWidget(addButton);
  buttonsRow->addWidget(mRemoveButton);
  buttonsRow->addWidget(mZoomButton);
  buttonsRow->addWidget(mStyleButton);
  buttonsRow->addWidget(mTableButton);
  buttonsRow->addStretch(1);

  auto *emptyHint = new QLabel(
    QStringLiteral("Cada fila es una capa cargada. La casilla controla su visibilidad."), this);
  emptyHint->setWordWrap(true);
  emptyHint->setStyleSheet(QStringLiteral("color: palette(mid);"));

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->addWidget(mTree, 1);
  layout->addLayout(buttonsRow);
  layout->addWidget(emptyHint);

  connect(addButton, &QPushButton::clicked, this, &LayerPanel::addLayerRequested);
  connect(mRemoveButton, &QPushButton::clicked, this, [this] {
    if (const std::shared_ptr<VectorLayer> layer = selectedLayer())
      emit removeLayerRequested(layer);
  });
  connect(mZoomButton, &QPushButton::clicked, this, [this] {
    if (const std::shared_ptr<VectorLayer> layer = selectedLayer())
      emit zoomToLayerRequested(layer);
  });
  connect(mStyleButton, &QPushButton::clicked, this, [this] {
    if (const std::shared_ptr<VectorLayer> layer = selectedLayer())
      emit styleRequested(layer);
  });
  connect(mTableButton, &QPushButton::clicked, this, [this] {
    if (const std::shared_ptr<VectorLayer> layer = selectedLayer())
      emit attributeTableRequested(layer);
  });
}

void LayerPanel::setLayers(const std::vector<std::shared_ptr<VectorLayer>> &layers,
                           const std::shared_ptr<VectorLayer> &current)
{
  mLayers = layers;
  mCurrent = current;

  mUpdating = true;
  mTree->clear();

  for (size_t i = 0; i < mLayers.size(); ++i)
  {
    const std::shared_ptr<VectorLayer> &layer = mLayers[i];
    if (!layer)
      continue;

    auto *item = new QTreeWidgetItem(mTree);
    item->setText(ColumnName, layer->name());
    item->setToolTip(ColumnName, layer->sourcePath());
    item->setText(ColumnGeometry, Style::kindName(layer->geometryKind()));
    item->setToolTip(ColumnGeometry, layer->geometryTypeName());
    item->setText(ColumnFeatures, formatFeatureCount(layer->featureCount()));
    item->setText(ColumnCrs, layer->crsDescription());
    item->setCheckState(ColumnName, layer->isVisible() ? Qt::Checked : Qt::Unchecked);
    item->setData(ColumnName, kLayerIndexRole, static_cast<int>(i));

    if (layer == mCurrent)
      mTree->setCurrentItem(item);
  }

  const bool hasSelection = mTree->currentItem() != nullptr;
  mRemoveButton->setEnabled(hasSelection);
  mZoomButton->setEnabled(hasSelection);
  mStyleButton->setEnabled(hasSelection);
  mTableButton->setEnabled(hasSelection);

  mUpdating = false;
}

std::shared_ptr<VectorLayer> LayerPanel::layerForItem(QTreeWidgetItem *item) const
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

std::shared_ptr<VectorLayer> LayerPanel::selectedLayer() const
{
  return layerForItem(mTree->currentItem());
}

void LayerPanel::onItemChanged(QTreeWidgetItem *item, int column)
{
  if (mUpdating || column != ColumnName)
    return;

  const std::shared_ptr<VectorLayer> layer = layerForItem(item);
  if (!layer)
    return;

  emit visibilityChanged(layer, item->checkState(ColumnName) == Qt::Checked);
}

void LayerPanel::onSelectionChanged()
{
  if (mUpdating)
    return;

  const bool hasSelection = mTree->currentItem() != nullptr;
  mRemoveButton->setEnabled(hasSelection);
  mZoomButton->setEnabled(hasSelection);
  mStyleButton->setEnabled(hasSelection);
  mTableButton->setEnabled(hasSelection);

  if (const std::shared_ptr<VectorLayer> layer = selectedLayer())
    emit currentLayerChanged(layer);
}

void LayerPanel::onItemDoubleClicked(QTreeWidgetItem *item, int /*column*/)
{
  if (const std::shared_ptr<VectorLayer> layer = layerForItem(item))
    emit zoomToLayerRequested(layer);
}

} // namespace minigis
