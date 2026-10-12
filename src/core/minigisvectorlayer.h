#pragma once

#include <QString>
#include <QVector>

#include <memory>

#include "minigiserror.h"
#include "minigisrectangle.h"
#include "minigisstyle.h"

#include <gdal.h>
#include <ogr_api.h>

class OGRSpatialReference;

namespace minigis
{

class GeometryStore;

/**
 * Capa vectorial abierta con GDAL/OGR (equivalente minimo a QgsVectorLayer).
 *
 * Es de solo lectura y es dueña del dataset de GDAL, que se cierra en el
 * destructor. Toda la interaccion con GDAL se hace a traves de la API C de
 * OGR, que es la mas estable entre versiones de la biblioteca.
 */
class VectorLayer
{
public:
  struct FieldInfo
  {
    QString name;
    QString typeName;
    int width = 0;
    bool isNumeric = false;
  };

  ~VectorLayer();
  VectorLayer(const VectorLayer &) = delete;
  VectorLayer &operator=(const VectorLayer &) = delete;

  /**
   * Abre una capa vectorial. En caso de fallo devuelve nullptr y rellena
   * `error` con la causa y la accion sugerida.
   */
  static std::shared_ptr<VectorLayer> open(const QString &path, Error *error = nullptr);

  QString sourcePath() const { return mSourcePath; }
  QString name() const { return mName; }
  QString driverName() const { return mDriverName; }
  QString geometryTypeName() const { return mGeometryTypeName; }
  GeometryKind geometryKind() const { return mKind; }

  qint64 featureCount() const;
  const QVector<FieldInfo> &fields() const { return mFields; }
  Rectangle extent() const { return mExtent; }

  /**
   * Extension de la capa expresada en el SRC indicado (por ejemplo el SRC de
   * destino del lienzo). Si la capa ya esta en ese SRC se devuelve tal cual; si
   * no, se transforman las esquinas con GDAL/PROJ.
   */
  Rectangle extentInEpsg(int destinationEpsg) const;

  QString crsDescription() const;
  int crsEpsg() const;
  QString crsWkt() const;
  const OGRSpatialReference *spatialRef() const; // prestado, puede ser nullptr

  /** Valor del campo `fieldIndex` en la fila `row`, ya formateado; vacio si es nulo. */
  QString fieldValue(qint64 row, int fieldIndex) const;

  /**
   * Envolvente de un elemento leida con GDAL. Sirve para comprobar que el indice
   * de envolventes de ShapefileIndex apunta al mismo elemento que el FID de OGR
   * (es la verificacion que se hace en el diagnostico --index-shp).
   */
  Rectangle featureEnvelope(qint64 row) const;

  /**
   * Valores de todos los campos de una fila. Hace una sola lectura en GDAL,
   * por lo que es la forma recomendada para pintar la tabla de atributos.
   */
  QVector<QString> fieldValues(qint64 row) const;

  bool isVisible() const { return mVisible; }
  void setVisible(bool visible) { mVisible = visible; }

  const Style &style() const { return mStyle; }
  void setStyle(const Style &style) { mStyle = style; }

  /** Handle de la capa OGR (prestado) para recorrer elementos al renderizar. */
  OGRLayerH ogrLayerHandle() const { return mLayerHandle; }

  /**
   * Almacen de geometrias indexado y simplificado. Lo construye el cargador (en
   * segundo plano) al abrir la capa; mientras es nulo, el lienzo dibuja la capa
   * con el camino clasico de lectura directa.
   */
  std::shared_ptr<GeometryStore> geometryStore() const { return mGeometryStore; }
  void setGeometryStore(const std::shared_ptr<GeometryStore> &store) { mGeometryStore = store; }

  /** Texto corto para listados: "cities (12 elementos)". */
  QString displayName() const;

private:
  VectorLayer() = default;

  void readMetadata();
  void readFields();

  GDALDatasetH mDatasetHandle = nullptr;
  OGRLayerH mLayerHandle = nullptr;
  QString mSourcePath;
  QString mName;
  QString mDriverName;
  QString mGeometryTypeName;
  GeometryKind mKind = GeometryKind::Unknown;
  QVector<FieldInfo> mFields;
  Rectangle mExtent;
  Style mStyle;
  std::shared_ptr<GeometryStore> mGeometryStore;
  bool mVisible = true;
  mutable qint64 mFeatureCount = -1; // -1 = pendiente de calcular
};

} // namespace minigis
