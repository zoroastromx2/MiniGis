#pragma once

#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

#include "minigisgeometrystore.h"

namespace minigis
{

class VectorLayer;

/**
 * Carga de capas vectoriales en segundo plano.
 *
 * El problema que resuelve: `VectorLayer::open()` + la lectura de geometrias
 * pueden tardar segundos en un GeoPackage o un shapefile grandes. Hacerlo en el
 * hilo de la interfaz congela la ventana y, ademas, impide mostrar cualquier
 * retroalimentacion (el propio mensaje de "cargando" no llegaria a pintarse).
 *
 * Diseno:
 *   * `start()` lanza el trabajo con QtConcurrent y vuelve inmediatamente: el
 *     hilo de la interfaz nunca se bloquea.
 *   * El trabajo (abrir con GDAL + leer geometrias + construir el indice) se
 *     ejecuta en un hilo del pool. Las senales se emiten desde ese hilo y Qt las
 *     entrega en la cola de eventos del hilo de la interfaz (conexion en cola
 *     automatica), de modo que los slots que actualizan la barra de progreso se
 *     ejecutan siempre en el hilo de la GUI.
 *   * Un dataset de GDAL no es seguro entre hilos: cada capa se abre, se lee y se
 *     indexa dentro del mismo hilo de trabajo, y solo despues se entrega a la
 *     interfaz (que ya no comparte el dataset con el hilo de trabajo).
 *   * `cancel()` marca una bandera atomica que los puntos de control consultan
 *     (lectura de geometrias y niveles de detalle), de modo que una carga larga
 *     se puede abortar sin esperar a que termine.
 *
 * Uso tipico:
 * @code
 *   auto *loader = new VectorLayerLoader(this);
 *   connect(loader, &VectorLayerLoader::progressChanged, this, [](int p, const QString &m) { ... });
 *   connect(loader, &VectorLayerLoader::layerReady, this, [](const auto &layer, bool first) { ... });
 *   loader->start({"/datos/parcelas.gpkg"});
 * @endcode
 */
class VectorLayerLoader : public QObject
{
  Q_OBJECT

public:
  explicit VectorLayerLoader(QObject *parent = nullptr);
  ~VectorLayerLoader() override;

  /** SRC de destino con el que se indexan las geometrias (por defecto EPSG:4326). */
  void setDestinationEpsg(int epsg) { mDestinationEpsg = epsg; }
  int destinationEpsg() const { return mDestinationEpsg; }

  /** Ajustes del almacen indexado (presupuesto de vertices, niveles, etc.). */
  void setStoreOptions(const GeometryStore::Options &options) { mStoreOptions = options; }
  const GeometryStore::Options &storeOptions() const { return mStoreOptions; }

  /**
   * Si es false solo se abre la capa (sin indice ni simplificacion). Util para
   * formatos enormes en los que no conviene copiar la geometria a memoria: la
   * capa se dibujara con el camino clasico de lectura directa.
   */
  void setBuildGeometryStore(bool enabled) { mBuildGeometryStore = enabled; }
  bool buildGeometryStore() const { return mBuildGeometryStore; }

  bool isRunning() const { return mRunning.load(); }

  /** Encola la carga de varias capas; vuelve inmediatamente. */
  void start(const QStringList &paths);

  /** Pide cancelar la carga en curso (se atiende en el siguiente punto de control). */
  void cancel();

signals:
  void started(const QStringList &paths);

  /** Progreso global 0..100 con un mensaje listo para mostrar. */
  void progressChanged(int percent, const QString &message);

  /**
   * Una capa esta lista para dibujarse. `first` indica que es la primera de la
   * tanda (el lienzo la usa para encuadrar la vista).
   */
  void layerReady(const std::shared_ptr<minigis::VectorLayer> &layer, bool first);

  /** Una capa no se pudo abrir; se informa y la carga continua con las demas. */
  void layerFailed(const QString &path, const QString &message, const QString &hint);

  /** Fin de la tanda: cuantas se cargaron, cuantas fallaron y si se cancelo. */
  void finished(int loaded, int failed, bool cancelled);

private:
  /** Cuerpo del trabajo; se ejecuta en un hilo del pool. */
  void run(const QStringList &paths);

  /** Emite el progreso global y devuelve false si hay que cancelar. */
  bool reportProgress(int percent, const QString &message);

  GeometryStore::Options mStoreOptions;
  int mDestinationEpsg = 4326;
  bool mBuildGeometryStore = true;
  std::atomic_bool mCancelled {false};
  std::atomic_bool mRunning {false};
};

} // namespace minigis
