#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <map>
#include <memory>

#include "minigiserror.h"
#include "minigisrasterstyle.h"
#include "minigisrectangle.h"

#include <gdal.h>

class OGRSpatialReference;

namespace minigis
{

/**
 * Capa raster de solo lectura (equivalente minimo a QgsRasterLayer).
 *
 * Es dueña del dataset de GDAL, que se cierra en el destructor. El dataset se
 * abre, se lee y se dibuja en el mismo hilo (el de la interfaz): GDAL no
 * garantiza acceso seguro a la misma instancia desde varios hilos.
 *
 * Toda la interaccion con GDAL se hace por la API C, que es la superficie mas
 * estable entre versiones, igual que en `VectorLayer`.
 */
class RasterLayer
{
public:
  /** Metadatos de una banda que se muestran en --info y usa el dialogo. */
  struct BandInfo
  {
    QString typeName;
    QString colorName;
    bool hasNoData = false;
    double noData = 0.0;
    int overviewCount = 0;
    int colorCount = 0;
  };

  /**
   * Estadisticas de una banda obtenidas de una muestra decimada al abrir.
   * `p2`/`p98` son los percentiles de recorte; `min`/`max`, los extremos.
   */
  struct BandStats
  {
    bool valid = false;
    double min = 0.0;
    double max = 255.0;
    double p2 = 0.0;
    double p98 = 255.0;
  };

  /**
   * Fuente de datos para dibujar: el dataset original cuando el SRC coincide
   * con el del lienzo, o un VRT reproyectado (cacheado por SRC de destino)
   * cuando no. La vista presta el dataset y su geotransformacion ya en el SRC
   * de destino.
   */
  struct View
  {
    GDALDatasetH dataset = nullptr;
    double geoTransform[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    int width = 0;
    int height = 0;
    bool ownsDataset = false;

    ~View();
  };

  ~RasterLayer();
  RasterLayer(const RasterLayer &) = delete;
  RasterLayer &operator=(const RasterLayer &) = delete;

  /**
   * Abre un raster. Devuelve nullptr y rellena `error` (causa + accion) si el
   * archivo no existe, GDAL no lo reconoce como raster o no tiene bandas.
   */
  static std::shared_ptr<RasterLayer> open(const QString &path, Error *error = nullptr);

  QString sourcePath() const { return mSourcePath; }
  QString name() const { return mName; }
  QString driverName() const { return mDriverName; }

  int width() const { return mWidth; }
  int height() const { return mHeight; }
  int bandCount() const { return static_cast<int>(mBandHandles.size()); }
  const QVector<BandInfo> &bands() const { return mBands; }
  /** Estadisticas de la banda (1-based); invalidas si no se pudieron calcular. */
  const BandStats &bandStats(int band) const;

  /**
   * Vista para dibujar en el SRC indicado. La primera vez que se pide un SRC
   * distinto del nativo crea un warp con GDAL y lo cachea; el llamador no es
   * dueño del resultado y este sigue siendo valido mientras viva la capa.
   */
  const View *viewForEpsg(int destinationEpsg) const;

  Rectangle extent() const { return mExtent; }

  /**
   * Geotransformacion afil del archivo (6 valores, convenio GDAL). El
   * renderizador la necesita para pasar de coordenadas de mapa a pixeles de
   * origen. Devuelve false si el archivo no esta georreferenciado.
   */
  bool hasGeoTransform() const { return mHasGeoTransform; }
  const double *geoTransform() const { return mGeoTransform; }

  /**
   * Extension de la capa en el SRC indicado. A diferencia del vectorial, se
   * transforman puntos intermedios de cada borde: la reproyeccion de un raster
   * no es afil y las esquinas solas dejarian fuera parte de la imagen.
   */
  Rectangle extentInEpsg(int destinationEpsg) const;

  QString crsDescription() const;
  int crsEpsg() const;
  QString crsWkt() const;
  const OGRSpatialReference *spatialRef() const; // prestado, puede ser nullptr

  /** Factores de overview disponibles en la banda 1 (por ejemplo 2, 4, 8). */
  QVector<int> overviewFactors() const;
  bool hasOverviews() const { return !overviewFactors().isEmpty(); }

  bool isVisible() const { return mVisible; }
  void setVisible(bool visible)
  {
    if (mVisible == visible)
      return;
    mVisible = visible;
    bumpRevision();
  }

  const RasterStyle &style() const { return mStyle; }
  void setStyle(const RasterStyle &style)
  {
    mStyle = style;
    bumpRevision();
  }

  /**
   * Numero que sube al cambiar algo que afecta a como se dibuja la capa
   * (estilo, visibilidad o la aparicion del warp). El lienzo lo usa en la clave
   * de su cache de cuadro, igual que hace con `VectorLayer`.
   */
  quint64 revision() const { return mRevision; }

  GDALDatasetH datasetHandle() const { return mDatasetHandle; }
  GDALRasterBandH bandHandle(int band) const; // 1-based; nullptr si no existe

  /** Texto corto para listados: "ortho_rgb (256x256, 3 bandas)". */
  QString displayName() const;

  /** Etiqueta de cada banda para los desplegables: "1: Byte (Red)". */
  QStringList bandLabels() const;

private:
  RasterLayer() = default;

  void readMetadata();
  void bumpRevision() { ++mRevision; }

  GDALDatasetH mDatasetHandle = nullptr;
  QVector<GDALRasterBandH> mBandHandles;
  QVector<BandInfo> mBands;
  QVector<BandStats> mBandStats;
  /** Vista nativa (sin warp): presta mDatasetHandle. */
  View mNativeView;
  /** Vistas reproyectadas, una por SRC de destino. */
  mutable std::map<int, std::shared_ptr<View>> mWarpedViews;
  QString mSourcePath;
  QString mName;
  QString mDriverName;
  int mWidth = 0;
  int mHeight = 0;
  bool mHasGeoTransform = false;
  double mGeoTransform[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  Rectangle mExtent;
  RasterStyle mStyle;
  bool mVisible = true;
  quint64 mRevision = 1;
};

} // namespace minigis
