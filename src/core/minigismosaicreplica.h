#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

namespace minigis
{

class RasterLayer;
class RasterMosaic;

/**
 * Opciones de la replica piramidal del mosaico.
 *
 * La replica es un GeoTIFF en el SRC del lienzo que se construye una vez (warp +
 * overviews) y se guarda en la cache del usuario: es lo que permite dibujar la
 * vista de pais en milisegundos en lugar de decodificar cientos de hojas.
 */
struct MosaicReplicaOptions
{
  /** Carpeta de cache; vacia = CacheLocation/mosaicos. */
  QString cacheDir;
  /** false con --no-mosaic-replica: se dibuja siempre desde las hojas. */
  bool enabled = true;
  /** Tope total de la cache; al pasarse se borran las replicas menos usadas. */
  qint64 maxCacheBytes = 4096LL * 1024LL * 1024LL;
  /** Pixeles de origen por pixel de replica (8 = 1/8 de la resolucion). */
  int factor = 8;
  /** Lado maximo de la replica, en pixeles. */
  int maxSide = 16384;
};

/** Resultado de preparar la replica. */
struct MosaicReplicaState
{
  /** true si hay replica disponible (nueva o reutilizada). */
  bool available = false;
  /** true si ya existia, estaba validada y no se reconstruyo. */
  bool reused = false;
  /** true si hubo que reconstruirla (faltaba o una hoja cambio). */
  bool rebuilt = false;
  /** Ruta del GeoTIFF de la replica. */
  QString path;
  /** Tamano de la replica, en pixeles. */
  QSize pixels;
  /** Unidades de mapa por pixel de la replica (SRC del lienzo). */
  double pixelSize = 0.0;
  qint64 bytes = 0;
  int levels = 0;
  /** Motivo cuando no hay replica (queda en `available == false`). */
  QString message;
};

/**
 * Replica piramidal del mosaico, cacheada en disco.
 *
 * `prepare` reutiliza la replica si la clave (rutas + tamano + fecha de las
 * hojas + EPSG + factor) coincide con la del archivo lateral, o la construye con
 * GDAL: VRT de las hojas, warp al SRC del lienzo a 1/factor y overviews
 * internas. Todo el trabajo pesado es cancelable con el callback de progreso.
 *
 * Nunca escribe en la carpeta de los datos: la cache va a la carpeta del usuario
 * y, si esa no se puede escribir, a la carpeta temporal.
 */
class MosaicReplica
{
public:
  /** Carpeta efectiva de cache (creandola si hace falta). */
  static QString cacheDirectory(const MosaicReplicaOptions &options);

  /**
   * Clave de la replica: hash de las rutas (ruta, tamano, fecha), del SRC de
   * destino, del factor y de la version del formato.
   */
  static QString cacheKey(const QStringList &paths, int destinationEpsg,
                          const MosaicReplicaOptions &options);

  /**
   * Prepara la replica a partir de las rutas de las hojas. Es la entrada que usa
   * el trabajo en segundo plano: solo necesita los archivos, no datasets
   * abiertos, de modo que puede ejecutarse sin tocar los que dibuja la interfaz.
   */
  static MosaicReplicaState preparePaths(const QStringList &paths, int destinationEpsg,
                                         const MosaicReplicaOptions &options,
                                         const std::function<bool(int, const QString &)> &progress = {});

  /**
   * Prepara la replica del mosaico. `progress` recibe (porcentaje, mensaje) y
   * devuelve false para cancelar; la replica queda como no disponible y el
   * mosaico se dibuja desde las hojas.
   */
  static MosaicReplicaState prepare(const RasterMosaic &mosaic, int destinationEpsg,
                                    const MosaicReplicaOptions &options,
                                    const std::function<bool(int, const QString &)> &progress = {});

  /** Borra las replicas menos usadas hasta respetar el tope. `keepKey` se salva. */
  static qint64 trimCache(const QString &directory, qint64 maxBytes, const QString &keepKey);
};

} // namespace minigis
