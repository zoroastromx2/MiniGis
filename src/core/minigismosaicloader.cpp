#include "minigismosaicloader.h"

#include "minigiserror.h"
#include "minigisrasterlayer.h"

#include <QFileInfo>
#include <QMutex>
#include <QThread>
#include <QThreadPool>

#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace minigis
{
namespace
{

/** Hilos de apertura: los del equipo, sin pasar de 8 (disco y GDAL). */
int effectiveThreadCount(int requested)
{
  if (requested > 0)
    return requested;
  const int hardware = QThread::idealThreadCount();
  return std::clamp(hardware > 0 ? hardware : 4, 1, 8);
}

} // namespace

MosaicLoader::MosaicLoader(QObject *parent) : QObject(parent) {}

MosaicLoader::~MosaicLoader()
{
  cancel();
  if (mFuture.isRunning())
    mFuture.waitForFinished();
}

void MosaicLoader::start(const QStringList &paths)
{
  if (paths.isEmpty() || mRunning.load())
    return;

  mCancelled.store(false);
  mRunning.store(true);
  const QStringList copy = paths;
  mFuture = QtConcurrent::run([this, copy]() { run(copy); });
}

void MosaicLoader::cancel()
{
  mCancelled.store(true);
}

void MosaicLoader::run(const QStringList &paths)
{
  const int total = paths.size();

  struct Slot
  {
    bool done = false;
    bool ok = false;
    std::shared_ptr<RasterLayer> layer;
    QString path;
    QString message;
    QString hint;
  };

  QVector<Slot> slotStates(total);
  QMutex mutex;
  int loaded = 0;
  int failed = 0;
  int nextToEmit = 0;

  // Entrega en orden: aunque las hojas terminen desordenadas, el mosaico y el
  // panel las reciben como las eligio el usuario.
  const auto flushReady = [&]() {
    QMutexLocker locker(&mutex);

    QVector<std::shared_ptr<RasterLayer>> batch;
    while (nextToEmit < total && slotStates[nextToEmit].done)
    {
      Slot &slot = slotStates[nextToEmit];
      if (slot.ok)
      {
        batch << slot.layer;
        ++loaded;
      }
      else
      {
        ++failed;
        emit tileFailed(slot.path, slot.message, slot.hint);
      }
      ++nextToEmit;
    }

    if (!batch.isEmpty())
      emit tilesReady(batch, loaded, total);
  };

  QVector<int> indices;
  indices.reserve(total);
  for (int index = 0; index < total; ++index)
    indices << index;

  QThreadPool pool;
  pool.setMaxThreadCount(effectiveThreadCount(mThreadCount));

  QtConcurrent::blockingMap(&pool, indices, [&](int index) {
    if (mCancelled.load())
      return;

    Slot slot;
    slot.path = paths.at(index);

    Error error;
    slot.layer = RasterLayer::open(slot.path, &error);
    slot.ok = slot.layer != nullptr;
    if (!slot.ok)
    {
      slot.message = error.message;
      slot.hint = error.hint;
    }
    slot.done = true;

    int completed = 0;
    {
      QMutexLocker locker(&mutex);
      slotStates[index] = slot;
      for (const Slot &candidate : slotStates)
      {
        if (candidate.done)
          ++completed;
      }
    }

    emit progressChanged(total > 0 ? completed * 100 / total : 100,
                         QStringLiteral("Abriendo %1 de %2: %3")
                           .arg(completed)
                           .arg(total)
                           .arg(QFileInfo(slot.path).fileName()),
                         slot.path);

    flushReady();
  });

  mRunning.store(false);
  emit finished(loaded, failed, mCancelled.load());
}

MosaicReplicaJob::MosaicReplicaJob(QObject *parent) : QObject(parent) {}

MosaicReplicaJob::~MosaicReplicaJob()
{
  cancel();
  if (mFuture.isRunning())
    mFuture.waitForFinished();
}

void MosaicReplicaJob::start(const QStringList &paths, int destinationEpsg)
{
  if (paths.isEmpty() || mRunning.load())
    return;

  mCancelled.store(false);
  mRunning.store(true);
  const QStringList copy = paths;
  mFuture = QtConcurrent::run([this, copy, destinationEpsg]() { run(copy, destinationEpsg); });
}

void MosaicReplicaJob::cancel()
{
  mCancelled.store(true);
}

void MosaicReplicaJob::run(const QStringList &paths, int destinationEpsg)
{
  const auto progressSink = [this](int percent, const QString &message) {
    if (mCancelled.load())
      return false;
    emit progressChanged(percent, message);
    return true;
  };

  const MosaicReplicaState state = MosaicReplica::preparePaths(paths, destinationEpsg, mOptions, progressSink);

  mRunning.store(false);
  emit finished(state);
}

} // namespace minigis
