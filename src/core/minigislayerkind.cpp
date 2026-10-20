#include "minigislayerkind.h"

#include "minigisgdalinit.h"

#include <QDir>
#include <QFileInfo>

#include <gdal.h>

namespace minigis
{

LayerKind detectLayerKind(const QString &path, Error *error)
{
  const auto fail = [error](const QString &message, const QString &hint) {
    if (error)
      *error = Error {message, hint};
    return LayerKind::Unknown;
  };

  if (path.trimmed().isEmpty())
    return fail(QStringLiteral("No se ha indicado ningun archivo."),
                QStringLiteral("Indica una capa vectorial o raster, por ejemplo un .shp o un .tif."));

  if (!QFileInfo::exists(path))
  {
    return fail(QStringLiteral("No existe el archivo «%1».").arg(QDir::toNativeSeparators(path)),
                QStringLiteral("Comprueba la ruta y vuelve a intentarlo."));
  }

  clearLastGdalError();
  const QByteArray utf8Path = path.toUtf8();
  GDALDatasetH dataset =
    GDALOpenEx(utf8Path.constData(), GDAL_OF_VECTOR | GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);

  if (!dataset)
  {
    // Algunos drivers no aceptan la apertura combinada: se prueban por separado.
    dataset = GDALOpenEx(utf8Path.constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (dataset)
    {
      const int layerCount = GDALDatasetGetLayerCount(dataset);
      GDALClose(dataset);
      return layerCount > 0 ? LayerKind::Vector : LayerKind::Unknown;
    }

    dataset = GDALOpenEx(utf8Path.constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (dataset)
    {
      const int bandCount = GDALGetRasterCount(dataset);
      GDALClose(dataset);
      return bandCount > 0 ? LayerKind::Raster : LayerKind::Unknown;
    }

    QString hint = QStringLiteral("GDAL no reconoce el archivo. Comprueba su contenido con: gdalinfo \"%1\"")
                     .arg(QDir::toNativeSeparators(path));
    const QString gdalMessage = lastGdalError();
    if (!gdalMessage.isEmpty())
      hint = QStringLiteral("GDAL informa: %1\n%2").arg(gdalMessage, hint);

    return fail(QStringLiteral("No se pudo abrir «%1».").arg(QDir::toNativeSeparators(path)), hint);
  }

  // Un GeoPackage con capas y teselas se trata como vectorial (documentado).
  const int layerCount = GDALDatasetGetLayerCount(dataset);
  const int bandCount = GDALGetRasterCount(dataset);
  GDALClose(dataset);

  if (layerCount > 0)
    return LayerKind::Vector;
  if (bandCount > 0)
    return LayerKind::Raster;

  return fail(QStringLiteral("«%1» no contiene capas vectoriales ni bandas raster.")
                .arg(QDir::toNativeSeparators(path)),
              QStringLiteral("Comprueba el contenido con: gdalinfo \"%1\"").arg(QDir::toNativeSeparators(path)));
}

} // namespace minigis
