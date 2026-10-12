#include "minigisvectorloader.h"

#include "minigisvectorlayer.h"

#include <QFileInfo>
#include <QLoggingCategory>
#include <QString>

#include <QtConcurrent/QtConcurrentRun>

namespace minigis
{

/** Traza de la carga: QT_LOGGING_RULES="minigis.loading=true" para verla. */
Q_LOGGING_CATEGORY(loadingLog, "minigis.loading")

VectorLayerLoader::VectorLayerLoader(QObject *parent)
  : QObject(parent)
{
  // El almacen indexado se construye en el hilo de trabajo: que su tipo viaje
  // por una conexion en cola no depende de nadie mas que de esta clase.
  qRegisterMetaType<std::shared_ptr<minigis::VectorLayer>>("std::shared_ptr<minigis::VectorLayer>");
}

VectorLayerLoader::~VectorLayerLoader()
{
  cancel();
}

void VectorLayerLoader::start(const QStringList &paths)
{
  if (paths.isEmpty())
    return;

  if (mRunning.load())
  {
    emit progressChanged(0, QStringLiteral("Ya hay una carga en curso."));
    return;
  }

  mCancelled.store(false);
  mRunning.store(true);

  emit started(paths);

  // El trabajo se ejecuta en el pool global; la lambda captura una copia de la
  // lista de rutas (no la referencia a mPaths, que podria cambiar).
  QtConcurrent::run([this, paths]() { run(paths); });
}

void VectorLayerLoader::cancel()
{
  mCancelled.store(true);
}

bool VectorLayerLoader::reportProgress(int percent, const QString &message)
{
  qCDebug(loadingLog) << "progreso" << percent << "%" << message;
  emit progressChanged(percent, message);
  return !mCancelled.load();
}

void VectorLayerLoader::run(const QStringList &paths)
{
  const int total = paths.size();
  int loaded = 0;
  int failed = 0;
  bool cancelled = false;

  for (int index = 0; index < total; ++index)
  {
    if (mCancelled.load())
    {
      cancelled = true;
      break;
    }

    const QString path = paths.at(index);
    const QString shortName = QFileInfo(path).fileName();

    // El progreso se reparte entre los archivos: cada uno ocupa una franja.
    const int base = (100 * index) / total;
    const int span = 100 / total;

    reportProgress(base, QStringLiteral("Abriendo «%1» (%2 de %3)...").arg(shortName).arg(index + 1).arg(total));

    Error error;
    const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, &error);
    if (!layer)
    {
      ++failed;
      emit layerFailed(path, error.message, error.hint);
      continue;
    }

    if (mBuildGeometryStore)
    {
      // Progreso del indice: de base+10 a base+span.
      const int innerBase = base + std::max(1, span / 10);
      const int innerSpan = std::max(0, span - std::max(1, span / 10));

      QString storeError;
      const std::shared_ptr<GeometryStore> store = GeometryStore::buildFromLayer(
        *layer, mDestinationEpsg, mStoreOptions,
        [this, innerBase, innerSpan, &shortName](int percent, const QString &message) {
          const int global = innerBase + (innerSpan * percent) / 100;
          return reportProgress(global, QStringLiteral("%1 — %2").arg(shortName, message));
        },
        &storeError);

      if (store)
      {
        layer->setGeometryStore(store);
      }
      else if (mCancelled.load())
      {
        cancelled = true;
        break;
      }
      else
      {
        // No es un fallo fatal: la capa se dibujara leyendo de GDAL en cada
        // cuadro. Se avisa para que el usuario entienda por que va mas lenta.
        emit layerFailed(path, QStringLiteral("La capa se abrio, pero no se pudo preparar el "
                                              "indice de dibujo rapido."),
                         storeError);
      }
    }

    ++loaded;
    emit layerReady(layer, index == 0);
  }

  mRunning.store(false);

  if (cancelled)
    emit progressChanged(0, QStringLiteral("Carga cancelada."));
  else
    emit progressChanged(100, QStringLiteral("Carga terminada."));

  emit finished(loaded, failed, cancelled);
}

} // namespace minigis
