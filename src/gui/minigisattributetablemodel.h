#pragma once

#include <QAbstractTableModel>
#include <QVector>

#include <memory>

#include "minigisvectorlayer.h"

namespace minigis
{

/**
 * Modelo de la tabla de atributos de una capa.
 *
 * Se leen los valores bajo demanda (una sola lectura de GDAL por fila) y se
 * guarda en cache la ultima fila consultada, que es justo lo que pide la vista
 * al pintar.
 */
class AttributeTableModel : public QAbstractTableModel
{
  Q_OBJECT

public:
  explicit AttributeTableModel(QObject *parent = nullptr);

  void setLayer(const std::shared_ptr<VectorLayer> &layer);
  std::shared_ptr<VectorLayer> layer() const { return mLayer; }

  /** Numero de filas totales de la capa (puede superar las mostradas). */
  qint64 totalRows() const { return mTotalRows; }
  bool isTruncated() const { return mTotalRows > mRowCount; }

  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  int columnCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
  /** Limite de filas mostradas: por encima, la tabla seria inmanejable. */
  static constexpr qint64 kMaximumRows = 100000;

  std::shared_ptr<VectorLayer> mLayer;
  QVector<VectorLayer::FieldInfo> mFields;
  qint64 mRowCount = 0;
  qint64 mTotalRows = 0;

  mutable qint64 mCachedRow = -1;
  mutable QVector<QString> mCachedValues;
};

} // namespace minigis
