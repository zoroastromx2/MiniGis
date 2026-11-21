#include "minigisattributetablemodel.h"

#include <QBrush>
#include <QColor>
#include <QFont>

#include <algorithm>

namespace minigis
{

AttributeTableModel::AttributeTableModel(QObject *parent)
  : QAbstractTableModel(parent)
{
}

void AttributeTableModel::setLayer(const std::shared_ptr<VectorLayer> &layer)
{
  beginResetModel();

  mLayer = layer;
  mFields.clear();
  mRowCount = 0;
  mTotalRows = 0;
  mPages.clear();
  mPageOrder.clear();

  if (mLayer)
  {
    mFields = mLayer->fields();
    mTotalRows = mLayer->featureCount();
    mRowCount = std::min<qint64>(mTotalRows, kMaximumRows);
  }

  endResetModel();
}

int AttributeTableModel::rowCount(const QModelIndex &parent) const
{
  if (parent.isValid())
    return 0;
  return static_cast<int>(mRowCount);
}

int AttributeTableModel::columnCount(const QModelIndex &parent) const
{
  if (parent.isValid())
    return 0;
  return static_cast<int>(mFields.size());
}

QVariant AttributeTableModel::data(const QModelIndex &index, int role) const
{
  if (!mLayer || !index.isValid() || index.row() < 0 || index.row() >= mRowCount)
    return {};

  const int column = index.column();
  if (column < 0 || column >= mFields.size())
    return {};

  switch (role)
  {
    case Qt::DisplayRole:
    {
      const QVector<QVector<QString>> &page = pageFor(index.row());
      const qint64 offset = index.row() % kPageRows;
      if (offset < 0 || offset >= page.size())
        return {};
      const QVector<QString> &values = page.at(static_cast<int>(offset));
      if (column >= values.size())
        return {};

      const QString &value = values.at(column);
      // Los valores nulos se distinguen de las cadenas vacias.
      return value.isEmpty() ? QStringLiteral("(nulo)") : value;
    }

    case Qt::TextAlignmentRole:
      return mFields.at(column).isNumeric ? QVariant(int(Qt::AlignRight | Qt::AlignVCenter))
                                          : QVariant(int(Qt::AlignLeft | Qt::AlignVCenter));

    case Qt::ForegroundRole:
    {
      const QVector<QVector<QString>> &page = pageFor(index.row());
      const qint64 offset = index.row() % kPageRows;
      if (offset >= 0 && offset < page.size())
      {
        const QVector<QString> &values = page.at(static_cast<int>(offset));
        if (column < values.size() && values.at(column).isEmpty())
          return QBrush(QColor(150, 150, 150));
      }
      return {};
    }

    case Qt::ToolTipRole:
      return QStringLiteral("Fila %1, campo «%2»").arg(index.row()).arg(mFields.at(column).name);

    default:
      return {};
  }
}

const QVector<QVector<QString>> &AttributeTableModel::pageFor(qint64 row) const
{
  static const QVector<QVector<QString>> empty;
  if (!mLayer || row < 0 || row >= mRowCount)
    return empty;

  const qint64 pageIndex = row / kPageRows;
  const auto found = mPages.find(pageIndex);
  if (found != mPages.end())
    return found->second;

  const qint64 first = pageIndex * kPageRows;
  const int count = static_cast<int>(std::min<qint64>(kPageRows, mRowCount - first));
  QVector<QVector<QString>> page = mLayer->fieldValuesPage(first, count);

  // Cache FIFO de paginas: al desplazarse se descarta la mas antigua.
  while (mPageOrder.size() >= static_cast<int>(kMaximumPages))
  {
    const qint64 victim = mPageOrder.takeFirst();
    mPages.erase(victim);
  }
  mPageOrder.append(pageIndex);
  const auto inserted = mPages.emplace(pageIndex, std::move(page));
  return inserted.first->second;
}

QVariant AttributeTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation == Qt::Horizontal)
  {
    if (section < 0 || section >= mFields.size())
      return {};

    switch (role)
    {
      case Qt::DisplayRole:
        return mFields.at(section).name;
      case Qt::ToolTipRole:
        return QStringLiteral("%1 (%2, ancho %3)")
          .arg(mFields.at(section).name, mFields.at(section).typeName)
          .arg(mFields.at(section).width);
      default:
        return {};
    }
  }

  if (orientation == Qt::Vertical && role == Qt::DisplayRole)
    return section;

  return {};
}

} // namespace minigis
