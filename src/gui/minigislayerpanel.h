#pragma once

#include <QWidget>

#include <memory>
#include <vector>

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace minigis
{

class VectorLayer;

/**
 * Panel de capas (arbol + acciones). No guarda estado propio: se reconstruye
 * cada vez que el lienzo avisa de un cambio, para que siempre refleje la
 * realidad del mapa.
 */
class LayerPanel : public QWidget
{
  Q_OBJECT

public:
  enum Column
  {
    ColumnName = 0,
    ColumnGeometry,
    ColumnFeatures,
    ColumnCrs,
    ColumnCount
  };

  explicit LayerPanel(QWidget *parent = nullptr);

  void setLayers(const std::vector<std::shared_ptr<VectorLayer>> &layers,
                 const std::shared_ptr<VectorLayer> &current);

signals:
  void addLayerRequested();
  void removeLayerRequested(const std::shared_ptr<VectorLayer> &layer);
  void zoomToLayerRequested(const std::shared_ptr<VectorLayer> &layer);
  void styleRequested(const std::shared_ptr<VectorLayer> &layer);
  void labelRequested(const std::shared_ptr<VectorLayer> &layer);
  void attributeTableRequested(const std::shared_ptr<VectorLayer> &layer);
  void visibilityChanged(const std::shared_ptr<VectorLayer> &layer, bool visible);
  void currentLayerChanged(const std::shared_ptr<VectorLayer> &layer);

private:
  void buildUi();
  void onItemChanged(QTreeWidgetItem *item, int column);
  void onSelectionChanged();
  void onItemDoubleClicked(QTreeWidgetItem *item, int column);
  std::shared_ptr<VectorLayer> layerForItem(QTreeWidgetItem *item) const;
  std::shared_ptr<VectorLayer> selectedLayer() const;

  QTreeWidget *mTree = nullptr;
  QPushButton *mRemoveButton = nullptr;
  QPushButton *mZoomButton = nullptr;
  QPushButton *mStyleButton = nullptr;
  QPushButton *mLabelButton = nullptr;
  QPushButton *mTableButton = nullptr;

  std::vector<std::shared_ptr<VectorLayer>> mLayers;
  std::shared_ptr<VectorLayer> mCurrent;
  bool mUpdating = false;
};

} // namespace minigis
