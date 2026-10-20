#pragma once

#include <QMetaType>
#include <QWidget>

#include <memory>
#include <vector>

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace minigis
{

class RasterLayer;
class VectorLayer;

/**
 * Una fila del panel de capas: vectorial o raster, con los textos ya
 * preparados para mostrar. Las dos clases comparten tabla porque el usuario ve
 * un solo mapa; el orden de las filas es el de apilado (primera fila = fondo).
 */
struct LayerEntry
{
  enum class Kind
  {
    Vector,
    Raster
  };

  Kind kind = Kind::Vector;
  std::shared_ptr<VectorLayer> vector;
  std::shared_ptr<RasterLayer> raster;
  QString name;
  QString sourcePath;
  QString kindText;
  QString countText;
  QString crsText;
  bool visible = true;
};

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

  void setLayers(const std::vector<LayerEntry> &layers, const LayerEntry *current);

signals:
  void addLayerRequested();
  void removeLayerRequested(const LayerEntry &entry);
  void zoomToLayerRequested(const LayerEntry &entry);
  void styleRequested(const LayerEntry &entry);
  void labelRequested(const LayerEntry &entry);
  void attributeTableRequested(const LayerEntry &entry);
  void visibilityChanged(const LayerEntry &entry, bool visible);
  void currentLayerChanged(const LayerEntry &entry);

private:
  void buildUi();
  void onItemChanged(QTreeWidgetItem *item, int column);
  void onSelectionChanged();
  void onItemDoubleClicked(QTreeWidgetItem *item, int column);
  LayerEntry entryForItem(QTreeWidgetItem *item) const;
  void updateButtons();

  QTreeWidget *mTree = nullptr;
  QPushButton *mRemoveButton = nullptr;
  QPushButton *mZoomButton = nullptr;
  QPushButton *mStyleButton = nullptr;
  QPushButton *mLabelButton = nullptr;
  QPushButton *mTableButton = nullptr;

  std::vector<LayerEntry> mLayers;
  bool mUpdating = false;
};

} // namespace minigis

Q_DECLARE_METATYPE(minigis::LayerEntry)
