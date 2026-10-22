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
  bool firstLayer = true;

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

    // Un archivo puede contener varias capas (un GeoPackage, por ejemplo): se
    // cargan todas, no solo la primera. Asi el panel del mapa muestra el
    // contenido real del archivo.
    Error listError;
    const QStringList layerNames = VectorLayer::layerNames(path, &listError);
    if (layerNames.isEmpty())
    {
      ++failed;
      emit layerFailed(path, listError.message, listError.hint);
      continue;
    }

    const int layerTotal = layerNames.size();

    for (int layerIndex = 0; layerIndex < layerTotal; ++layerIndex)
    {
      if (mCancelled.load())
      {
        cancelled = true;
        break;
      }

      // Franja del progreso que ocupa esta capa dentro del archivo. Se calculan
      // los dos extremos (no un ancho fijo) para que la ultima capa llegue justo
      // al final de la franja aunque el reparto no sea divisible.
      const int layerBase = base + (span * layerIndex) / layerTotal;
      const int layerEnd = base + (span * (layerIndex + 1)) / layerTotal;
      const QString layerName = layerNames.at(layerIndex);

      const QString opening = layerTotal > 1
        ? QStringLiteral("Abriendo «%1» — «%2» (%3 de %4)...")
            .arg(shortName, layerName).arg(layerIndex + 1).arg(layerTotal)
        : QStringLiteral("Abriendo «%1»...").arg(shortName);
      reportProgress(layerBase, opening);

      Error error;
      const std::shared_ptr<VectorLayer> layer = VectorLayer::open(path, layerIndex, &error);
      if (!layer)
      {
        ++failed;
        const QString message = layerTotal > 1
          ? QStringLiteral("Capa «%1»: %2").arg(layerName, error.message)
          : error.message;
        emit layerFailed(path, message, error.hint);
        continue;
      }

      if (mBuildGeometryStore)
      {
        // Progreso del indice: deja el primer 10 % de la franja para abrir.
        const int innerBase = layerBase + std::max(1, (layerEnd - layerBase) / 10);
        const int innerSpan = std::max(0, layerEnd - innerBase);
        const QString progressName = layerTotal > 1
          ? QStringLiteral("«%1» — %2").arg(shortName, layerName)
          : shortName;

        QString storeError;
        std::shared_ptr<GeometryStore> store = GeometryStore::buildFromLayer(
          *layer, mDestinationEpsg, mStoreOptions,
          [this, innerBase, innerSpan, &progressName](int percent, const QString &message) {
            const int global = innerBase + (innerSpan * percent) / 100;
            return reportProgress(global, QStringLiteral("%1 — %2").arg(progressName, message));
          },
          &storeError);

        // La capa no cabe en memoria (millones de elementos): en lugar de
        // renunciar a la aceleracion, se indexan solo las envolventes y la
        // geometria se lee bajo demanda segun lo que se ve.
        if (!store && mStoreOptions.allowEnvelopeFallback && !mCancelled.load())
        {
          reportProgress(innerBase, QStringLiteral("%1 — La capa no cabe en memoria; indexando "
                                                   "envolventes...").arg(progressName));

          QString envelopeError;
          store = GeometryStore::buildEnvelopeIndex(
            *layer, mDestinationEpsg, mStoreOptions,
            [this, innerBase, innerSpan, &progressName](int percent, const QString &message) {
              const int global = innerBase + (innerSpan * percent) / 100;
              return reportProgress(global, QStringLiteral("%1 — %2").arg(progressName, message));
            },
            &envelopeError);

          if (!store)
            storeError = QStringLiteral("%1\nIndice de envolventes: %2").arg(storeError, envelopeError);
        }

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
          emit layerFailed(path, QStringLiteral("La capa se abrió, pero no se pudo preparar el "
                                                "índice de dibujo rápido."),
                           storeError);
        }
      }

      ++loaded;
      emit layerReady(layer, firstLayer);
      firstLayer = false;
    }

    if (cancelled)
      break;
  }

  mRunning.store(false);

  if (cancelled)
    emit progressChanged(0, QStringLiteral("Carga cancelada."));
  else
    emit progressChanged(100, QStringLiteral("Carga terminada."));

  emit finished(loaded, failed, cancelled);
}

} // namespace minigis
