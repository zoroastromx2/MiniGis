#include "minigismosaiclayer.h"

#include "minigiserror.h"
#include "minigisrasterlayer.h"

#include <QFileInfo>

#include <algorithm>

namespace minigis
{

std::shared_ptr<RasterMosaic> RasterMosaic::create(int destinationEpsg)
{
  std::shared_ptr<RasterMosaic> mosaic(new RasterMosaic);
  mosaic->mDestinationEpsg = destinationEpsg;
  return mosaic;
}

bool RasterMosaic::addTile(const std::shared_ptr<RasterLayer> &tile, Error *error)
{
  const auto fail = [error](const QString &message, const QString &hint) {
    if (error)
      *error = Error {message, hint};
    return false;
  };

  if (!tile)
    return fail(QStringLiteral("No hay hoja que anadir al mosaico."),
                QStringLiteral("Comprueba que el archivo se abre como raster: gdalinfo «archivo»"));

  if (!tile->hasGeoTransform())
  {
    return fail(QStringLiteral("La hoja «%1» no tiene georreferenciacion.").arg(tile->name()),
                QStringLiteral("Anade un archivo de mundo (.jgw/.tfw) o georreferencia la imagen con "
                               "gdal_translate -a_ullr. Sin geotransformacion no se puede colocar en el mapa."));
  }

  if (containsSource(tile->sourcePath()))
    return true;

  mTiles.push_back(tile);
  invalidateCache();
  bumpRevision();
  return true;
}

bool RasterMosaic::containsSource(const QString &path) const
{
  return std::any_of(mTiles.cbegin(), mTiles.cend(), [&path](const std::shared_ptr<RasterLayer> &tile) {
    return tile && tile->sourcePath() == path;
  });
}

std::shared_ptr<RasterLayer> RasterMosaic::tile(int index) const
{
  if (index < 0 || index >= tileCount())
    return {};
  return mTiles[static_cast<size_t>(index)];
}

QString RasterMosaic::name() const
{
  if (!mName.isEmpty())
    return mName;
  if (!mDerivedName.isEmpty())
    return mDerivedName;

  // La carpeta comun de las hojas es el nombre que el usuario reconoce: la
  // cobertura, no la primera hoja que resulto estar en la lista.
  if (mTiles.empty())
    return QStringLiteral("Mosaico");

  QString commonPath = QFileInfo(mTiles.front()->sourcePath()).absolutePath();
  for (const std::shared_ptr<RasterLayer> &tile : mTiles)
  {
    if (!tile)
      continue;

    const QString tileDir = QFileInfo(tile->sourcePath()).absolutePath();
    while (!commonPath.isEmpty() && tileDir != commonPath
           && !tileDir.startsWith(commonPath + QLatin1Char('/')))
    {
      const int separator = std::max(commonPath.lastIndexOf(QLatin1Char('/')),
                                     commonPath.lastIndexOf(QLatin1Char('\\')));
      if (separator <= 0)
      {
        commonPath.clear();
        break;
      }
      commonPath.truncate(separator);
    }

    if (commonPath.isEmpty())
      break;
  }

  QString folderName = QFileInfo(commonPath).fileName();
  if (folderName.isEmpty())
    folderName = QStringLiteral("Mosaico");

  mDerivedName = folderName;
  return mDerivedName;
}

Rectangle RasterMosaic::extentInEpsg(int epsg) const
{
  const auto cached = mExtents.find(epsg);
  if (cached != mExtents.end())
    return cached->second;

  Rectangle extent;
  for (const Rectangle &tileExtent : tileExtents(epsg))
    extent.combine(tileExtent);

  mExtents[epsg] = extent;
  return extent;
}

const std::vector<Rectangle> &RasterMosaic::tileExtents(int epsg) const
{
  const auto cached = mTileExtents.find(epsg);
  if (cached != mTileExtents.end())
    return cached->second;

  std::vector<Rectangle> extents;
  extents.reserve(mTiles.size());
  for (const std::shared_ptr<RasterLayer> &tile : mTiles)
    extents.push_back(tile ? tile->extentInEpsg(epsg) : Rectangle());

  return mTileExtents.emplace(epsg, std::move(extents)).first->second;
}

void RasterMosaic::setVisible(bool visible)
{
  if (mVisible == visible)
    return;
  mVisible = visible;
  bumpRevision();
}

void RasterMosaic::setStyle(const RasterStyle &style)
{
  if (mStyle == style)
    return;
  mStyle = style;
  bumpRevision();
}

void RasterMosaic::invalidateCache()
{
  mExtents.clear();
  mTileExtents.clear();
  mDerivedName.clear();
}

bool RasterMosaic::setReplica(const QString &path, double pixelSize, const QSize &pixels)
{
  if (path.isEmpty() || !(pixelSize > 0.0) || pixels.isEmpty())
    return false;

  if (mReplicaLayer && mReplicaLayer->sourcePath() == path && mReplicaPixelSize == pixelSize)
    return true;

  Error error;
  const std::shared_ptr<RasterLayer> replica = RasterLayer::open(path, &error);
  if (!replica)
    return false;

  mReplicaLayer = replica;
  mReplicaPixelSize = pixelSize;
  mReplicaPixels = pixels;
  bumpRevision();
  return true;
}

void RasterMosaic::clearReplica()
{
  if (!mReplicaLayer)
    return;

  mReplicaLayer.reset();
  mReplicaPixelSize = 0.0;
  mReplicaPixels = QSize();
  bumpRevision();
}

} // namespace minigis
