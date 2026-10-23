#include "minigisidentifypanel.h"

#include "minigisstyle.h"
#include "minigisvectorlayer.h"

#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace minigis
{
namespace
{

QString geometryKindName(GeometryKind kind)
{
  switch (kind)
  {
    case GeometryKind::Point:
      return QStringLiteral("Punto");
    case GeometryKind::Line:
      return QStringLiteral("Línea");
    case GeometryKind::Polygon:
      return QStringLiteral("Polígono");
    default:
      return QStringLiteral("Elemento");
  }
}

} // namespace

IdentifyPanel::IdentifyPanel(QWidget *parent)
  : QWidget(parent)
{
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);

  mHeader = new QLabel(this);
  mHeader->setWordWrap(true);
  mHeader->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(mHeader);

  mSearchAll = new QCheckBox(QStringLiteral("Buscar en todas las capas"), this);
  mSearchAll->setToolTip(QStringLiteral("Amplía la consulta a todas las capas visibles "
                                        "aunque haya una seleccionada"));
  layout->addWidget(mSearchAll);

  mTree = new QTreeWidget(this);
  mTree->setColumnCount(2);
  mTree->setHeaderLabels({QStringLiteral("Campo"), QStringLiteral("Valor")});
  mTree->setRootIsDecorated(true);
  mTree->setAlternatingRowColors(true);
  mTree->setUniformRowHeights(true);
  // La columna de nombres no puede comerse el valor: se fija un ancho comodo y
  // el valor ocupa el resto.
  mTree->header()->setStretchLastSection(true);
  mTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
  mTree->setColumnWidth(0, 170);
  layout->addWidget(mTree, 1);

  // Con el dock estrecho, los nombres de campo y el alcance se cortarian.
  setMinimumWidth(280);

  auto *buttons = new QHBoxLayout();
  buttons->addStretch(1);
  mCopyButton = new QPushButton(QStringLiteral("Copiar"), this);
  mCopyButton->setToolTip(QStringLiteral("Copia los resultados al portapapeles"));
  buttons->addWidget(mCopyButton);
  layout->addLayout(buttons);

  connect(mCopyButton, &QPushButton::clicked, this, &IdentifyPanel::copyToClipboard);
  connect(mSearchAll, &QCheckBox::toggled, this, &IdentifyPanel::searchAllLayersChanged);
}

void IdentifyPanel::setResult(const View &view)
{
  mRows.clear();
  mTree->clear();

  const double unitsPerPixel =
    view.radiusPixels > 0 ? view.result.tolerance / static_cast<double>(view.radiusPixels) : 0.0;

  QString header = QStringLiteral("x: %1  y: %2  ·  radio %3 px (%4 u.m.)")
                     .arg(QString::number(view.result.mapPoint.x(), 'f', 6))
                     .arg(QString::number(view.result.mapPoint.y(), 'f', 6))
                     .arg(view.radiusPixels)
                     .arg(QString::number(view.result.tolerance, 'g', 4));
  if (!view.scopeText.isEmpty())
    header += QStringLiteral("  ·  ") + view.scopeText;
  mHeader->setText(header);

  // Agrupar los resultados por capa conservando el orden del motor.
  struct Group
  {
    std::shared_ptr<VectorLayer> layer;
    QVector<int> indices;
  };
  QVector<Group> groups;

  for (int i = 0; i < view.result.hits.size(); ++i)
  {
    const IdentifyHit &hit = view.result.hits.at(i);
    if (!hit.layer)
      continue;

    if (groups.isEmpty() || groups.last().layer != hit.layer)
      groups.append(Group {hit.layer, QVector<int>()});
    groups.last().indices.append(i);
  }

  for (const Group &group : groups)
  {
    const std::shared_ptr<VectorLayer> &layer = group.layer;

    QString layerLabel = layer->name();
    if (layer->layerCount() > 1)
      layerLabel += QStringLiteral(" · ") + layer->containerName();
    if (view.priorityLayer == layer)
      layerLabel += QStringLiteral(" (seleccionada)");

    auto *layerItem = new QTreeWidgetItem(mTree);
    layerItem->setText(0, layerLabel);
    layerItem->setText(1, QString::number(group.indices.size()));
    layerItem->setExpanded(true);
    mRows.push_back(Row {RowKind::Layer, layerLabel, QString::number(group.indices.size()), 0});

    for (const int index : group.indices)
    {
      const IdentifyHit &hit = view.result.hits.at(index);

      QString where;
      if (hit.inside)
        where = QStringLiteral("dentro");
      else if (unitsPerPixel > 0.0)
        where = QStringLiteral("a %1 px").arg(QString::number(hit.distance / unitsPerPixel, 'f', 1));
      else
        where = QStringLiteral("a %1 u.m.").arg(QString::number(hit.distance, 'g', 4));

      const QString featureLabel = QStringLiteral("#%1 · %2 · %3")
                                     .arg(hit.fid)
                                     .arg(geometryKindName(layer->geometryKind()), where);

      auto *featureItem = new QTreeWidgetItem(layerItem);
      featureItem->setText(0, featureLabel);
      featureItem->setExpanded(true);
      mRows.push_back(Row {RowKind::Feature, featureLabel, QString(), 1});

      const QVector<VectorLayer::FieldInfo> &fields = layer->fields();
      for (int field = 0; field < fields.size() && field < hit.values.size(); ++field)
      {
        const QString name = fields.at(field).name;
        QString value = hit.values.at(field);
        if (value.isEmpty())
          value = QStringLiteral("(nulo)");

        auto *fieldItem = new QTreeWidgetItem(featureItem);
        fieldItem->setText(0, name);
        fieldItem->setText(1, value);
        mRows.push_back(Row {RowKind::Field, name, value, 2});
      }
    }
  }

  // Avisos del llamador y los que dependen del resultado.
  QStringList notes = view.notes;
  if (view.result.truncated)
  {
    notes << QStringLiteral("Se muestran los primeros %1 resultados; hay mas.")
               .arg(view.result.hits.size());
  }
  if (view.result.stats.budgetExceeded)
  {
    notes << QStringLiteral("La busqueda se corto por tiempo; acerca la vista o selecciona una capa.");
  }
  if (view.result.hits.isEmpty() && notes.isEmpty())
    notes << QStringLiteral("Sin resultados.");

  for (const QString &note : notes)
  {
    auto *noteItem = new QTreeWidgetItem(mTree);
    noteItem->setText(0, note);
    mRows.push_back(Row {RowKind::Note, note, QString(), 0});
  }

  mTree->expandAll();
}

void IdentifyPanel::clear()
{
  mRows.clear();
  mTree->clear();
  mHeader->clear();
}

QString IdentifyPanel::headerText() const
{
  return mHeader ? mHeader->text() : QString();
}

void IdentifyPanel::setSearchAllLayers(bool all)
{
  if (!mSearchAll)
    return;

  const QSignalBlocker blocker(mSearchAll);
  mSearchAll->setChecked(all);
}

bool IdentifyPanel::searchAllLayers() const
{
  return mSearchAll && mSearchAll->isChecked();
}

void IdentifyPanel::copyToClipboard()
{
  QStringList lines;
  lines.reserve(static_cast<int>(mRows.size()));
  for (const Row &row : mRows)
  {
    QString line = QString(row.depth * 2, QLatin1Char(' ')) + row.label;
    if (!row.value.isEmpty())
      line += QStringLiteral(": ") + row.value;
    lines << line;
  }

  if (QClipboard *clipboard = QGuiApplication::clipboard())
    clipboard->setText(lines.join(QLatin1Char('\n')));
}

} // namespace minigis
