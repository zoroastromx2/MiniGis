#pragma once

#include <QFuture>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

#include "minigismosaicreplica.h"

namespace minigis
{

class RasterLayer;

/**
 * Apertura en paralelo de las hojas de un mosaico.
 *
 * Abrir una hoja cuesta ~50 ms (metadatos, muestreo de estadisticas y lectura del
 * PAM): con 508 hojas eso son 25 s de ventana congelada si se hace en el hilo de
 * la interfaz. Aqui se abren varias a la vez en hilos del pool y se entregan por
 * lotes EN EL ORDEN DE LA SELECCION, de modo que el mapa se va llenando mientras
 * carga.
 *
 * Cada hilo abre y termina su propio dataset: no se comparte ninguno entre hilos
 * y el que se entrega a la interfaz ya no lo toca el trabajador.
 */
class MosaicLoader : public QObject
{
  Q_OBJECT

public:
  explicit MosaicLoader(QObject *parent = nullptr);
  ~MosaicLoader() override;

  /** Hilos de apertura (0 = los del equipo, acotado a 8). */
  void setThreadCount(int threads) { mThreadCount = threads; }
  int threadCount() const { return mThreadCount; }

  bool isRunning() const { return mRunning.load(); }

  /** Encola la apertura de las rutas; vuelve inmediatamente. */
  void start(const QStringList &paths);

  /** Pide cancelar: los archivos en curso terminan, no se empiezan mas. */
  void cancel();

signals:
  void progressChanged(int percent, const QString &message, const QString &currentPath);

  /** Lote de hojas ya abiertas, en el orden de la seleccion. */
  void tilesReady(const QVector<std::shared_ptr<minigis::RasterLayer>> &tiles, int loaded, int total);

  /** Una hoja no se pudo abrir; la carga continua con las demas. */
  void tileFailed(const QString &path, const QString &message, const QString &hint);

  void finished(int loaded, int failed, bool cancelled);

private:
  void run(const QStringList &paths);

  QFuture<void> mFuture;
  int mThreadCount = 0;
  std::atomic_bool mCancelled {false};
  std::atomic_bool mRunning {false};
};

/**
 * Preparacion de la replica piramidal en segundo plano.
 *
 * La replica de una cobertura de cientos de hojas tarda decenas de segundos: si
 * se hiciera en el hilo de la interfaz, la ventana se quedaria congelada justo
 * despues de cargar. El trabajo se hace por rutas (no necesita datasets abiertos),
 * informa del avance y se puede cancelar.
 */
class MosaicReplicaJob : public QObject
{
  Q_OBJECT

public:
  explicit MosaicReplicaJob(QObject *parent = nullptr);
  ~MosaicReplicaJob() override;

  void setOptions(const MosaicReplicaOptions &options) { mOptions = options; }
  const MosaicReplicaOptions &options() const { return mOptions; }

  bool isRunning() const { return mRunning.load(); }

  void start(const QStringList &paths, int destinationEpsg);
  void cancel();

signals:
  void progressChanged(int percent, const QString &message);
  void finished(const minigis::MosaicReplicaState &state);

private:
  void run(const QStringList &paths, int destinationEpsg);

  QFuture<void> mFuture;
  MosaicReplicaOptions mOptions;
  std::atomic_bool mCancelled {false};
  std::atomic_bool mRunning {false};
};

} // namespace minigis

Q_DECLARE_METATYPE(minigis::MosaicReplicaState)
