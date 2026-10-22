#pragma once

#include <QMetaType>
#include <QSet>
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
  /**
   * Nombre visible del archivo que contiene la capa. Solo lo llevan las capas de
   * un archivo con varias (un GeoPackage): las de un archivo de una sola capa lo
   * dejan vacio y se muestran como filas sueltas. Las filas con el mismo
   * `sourcePath` se agrupan bajo este nombre.
   */
  QString groupName;
  QString kindText;
  QString countText;
  QString crsText;
  bool visible = true;
};

/**
 * Panel de capas (arbol + acciones). No guarda estado propio: se reconstruye
 * cada vez que el lienzo avisa de un cambio, para que siempre refleje la
 * realidad del mapa.
 *
 * Un archivo con varias capas (un GeoPackage) se muestra como un item con el
 * nombre del archivo y una capa por subitem. Es lo que hace visible que el
 * archivo se abrio entero y permite distinguir capas que se llaman igual en
 * archivos distintos.
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

  /**
   * Una fila del arbol tal como la ve el usuario. La usa la comprobacion sin
   * ventana (`--check-layers`) para verificar que se muestran todas las capas y
   * agrupadas bajo su archivo.
   */
  struct TreeRow
  {
    QString name;
    int depth = 0;
    bool isGroup = false;
  };

  explicit LayerPanel(QWidget *parent = nullptr);

  void setLayers(const std::vector<LayerEntry> &layers, const LayerEntry *current);

  /** Filas del arbol en orden, con la profundidad y las capas de cada grupo. */
  std::vector<TreeRow> treeRows() const;

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
  void onGroupExpansionChanged(QTreeWidgetItem *item, bool expanded);
  void addLayerRow(QTreeWidgetItem *parent, size_t index, const LayerEntry *current);
  LayerEntry entryForItem(QTreeWidgetItem *item) const;
  void updateButtons();

  QTreeWidget *mTree = nullptr;
  QPushButton *mRemoveButton = nullptr;
  QPushButton *mZoomButton = nullptr;
  QPushButton *mStyleButton = nullptr;
  QPushButton *mLabelButton = nullptr;
  QPushButton *mTableButton = nullptr;

  std::vector<LayerEntry> mLayers;

  /**
   * Grupos que el usuario ha plegado. Se recuerda la ruta del archivo (no el
   * item) porque el arbol se reconstruye en cada cambio del mapa; sin esto,
   * marcar una casilla volveria a desplegar todos los grupos.
   */
  QSet<QString> mCollapsedGroups;
  bool mUpdating = false;
};

} // namespace minigis

Q_DECLARE_METATYPE(minigis::LayerEntry)
