#pragma once

#include "minigisidentify.h"

#include <QStringList>
#include <QWidget>

#include <memory>
#include <vector>

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace minigis
{

class VectorLayer;

/**
 * Panel (dock) con los resultados de Identificar.
 *
 * Presenta un grupo por capa, un elemento por resultado y una fila por campo.
 * No lee datos: recibe el `IdentifyResult` ya resuelto por el motor y solo lo
 * pinta. `rows()` y `headerText()` existen para que el contrato sin ventana
 * (`--check-identify`) pueda comprobar el contenido sin comparar pixeles.
 */
class IdentifyPanel : public QWidget
{
  Q_OBJECT

public:
  enum class RowKind
  {
    Layer,
    Feature,
    Field,
    Note
  };

  /** Una fila logica del panel, en orden de lectura. */
  struct Row
  {
    RowKind kind = RowKind::Note;
    QString label;
    QString value;
    int depth = 0;
  };

  /** Lo que el panel necesita para pintar una consulta. */
  struct View
  {
    IdentifyResult result;
    int radiusPixels = 6;
    QString scopeText;
    /** Avisos del llamador (capa oculta, raster seleccionado, sin resultados). */
    QStringList notes;
    std::shared_ptr<VectorLayer> priorityLayer;
    /** Elemento seleccionado (el primero del resultado), para marcarlo. */
    std::shared_ptr<VectorLayer> selectedLayer;
    qint64 selectedFid = -1;
  };

  explicit IdentifyPanel(QWidget *parent = nullptr);

  void setResult(const View &view);
  void clear();

  QString headerText() const;
  std::vector<Row> rows() const { return mRows; }

  void setSearchAllLayers(bool all);
  bool searchAllLayers() const;

signals:
  void searchAllLayersChanged(bool all);

private:
  void copyToClipboard();

  QLabel *mHeader = nullptr;
  QCheckBox *mSearchAll = nullptr;
  QTreeWidget *mTree = nullptr;
  QPushButton *mCopyButton = nullptr;
  std::vector<Row> mRows;
};

} // namespace minigis
