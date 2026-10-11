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
  mCachedRow = -1;
  mCachedValues.clear();

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
      if (mCachedRow != index.row())
      {
        mCachedValues = mLayer->fieldValues(index.row());
        mCachedRow = index.row();
      }

      if (column >= mCachedValues.size())
        return {};

      const QString &value = mCachedValues.at(column);
      // Los valores nulos se distinguen de las cadenas vacias.
      return value.isEmpty() ? QStringLiteral("(nulo)") : value;
    }

    case Qt::TextAlignmentRole:
      return mFields.at(column).isNumeric ? QVariant(int(Qt::AlignRight | Qt::AlignVCenter))
                                          : QVariant(int(Qt::AlignLeft | Qt::AlignVCenter));

    case Qt::ForegroundRole:
      if (mCachedRow != index.row())
      {
        mCachedValues = mLayer->fieldValues(index.row());
        mCachedRow = index.row();
      }
      if (column < mCachedValues.size() && mCachedValues.at(column).isEmpty())
        return QBrush(QColor(150, 150, 150));
      return {};

    case Qt::ToolTipRole:
      return QStringLiteral("Fila %1, campo «%2»").arg(index.row()).arg(mFields.at(column).name);

    default:
      return {};
  }
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
