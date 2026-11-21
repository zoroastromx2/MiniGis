#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QVector>

#include <map>
#include <memory>

#include "minigisvectorlayer.h"

namespace minigis
{

/**
 * Modelo de la tabla de atributos de una capa.
 *
 * Los valores se leen por paginas contiguas y se cachean: pedir las filas de una
 * en una (como hacia antes) cuesta milisegundos por fila en Parquet, y una
 * pagina entera se lee en una sola pasada secuencial.
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
  /** Filas por pagina leida del origen. */
  static constexpr qint64 kPageRows = 128;
  /** Paginas que se mantienen en memoria (las visibles y el salto anterior). */
  static constexpr size_t kMaximumPages = 4;

  /** Pagina que contiene `row`, cargandola si hace falta. */
  const QVector<QVector<QString>> &pageFor(qint64 row) const;

  std::shared_ptr<VectorLayer> mLayer;
  QVector<VectorLayer::FieldInfo> mFields;
  qint64 mRowCount = 0;
  qint64 mTotalRows = 0;

  mutable std::map<qint64, QVector<QVector<QString>>> mPages;
  mutable QList<qint64> mPageOrder;
};

} // namespace minigis
