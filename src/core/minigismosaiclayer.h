#pragma once

#include <QSize>
#include <QString>

#include <map>
#include <memory>
#include <vector>

#include "minigisrasterstyle.h"
#include "minigisrectangle.h"

namespace minigis
{

class RasterLayer;
struct Error;

/**
 * Mosaico de capas raster contiguas (las "hojas" de una cobertura).
 *
 * El mosaico NO es un `RasterLayer`: es una coleccion de hojas con su propia
 * revision, visibilidad y estilo, y se dibuja con un camino propio
 * (`RasterRenderer::paintMosaic`). Esa separacion es la que evita el coste medido
 * de cargar una cobertura como N capas sueltas: cada capa suelta creaba su VRT
 * reproyectado en cada escala (6,5 ms por hoja y cuadro) aunque estuviera fuera
 * de la vista.
 *
 * La revision sube cuando entra o sale una hoja y cuando cambia la visibilidad o
 * el estilo: es la llave de la cache de cuadro del lienzo, igual que en
 * `VectorLayer` y `RasterLayer`. No se invalida la cache a mano.
 */
class RasterMosaic
{
public:
  /** Crea un mosaico vacio que se dibuja en el SRC indicado. */
  static std::shared_ptr<RasterMosaic> create(int destinationEpsg);

  /**
   * Anade una hoja. Devuelve false y rellena `error` si la hoja no tiene
   * georreferenciacion (sin geotransformacion no se puede colocar en el mapa) o
   * si ya estaba (misma ruta de origen).
   */
  bool addTile(const std::shared_ptr<RasterLayer> &tile, Error *error = nullptr);
  bool containsSource(const QString &path) const;

  int tileCount() const { return static_cast<int>(mTiles.size()); }
  const std::vector<std::shared_ptr<RasterLayer>> &tiles() const { return mTiles; }
  std::shared_ptr<RasterLayer> tile(int index) const;

  /** Nombre visible: el que se fijo, o el de la carpeta comun de las hojas. */
  QString name() const;
  void setName(const QString &name)
  {
    mName = name;
    mDerivedName.clear();
  }

  int destinationEpsg() const { return mDestinationEpsg; }

  /**
   * Extension de la union de las hojas en el SRC indicado. Se calcula una vez
   * por SRC y se guarda: el lienzo la pide en cada encuadre y recorrer 508 hojas
   * con sus transformaciones en cada cuadro no es gratis.
   */
  Rectangle extentInEpsg(int epsg) const;
  Rectangle extent() const { return extentInEpsg(mDestinationEpsg); }

  /**
   * Extension de cada hoja en el SRC indicado, en el mismo orden que `tiles()`.
   * La usa el renderizador para descartar lo que no corta la vista.
   */
  const std::vector<Rectangle> &tileExtents(int epsg) const;

  bool isVisible() const { return mVisible; }
  void setVisible(bool visible);

  const RasterStyle &style() const { return mStyle; }
  void setStyle(const RasterStyle &style);

  /**
   * Replica piramidal del mosaico: un GeoTIFF ya en el SRC del lienzo, con sus
   * overviews. Mientras exista y su pixel no haya que ampliarlo mas de 2x, el
   * mosaico se dibuja desde ella en vez de leer las hojas.
   */
  bool hasReplica() const { return mReplicaLayer != nullptr; }
  const std::shared_ptr<RasterLayer> &replicaLayer() const { return mReplicaLayer; }
  double replicaPixelSize() const { return mReplicaPixelSize; }
  QSize replicaPixels() const { return mReplicaPixels; }

  /**
   * Fija la replica (o la cambia). Sube la revision: el lienzo tiene que
   * redibujar con el camino nuevo. Devuelve false si el archivo no abre.
   */
  bool setReplica(const QString &path, double pixelSize, const QSize &pixels);
  void clearReplica();

  /** Numero que sube al cambiar algo que afecta a como se dibuja el mosaico. */
  quint64 revision() const { return mRevision; }

private:
  RasterMosaic() = default;

  void bumpRevision() { ++mRevision; }
  void invalidateCache();

  std::vector<std::shared_ptr<RasterLayer>> mTiles;
  /** Extensiones por SRC de destino (clave: EPSG). */
  mutable std::map<int, Rectangle> mExtents;
  mutable std::map<int, std::vector<Rectangle>> mTileExtents;
  RasterStyle mStyle;
  /** Replica piramidal (presta su propio RasterLayer) y su resolucion. */
  std::shared_ptr<RasterLayer> mReplicaLayer;
  double mReplicaPixelSize = 0.0;
  QSize mReplicaPixels;
  QString mName;
  mutable QString mDerivedName;
  int mDestinationEpsg = 4326;
  bool mVisible = true;
  quint64 mRevision = 1;
};

} // namespace minigis
