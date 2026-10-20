#pragma once

#include <QString>

#include "minigiserror.h"

namespace minigis
{

/** Clase de capa que contiene un archivo, detectada por contenido. */
enum class LayerKind
{
  Vector,
  Raster,
  Unknown
};

/**
 * Deteccion barata del tipo de capa: abre el archivo con GDAL en modo lectura
 * (vectorial y raster a la vez), mira si tiene capas o bandas y lo cierra.
 *
 * Se usa antes de decidir si un archivo va al cargador vectorial en segundo
 * plano o se abre como raster. No sustituye a `VectorLayer::open` /
 * `RasterLayer::open`: solo clasifica.
 */
LayerKind detectLayerKind(const QString &path, Error *error = nullptr);

} // namespace minigis
