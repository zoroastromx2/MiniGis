#include "minigismaprenderer.h"

#include "minigisstyle.h"
#include "minigisvectorlayer.h"

#include <QBrush>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <memory>

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_core.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>

namespace minigis
{
namespace
{

/** Geometria C++ a partir del handle C de OGR (misma direccion, sin copia). */
OGRGeometry *geometryFromHandle(OGRGeometryH handle)
{
  return reinterpret_cast<OGRGeometry *>(handle);
}

/** Anade un anillo (exterior o interior) a un QPainterPath ya iniciado. */
void appendRingToPath(QPainterPath &path, const MapToPixel &transform, OGRGeometryH ringHandle)
{
  if (!ringHandle)
    return;

  const int pointCount = OGR_G_GetPointCount(ringHandle);
  if (pointCount <= 0)
    return;

  path.moveTo(transform.transform(OGR_G_GetX(ringHandle, 0), OGR_G_GetY(ringHandle, 0)));
  for (int i = 1; i < pointCount; ++i)
    path.lineTo(transform.transform(OGR_G_GetX(ringHandle, i), OGR_G_GetY(ringHandle, i)));
  path.closeSubpath();
}

void paintPoint(QPainter &painter, const MapToPixel &transform, OGRGeometryH handle, const Style &style)
{
  const QPointF pixel = transform.transform(OGR_G_GetX(handle, 0), OGR_G_GetY(handle, 0));
  const double radius = std::max(1.0, style.pointSize / 2.0);

  painter.setPen(style.strokeWidth > 0.0 ? QPen(style.strokeColor, style.strokeWidth) : QPen(Qt::NoPen));
  painter.setBrush(QBrush(style.fillColor));
  painter.drawEllipse(pixel, radius, radius);
}

void paintCurve(QPainter &painter, const MapToPixel &transform, OGRGeometryH handle, const Style &style)
{
  QPainterPath path;
  const int pointCount = OGR_G_GetPointCount(handle);
  for (int i = 0; i < pointCount; ++i)
  {
    const QPointF pixel = transform.transform(OGR_G_GetX(handle, i), OGR_G_GetY(handle, i));
    if (i == 0)
      path.moveTo(pixel);
    else
      path.lineTo(pixel);
  }

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(style.strokeColor, style.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(path);
}

void paintPolygon(QPainter &painter, const MapToPixel &transform, OGRGeometryH handle, const Style &style)
{
  OGRGeometry *geometry = geometryFromHandle(handle);
  OGRCurvePolygon *polygon = geometry ? geometry->toCurvePolygon() : nullptr;
  if (!polygon)
    return;

  QPainterPath path;
  // Regla par-impar: los anillos interiores quedan como huecos.
  path.setFillRule(Qt::OddEvenFill);

  if (OGRCurve *exterior = polygon->getExteriorRingCurve())
    appendRingToPath(path, transform, reinterpret_cast<OGRGeometryH>(exterior));

  const int interiorCount = polygon->getNumInteriorRings();
  for (int i = 0; i < interiorCount; ++i)
  {
    if (OGRCurve *interior = polygon->getInteriorRingCurve(i))
      appendRingToPath(path, transform, reinterpret_cast<OGRGeometryH>(interior));
  }

  painter.setBrush(style.fillEnabled ? QBrush(style.fillColor) : Qt::NoBrush);
  painter.setPen(style.strokeWidth > 0.0
                   ? QPen(style.strokeColor, style.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                   : QPen(Qt::NoPen));
  painter.drawPath(path);
}

/** Dibuja una geometria; las colecciones se recorren recursivamente. */
void paintGeometry(QPainter &painter, const MapToPixel &transform, OGRGeometryH handle, const Style &style)
{
  if (!handle || OGR_G_IsEmpty(handle))
    return;

  switch (wkbFlatten(OGR_G_GetGeometryType(handle)))
  {
    case wkbPoint:
      paintPoint(painter, transform, handle, style);
      return;

    case wkbLineString:
    case wkbLinearRing:
    case wkbCircularString:
    case wkbCompoundCurve:
      paintCurve(painter, transform, handle, style);
      return;

    case wkbPolygon:
    case wkbTriangle:
    case wkbCurvePolygon:
      paintPolygon(painter, transform, handle, style);
      return;

    default:
    {
      OGRGeometry *geometry = geometryFromHandle(handle);
      OGRGeometryCollection *collection = geometry ? geometry->toGeometryCollection() : nullptr;
      if (!collection)
        return; // tipo no soportado (por ejemplo, superficies 3D)

      const int count = collection->getNumGeometries();
      for (int i = 0; i < count; ++i)
        paintGeometry(painter, transform, reinterpret_cast<OGRGeometryH>(collection->getGeometryRef(i)), style);
      return;
    }
  }
}

} // namespace

MapToPixel::MapToPixel(const Rectangle &extent, const QSize &size)
  : mExtent(extent)
{
  if (!extent.isValid() || size.isEmpty())
    return;

  const double pixelWidth = std::max(1, size.width());
  const double pixelHeight = std::max(1, size.height());
  const double extentWidth = std::max(extent.width(), 1e-12);
  const double extentHeight = std::max(extent.height(), 1e-12);

  // Escala uniforme: la misma en X y en Y para no deformar las geometrias.
  // mScale se guarda como pixeles por unidad de mapa.
  mScale = std::min(pixelWidth / extentWidth, pixelHeight / extentHeight);

  const double contentWidth = extentWidth * mScale;
  const double contentHeight = extentHeight * mScale;
  mTopLeft = QPointF((pixelWidth - contentWidth) / 2.0, (pixelHeight - contentHeight) / 2.0);
  mValid = true;
}

QPointF MapToPixel::transform(double x, double y) const
{
  if (!mValid)
    return {};
  // El eje Y del mapa crece hacia arriba y el de los pixeles hacia abajo.
  return QPointF(mTopLeft.x() + (x - mExtent.xMin()) * mScale,
                 mTopLeft.y() + (mExtent.yMax() - y) * mScale);
}

QPointF MapToPixel::toMap(const QPointF &pixel) const
{
  if (!mValid)
    return {};
  return QPointF(mExtent.xMin() + (pixel.x() - mTopLeft.x()) / mScale,
                 mExtent.yMax() - (pixel.y() - mTopLeft.y()) / mScale);
}

Rectangle MapRenderer::fullExtent() const
{
  Rectangle result;
  for (const std::shared_ptr<VectorLayer> &layer : mLayers)
  {
    if (!layer || !layer->isVisible())
      continue;
    result.combine(layer->extent());
  }
  return result;
}

QImage MapRenderer::render(const Rectangle &extent, const QSize &size, const QColor &background) const
{
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  image.fill(background);

  if (!extent.isValid() || size.isEmpty())
    return image;

  const MapToPixel transform(extent, size);
  if (!transform.isValid())
    return image;

  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);

  for (const std::shared_ptr<VectorLayer> &layer : mLayers)
  {
    if (!layer || !layer->isVisible())
      continue;
    paintLayer(painter, transform, *layer);
  }

  painter.end();
  return image;
}

void MapRenderer::paintLayer(QPainter &painter, const MapToPixel &transform, const VectorLayer &layer) const
{
  OGRLayerH layerHandle = layer.ogrLayerHandle();
  if (!layerHandle)
    return;

  const Style &style = layer.style();
  painter.setOpacity(std::clamp(style.opacity / 100.0, 0.0, 1.0));

  // SRC de destino en orden GIS tradicional (lon, lat / este, norte).
  OGRSpatialReference destination;
  const bool hasDestination = (mDestinationEpsg > 0) && (destination.importFromEPSG(mDestinationEpsg) == OGRERR_NONE);
  if (hasDestination)
    destination.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // SRC de origen: se clona para no alterar el de la capa.
  std::unique_ptr<OGRSpatialReference> sourceClone;
  if (const OGRSpatialReference *source = layer.spatialRef())
  {
    sourceClone.reset(source->Clone());
    if (sourceClone)
      sourceClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  }

  std::unique_ptr<OGRCoordinateTransformation> transformation;
  if (hasDestination && sourceClone && !sourceClone->IsSame(&destination))
    transformation.reset(OGRCreateCoordinateTransformation(sourceClone.get(), &destination));

  OGR_L_ResetReading(layerHandle);

  qint64 painted = 0;
  while (OGRFeatureH feature = OGR_L_GetNextFeature(layerHandle))
  {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    if (geometry)
    {
      if (transformation)
      {
        // Se clona porque la transformacion modifica la geometria in situ.
        OGRGeometry *clone = geometryFromHandle(geometry)->clone();
        if (clone)
        {
          if (clone->transform(transformation.get()) == OGRERR_NONE)
            paintGeometry(painter, transform, reinterpret_cast<OGRGeometryH>(clone), style);
          delete clone;
        }
      }
      else
      {
        paintGeometry(painter, transform, geometry, style);
      }
    }

    OGR_F_Destroy(feature);

    ++painted;
    if (mMaximumFeatures > 0 && painted >= mMaximumFeatures)
      break;
  }

  painter.setOpacity(1.0);
}

} // namespace minigis
