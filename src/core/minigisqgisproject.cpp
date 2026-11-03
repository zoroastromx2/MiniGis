#include "minigisqgisproject.h"

#include "minigisqgissymbology.h"
#include "minigisziparchive.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QUrl>
#include <QUuid>

#include <ogr_spatialref.h>

#include <algorithm>
#include <memory>

namespace minigis
{

namespace
{

bool readNumber(const XmlNode *parent, const QString &name, double *value)
{
  if (!parent)
    return false;
  const XmlNode *node = parent->child(name);
  if (!node)
    return false;
  bool ok = false;
  const double parsed = node->text().trimmed().toDouble(&ok);
  if (!ok)
    return false;
  *value = parsed;
  return true;
}

/** EPSG de un nodo que contiene (o es) un `<spatialrefsys>` de QGIS. */
int epsgFromSrs(const XmlNode *container)
{
  if (!container)
    return 0;

  const XmlNode *sys = container->child(QStringLiteral("spatialrefsys"));
  if (!sys)
    sys = container;

  QString authid = sys->child(QStringLiteral("authid")) ? sys->child(QStringLiteral("authid"))->text().trimmed() : QString();
  const int colon = authid.indexOf(QLatin1Char(':'));
  if (colon >= 0)
    authid = authid.mid(colon + 1);
  if (!authid.isEmpty())
  {
    bool ok = false;
    const int epsg = authid.toInt(&ok);
    if (ok && epsg > 0)
      return epsg;
  }

  if (const XmlNode *srid = sys->child(QStringLiteral("srid")))
  {
    bool ok = false;
    const int epsg = srid->text().trimmed().toInt(&ok);
    if (ok && epsg > 0)
      return epsg;
  }

  // Ultimo recurso: resolver el WKT con GDAL/PROJ (algunos proyectos antiguos
  // no escriben authid).
  if (const XmlNode *wkt = sys->child(QStringLiteral("wkt")))
  {
    OGRSpatialReference reference;
    if (reference.SetFromUserInput(wkt->text().trimmed().toUtf8().constData()) == OGRERR_NONE)
    {
      const char *code = reference.GetAuthorityCode(nullptr);
      if (code)
      {
        bool ok = false;
        const int epsg = QString::fromLatin1(code).toInt(&ok);
        if (ok && epsg > 0)
          return epsg;
      }
    }
  }

  return 0;
}

bool isGeographicEpsg(int epsg)
{
  if (epsg <= 0)
    return false;
  OGRSpatialReference reference;
  const QByteArray definition = QStringLiteral("EPSG:%1").arg(epsg).toUtf8();
  if (reference.SetFromUserInput(definition.constData()) != OGRERR_NONE)
    return false;
  return reference.IsGeographic() == 1;
}

/** WKT de un nodo que contiene (o es) un `<spatialrefsys>`. */
QString wktFromSrs(const XmlNode *container)
{
  if (!container)
    return QString();
  const XmlNode *sys = container->child(QStringLiteral("spatialrefsys"));
  if (!sys)
    sys = container;
  const XmlNode *wkt = sys->child(QStringLiteral("wkt"));
  return wkt ? wkt->text().trimmed() : QString();
}

bool transformRectangle(const QString &sourceWkt, const QString &destinationDefinition, Rectangle *rectangle);

bool transformRectangle(const QString &sourceWkt, int destinationEpsg, Rectangle *rectangle)
{
  return transformRectangle(sourceWkt, QStringLiteral("EPSG:%1").arg(destinationEpsg), rectangle);
}

/**
 * Convierte un rectangulo entre dos SRS. Se transforman las cuatro esquinas: es
 * una aproximacion suficiente para encuadrar, no para medir.
 */
bool transformRectangle(const QString &sourceWkt, const QString &destinationDefinition, Rectangle *rectangle)
{
  if (!rectangle || sourceWkt.isEmpty() || destinationDefinition.isEmpty())
    return false;

  OGRSpatialReference source;
  OGRSpatialReference destination;
  if (source.SetFromUserInput(sourceWkt.toUtf8().constData()) != OGRERR_NONE)
    return false;
  if (destination.SetFromUserInput(destinationDefinition.toUtf8().constData()) != OGRERR_NONE)
    return false;

  // En GDAL 3 los SRS respetan el orden de ejes oficial (lat,lon en 4326);
  // MiniGIS trabaja siempre en orden x=longitud/easting, y=latitud/northing.
  source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  destination.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  std::unique_ptr<OGRCoordinateTransformation> transformation(OGRCreateCoordinateTransformation(&source, &destination));
  if (!transformation)
    return false;

  double x[4] = {rectangle->xMin(), rectangle->xMax(), rectangle->xMin(), rectangle->xMax()};
  double y[4] = {rectangle->yMin(), rectangle->yMin(), rectangle->yMax(), rectangle->yMax()};
  if (!transformation->Transform(4, x, y))
    return false;

  double xmin = x[0];
  double xmax = x[0];
  double ymin = y[0];
  double ymax = y[0];
  for (int index = 1; index < 4; ++index)
  {
    xmin = std::min(xmin, x[index]);
    xmax = std::max(xmax, x[index]);
    ymin = std::min(ymin, y[index]);
    ymax = std::max(ymax, y[index]);
  }

  const Rectangle transformed(xmin, ymin, xmax, ymax);
  if (!transformed.isValid())
    return false;
  *rectangle = transformed;
  return true;
}

GeometryKind geometryKindFromMaplayer(const XmlNode &maplayer)
{
  QString geometry = maplayer.attribute(QStringLiteral("geometry")).toLower();
  if (geometry.isEmpty())
    geometry = maplayer.attribute(QStringLiteral("wkbType")).toLower();

  if (geometry.contains(QStringLiteral("point")))
    return GeometryKind::Point;
  if (geometry.contains(QStringLiteral("line")) || geometry.contains(QStringLiteral("curve")))
    return GeometryKind::Line;
  if (geometry.contains(QStringLiteral("polygon")))
    return GeometryKind::Polygon;
  return GeometryKind::Unknown;
}

struct TreeLeaf
{
  QString id;
  QString name;
  QString source;
  bool visible = true;
};

bool checkedValueIsVisible(const QString &checked)
{
  return !(checked == QStringLiteral("Qt::Unchecked") || checked == QStringLiteral("0")
           || checked.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
}

/** Recorre el arbol de capas en orden (arriba -> abajo) y aplana los grupos. */
void collectTreeLeaves(const XmlNode &group, QVector<TreeLeaf> *leaves)
{
  for (const XmlNode &child : group.children())
  {
    if (!child.isElement())
      continue;

    if (child.name() == QStringLiteral("layer-tree-layer"))
    {
      TreeLeaf leaf;
      leaf.id = child.attribute(QStringLiteral("id"));
      leaf.name = child.attribute(QStringLiteral("name"));
      leaf.source = child.attribute(QStringLiteral("source"));
      leaf.visible = checkedValueIsVisible(child.attribute(QStringLiteral("checked"), QStringLiteral("Qt::Checked")));
      if (!leaf.id.isEmpty())
        leaves->append(leaf);
    }
    else if (child.name() == QStringLiteral("layer-tree-group"))
    {
      collectTreeLeaves(child, leaves);
    }
  }
}

QString childText(const XmlNode *parent, const QString &name)
{
  if (!parent)
    return QString();
  const XmlNode *child = parent->child(name);
  return child ? child->text().trimmed() : QString();
}

/** Ruta relativa al proyecto, o absoluta si el destino esta en otra unidad. */
QString relativeDatasource(const QString &absolutePath, const QString &projectDirectory)
{
  QDir directory(projectDirectory);
  QString relative = directory.relativeFilePath(absolutePath);
  if (QFileInfo(relative).isAbsolute() || relative.contains(QLatin1Char(':')))
    return QDir::fromNativeSeparators(absolutePath);
  if (!relative.startsWith(QStringLiteral("..")))
    relative = QStringLiteral("./") + relative;
  return QDir::fromNativeSeparators(relative);
}

bool sameNumber(double left, double right)
{
  return std::abs(left - right) < 1e-9;
}

bool sameColor(const QColor &left, const QColor &right)
{
  return left.rgba() == right.rgba();
}

bool sameStyle(const Style &left, const Style &right)
{
  return sameColor(left.fillColor, right.fillColor) && sameColor(left.strokeColor, right.strokeColor)
         && sameNumber(left.strokeWidth, right.strokeWidth) && sameNumber(left.pointSize, right.pointSize)
         && left.opacity == right.opacity && left.fillEnabled == right.fillEnabled;
}

bool sameLabels(const LabelStyle &left, const LabelStyle &right)
{
  return left.enabled == right.enabled && left.fields == right.fields && left.separator == right.separator
         && left.sizeMode == right.sizeMode && left.fontFamily == right.fontFamily
         && sameNumber(left.fontSize, right.fontSize) && left.bold == right.bold && left.italic == right.italic
         && sameColor(left.color, right.color) && left.haloEnabled == right.haloEnabled
         && sameNumber(left.haloWidth, right.haloWidth) && sameColor(left.haloColor, right.haloColor)
         && sameNumber(left.minimumScale, right.minimumScale) && sameNumber(left.maximumScale, right.maximumScale)
         && left.placement == right.placement && sameNumber(left.offsetX, right.offsetX)
         && sameNumber(left.offsetY, right.offsetY) && left.maximumLabels == right.maximumLabels
         && sameNumber(left.minimumDistance, right.minimumDistance)
         && sameNumber(left.minimumFeatureSize, right.minimumFeatureSize)
         && left.discardDuplicates == right.discardDuplicates && left.priority == right.priority;
}

bool sameRaster(const RasterStyle &left, const RasterStyle &right)
{
  return left == right;
}

XmlNode *findMaplayerById(XmlNode *document, const QString &id)
{
  XmlNode *projectLayers = document->child(QStringLiteral("projectlayers"));
  if (!projectLayers)
    return nullptr;
  for (XmlNode *maplayer : projectLayers->childrenNamed(QStringLiteral("maplayer")))
  {
    if (childText(maplayer, QStringLiteral("id")) == id)
      return maplayer;
  }
  return nullptr;
}

void setChildText(XmlNode *parent, const QString &name, const QString &text)
{
  if (!parent)
    return;
  XmlNode *child = parent->child(name);
  if (!child)
    child = parent->appendChild(XmlNode(name));
  child->setText(text);
}

void replaceChildNode(XmlNode *parent, const QString &name, const XmlNode &node)
{
  if (!parent)
    return;
  const int index = parent->indexOfChild(name);
  if (index >= 0)
    parent->children()[index] = node;
  else
    parent->appendChild(node);
}

void removeLayerNodes(XmlNode *document, const QString &id)
{
  if (!document || id.isEmpty())
    return;

  if (XmlNode *projectLayers = document->child(QStringLiteral("projectlayers")))
  {
    QVector<XmlNode> &children = projectLayers->children();
    for (int index = children.size() - 1; index >= 0; --index)
    {
      if (children.at(index).isElement() && children.at(index).name() == QStringLiteral("maplayer")
          && childText(&children[index], QStringLiteral("id")) == id)
        children.removeAt(index);
    }
  }

  const auto removeFromTree = [&](const auto &self, XmlNode *group) -> void {
    QVector<XmlNode> &children = group->children();
    for (int index = children.size() - 1; index >= 0; --index)
    {
      XmlNode &child = children[index];
      if (!child.isElement())
        continue;
      if (child.name() == QStringLiteral("layer-tree-layer") && child.attribute(QStringLiteral("id")) == id)
        children.removeAt(index);
      else if (child.name() == QStringLiteral("layer-tree-group"))
        self(self, &child);
    }
  };
  if (XmlNode *root = document->child(QStringLiteral("layer-tree-group")))
    removeFromTree(removeFromTree, root);

  if (XmlNode *order = document->child(QStringLiteral("layerorder")))
  {
    QVector<XmlNode> &children = order->children();
    for (int index = children.size() - 1; index >= 0; --index)
    {
      if (children.at(index).isElement() && children.at(index).attribute(QStringLiteral("id")) == id)
        children.removeAt(index);
    }
  }

  if (XmlNode *customOrder = document->child(QStringLiteral("custom-order")))
  {
    QVector<XmlNode> &children = customOrder->children();
    for (int index = children.size() - 1; index >= 0; --index)
    {
      if (children.at(index).isElement() && children.at(index).name() == QStringLiteral("item")
          && children.at(index).text() == id)
        children.removeAt(index);
    }
  }

  if (XmlNode *legend = document->child(QStringLiteral("legend")))
  {
    QVector<XmlNode> &children = legend->children();
    for (int index = children.size() - 1; index >= 0; --index)
    {
      XmlNode &entry = children[index];
      if (!entry.isElement() || entry.name() != QStringLiteral("legendlayer"))
        continue;
      bool matches = false;
      if (XmlNode *fileGroup = entry.child(QStringLiteral("filegroup")))
      {
        for (XmlNode *file : fileGroup->childrenNamed(QStringLiteral("legendlayerfile")))
        {
          if (file->attribute(QStringLiteral("layerid")) == id)
            matches = true;
        }
      }
      if (matches)
        children.removeAt(index);
    }
  }
}

void setLayerVisibility(XmlNode *document, const QString &id, bool visible)
{
  const QString checked = visible ? QStringLiteral("Qt::Checked") : QStringLiteral("Qt::Unchecked");

  const auto updateTree = [&](const auto &self, XmlNode *group) -> void {
    for (XmlNode *leaf : group->childrenNamed(QStringLiteral("layer-tree-layer")))
    {
      if (leaf->attribute(QStringLiteral("id")) == id)
        leaf->setAttribute(QStringLiteral("checked"), checked);
    }
    for (XmlNode *child : group->childrenNamed(QStringLiteral("layer-tree-group")))
      self(self, child);
  };
  if (XmlNode *root = document->child(QStringLiteral("layer-tree-group")))
    updateTree(updateTree, root);

  if (XmlNode *legend = document->child(QStringLiteral("legend")))
  {
    for (XmlNode *entry : legend->childrenNamed(QStringLiteral("legendlayer")))
    {
      XmlNode *fileGroup = entry->child(QStringLiteral("filegroup"));
      if (!fileGroup)
        continue;
      for (XmlNode *file : fileGroup->childrenNamed(QStringLiteral("legendlayerfile")))
      {
        if (file->attribute(QStringLiteral("layerid")) != id)
          continue;
        file->setAttribute(QStringLiteral("visible"), visible ? QStringLiteral("1") : QStringLiteral("0"));
        entry->setAttribute(QStringLiteral("checked"), checked);
      }
    }
  }
}

void setDocumentExtent(XmlNode *document, const ProjectSnapshot &snapshot)
{
  if (!document || !snapshot.hasExtent)
    return;

  if (XmlNode *canvas = document->child(QStringLiteral("mapcanvas")))
  {
    XmlNode *extent = canvas->child(QStringLiteral("extent"));
    if (!extent)
      extent = canvas->appendChild(XmlNode(QStringLiteral("extent")));
    setChildText(extent, QStringLiteral("xmin"), QString::number(snapshot.extent.xMin(), 'g', 17));
    setChildText(extent, QStringLiteral("ymin"), QString::number(snapshot.extent.yMin(), 'g', 17));
    setChildText(extent, QStringLiteral("xmax"), QString::number(snapshot.extent.xMax(), 'g', 17));
    setChildText(extent, QStringLiteral("ymax"), QString::number(snapshot.extent.yMax(), 'g', 17));
  }

  if (XmlNode *view = document->path(
        QStringList {QStringLiteral("ProjectViewSettings"), QStringLiteral("DefaultViewExtent")}))
  {
    view->setAttribute(QStringLiteral("xmin"), QString::number(snapshot.extent.xMin(), 'g', 17));
    view->setAttribute(QStringLiteral("ymin"), QString::number(snapshot.extent.yMin(), 'g', 17));
    view->setAttribute(QStringLiteral("xmax"), QString::number(snapshot.extent.xMax(), 'g', 17));
    view->setAttribute(QStringLiteral("ymax"), QString::number(snapshot.extent.yMax(), 'g', 17));
  }
}

QString newLayerId(const QString &name)
{
  QString base = name;
  if (base.isEmpty())
    base = QStringLiteral("capa");
  for (int index = 0; index < base.size(); ++index)
  {
    const QChar character = base.at(index);
    if (!character.isLetterOrNumber() && character != QLatin1Char('_'))
      base[index] = QLatin1Char('_');
  }
  QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
  uuid.replace(QLatin1Char('-'), QLatin1Char('_'));
  return QStringLiteral("%1_%2").arg(base, uuid);
}

QString datasourceForNewLayer(const ProjectLayer &layer, const QString &targetDirectory)
{
  QString datasource = relativeDatasource(layer.sourcePath, targetDirectory);
  if (!layer.layerName.isEmpty() && !layer.sourcePath.endsWith(QStringLiteral(".shp"), Qt::CaseInsensitive))
    datasource += QStringLiteral("|layername=") + layer.layerName;
  return datasource;
}

XmlNode buildSrsNode(const ProjectLayer &layer)
{
  XmlNode srs(QStringLiteral("srs"));
  XmlNode spatial(QStringLiteral("spatialrefsys"));
  if (!layer.crsWkt.isEmpty())
    spatial.setAttribute(QStringLiteral("nativeFormat"), QStringLiteral("Wkt"));

  if (!layer.crsWkt.isEmpty())
  {
    XmlNode wkt(QStringLiteral("wkt"));
    wkt.setText(layer.crsWkt);
    spatial.appendChild(wkt);
  }
  if (layer.crsEpsg > 0)
  {
    XmlNode srid(QStringLiteral("srid"));
    srid.setText(QString::number(layer.crsEpsg));
    spatial.appendChild(srid);
    XmlNode authid(QStringLiteral("authid"));
    authid.setText(QStringLiteral("EPSG:%1").arg(layer.crsEpsg));
    spatial.appendChild(authid);
  }
  srs.appendChild(spatial);
  return srs;
}

XmlNode buildTreeLeaf(const ProjectLayer &layer, const QString &datasource)
{
  XmlNode leaf(QStringLiteral("layer-tree-layer"));
  leaf.setAttribute(QStringLiteral("id"), layer.id);
  leaf.setAttribute(QStringLiteral("name"), layer.name);
  leaf.setAttribute(QStringLiteral("source"), datasource);
  leaf.setAttribute(QStringLiteral("providerKey"), layer.type == ProjectLayer::Type::Raster
                                                     ? QStringLiteral("gdal")
                                                     : QStringLiteral("ogr"));
  leaf.setAttribute(QStringLiteral("checked"),
                    layer.visible ? QStringLiteral("Qt::Checked") : QStringLiteral("Qt::Unchecked"));
  leaf.setAttribute(QStringLiteral("expanded"), QStringLiteral("1"));
  leaf.setAttribute(QStringLiteral("legend_split_behavior"), QStringLiteral("0"));
  leaf.setAttribute(QStringLiteral("legend_exp"), QString());
  leaf.setAttribute(QStringLiteral("patch_size"), QStringLiteral("-1,-1"));
  XmlNode customProperties(QStringLiteral("customproperties"));
  customProperties.appendChild(XmlNode(QStringLiteral("Option")));
  leaf.appendChild(customProperties);
  return leaf;
}

XmlNode buildMaplayer(const ProjectLayer &layer, const QString &targetDirectory, const XmlNode *templateNode,
                      bool geographicCrs)
{
  XmlNode node = templateNode ? *templateNode : XmlNode(QStringLiteral("maplayer"));
  const bool vector = layer.type == ProjectLayer::Type::Vector;
  node.setAttribute(QStringLiteral("type"), vector ? QStringLiteral("vector") : QStringLiteral("raster"));

  if (vector)
  {
    QString geometry = QStringLiteral("Polygon");
    if (layer.geometryKind == GeometryKind::Point)
      geometry = QStringLiteral("Point");
    else if (layer.geometryKind == GeometryKind::Line)
      geometry = QStringLiteral("Line");
    node.setAttribute(QStringLiteral("geometry"), geometry);
    node.setAttribute(QStringLiteral("wkbType"),
                       layer.geometryName.isEmpty() ? geometry : layer.geometryName);
  }
  node.setAttribute(QStringLiteral("labelsEnabled"), layer.labels.enabled ? QStringLiteral("1") : QStringLiteral("0"));

  const QString datasource = datasourceForNewLayer(layer, targetDirectory);
  setChildText(&node, QStringLiteral("id"), layer.id);
  setChildText(&node, QStringLiteral("datasource"), datasource);
  setChildText(&node, QStringLiteral("layername"), layer.name);
  setChildText(&node, QStringLiteral("provider"),
               vector ? QStringLiteral("ogr") : QStringLiteral("gdal"));

  replaceChildNode(&node, QStringLiteral("srs"), buildSrsNode(layer));

  if (vector)
  {
    replaceChildNode(&node, QStringLiteral("renderer-v2"),
                     qgis::buildVectorRenderer(layer.style, layer.geometryKind));
    node.removeChild(QStringLiteral("labeling"));
    if (layer.labels.enabled)
      node.appendChild(qgis::buildLabeling(layer.labels, geographicCrs));
  }
  else
  {
    XmlNode *pipe = node.child(QStringLiteral("pipe"));
    if (!pipe)
      pipe = node.appendChild(XmlNode(QStringLiteral("pipe")));
    replaceChildNode(pipe, QStringLiteral("rasterrenderer"), qgis::buildRasterRenderer(layer.raster));
  }

  if (!node.child(QStringLiteral("blendMode")))
  {
    XmlNode blend(QStringLiteral("blendMode"));
    blend.setText(QStringLiteral("0"));
    node.appendChild(blend);
  }
  return node;
}

void addNewLayer(XmlNode *document, const ProjectLayer &layer, const QString &targetDirectory, bool geographicCrs)
{
  XmlNode *projectLayers = document->child(QStringLiteral("projectlayers"));
  if (!projectLayers)
    return;

  const XmlNode *templateNode = nullptr;
  for (const XmlNode *candidate : projectLayers->childrenNamed(QStringLiteral("maplayer")))
  {
    const bool raster = candidate->attribute(QStringLiteral("type")) == QStringLiteral("raster");
    if (raster == (layer.type == ProjectLayer::Type::Raster))
    {
      templateNode = candidate;
      break;
    }
  }

  const XmlNode maplayer = buildMaplayer(layer, targetDirectory, templateNode, geographicCrs);
  projectLayers->appendChild(maplayer);

  if (XmlNode *root = document->child(QStringLiteral("layer-tree-group")))
  {
    int index = 0;
    for (; index < root->children().size(); ++index)
    {
      const XmlNode &child = root->children().at(index);
      if (child.isElement()
          && (child.name() == QStringLiteral("layer-tree-layer") || child.name() == QStringLiteral("layer-tree-group")))
        break;
    }
    root->children().insert(index, buildTreeLeaf(layer, datasourceForNewLayer(layer, targetDirectory)));
  }

  if (XmlNode *order = document->child(QStringLiteral("layerorder")))
  {
    XmlNode item(QStringLiteral("layer"));
    item.setAttribute(QStringLiteral("id"), layer.id);
    order->children().insert(0, item);
  }
}

void applySnapshotToDocument(XmlNode *document, const ProjectSnapshot &snapshot,
                             const QVector<ProjectLayer> &loadedLayers, const QString &targetDirectory,
                             bool geographicCrs)
{
  if (!document)
    return;

  setDocumentExtent(document, snapshot);

  QSet<QString> snapshotIds;
  for (const ProjectLayer &layer : snapshot.layers)
  {
    if (!layer.id.isEmpty())
      snapshotIds.insert(layer.id);
  }

  // Una capa abrible que ya no esta en el mapa se ha cerrado: se quita. Las no
  // disponibles no estan en `loadedLayers`, asi que nunca se tocan.
  for (const ProjectLayer &loaded : loadedLayers)
  {
    if (!snapshotIds.contains(loaded.id))
      removeLayerNodes(document, loaded.id);
  }

  for (const ProjectLayer &layer : snapshot.layers)
  {
    if (layer.id.isEmpty())
    {
      ProjectLayer newLayer = layer;
      newLayer.id = newLayerId(layer.name);
      addNewLayer(document, newLayer, targetDirectory, geographicCrs);
      continue;
    }

    XmlNode *maplayer = findMaplayerById(document, layer.id);
    if (!maplayer)
      continue;

    setLayerVisibility(document, layer.id, layer.visible);

    const auto old = std::find_if(loadedLayers.cbegin(), loadedLayers.cend(),
                                  [&layer](const ProjectLayer &candidate) { return candidate.id == layer.id; });
    if (old == loadedLayers.cend())
      continue;

    if (layer.type == ProjectLayer::Type::Raster)
    {
      if (!sameRaster(old->raster, layer.raster))
      {
        XmlNode *pipe = maplayer->child(QStringLiteral("pipe"));
        if (!pipe)
          pipe = maplayer->appendChild(XmlNode(QStringLiteral("pipe")));
        replaceChildNode(pipe, QStringLiteral("rasterrenderer"), qgis::buildRasterRenderer(layer.raster));
      }
      continue;
    }

    if (!sameStyle(old->style, layer.style))
      replaceChildNode(maplayer, QStringLiteral("renderer-v2"),
                       qgis::buildVectorRenderer(layer.style, layer.geometryKind));

    if (!sameLabels(old->labels, layer.labels))
    {
      maplayer->setAttribute(QStringLiteral("labelsEnabled"), layer.labels.enabled ? QStringLiteral("1")
                                                                                   : QStringLiteral("0"));
      if (layer.labels.enabled)
        replaceChildNode(maplayer, QStringLiteral("labeling"), qgis::buildLabeling(layer.labels, geographicCrs));
      else
        maplayer->removeChild(QStringLiteral("labeling"));
    }
  }
}

} // namespace

ProjectDatasource parseProjectDatasource(const QString &datasource, const QString &projectDirectory)
{
  ProjectDatasource result;
  QString text = datasource.trimmed();
  if (text.isEmpty())
    return result;

  if (text.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive))
  {
    const QUrl url(text);
    if (url.isLocalFile())
      text = url.toLocalFile();
    else
      text = text.mid(7);
  }

  const QStringList parts = text.split(QLatin1Char('|'));
  const QString rawPath = parts.first().trimmed();
  if (rawPath.isEmpty())
    return result;

  for (int index = 1; index < parts.size(); ++index)
  {
    const QString keyValue = parts.at(index).trimmed();
    const int equals = keyValue.indexOf(QLatin1Char('='));
    if (equals < 0)
      continue;
    const QString key = keyValue.left(equals).trimmed();
    if (key.compare(QStringLiteral("layername"), Qt::CaseInsensitive) == 0)
      result.layerName = QUrl::fromPercentEncoding(keyValue.mid(equals + 1).toUtf8());
  }

  QString resolved;
  if (QFileInfo(rawPath).isAbsolute())
    resolved = QDir::cleanPath(rawPath);
  else
    resolved = QDir::cleanPath(QDir(projectDirectory).absoluteFilePath(rawPath));

  result.valid = true;
  result.rawPath = rawPath;
  result.absolutePath = QDir::fromNativeSeparators(resolved);
  return result;
}

bool QgisProject::isProjectPath(const QString &path)
{
  return path.endsWith(QStringLiteral(".qgz"), Qt::CaseInsensitive)
         || path.endsWith(QStringLiteral(".qgs"), Qt::CaseInsensitive);
}

bool QgisProject::load(const QString &path, std::unique_ptr<QgisProject> *project, Error *error)
{
  if (!project)
    return false;
  project->reset();

  const auto fail = [error](const QString &message, const QString &hint) {
    if (error)
      *error = Error {message, hint};
    return false;
  };

  auto loaded = std::unique_ptr<QgisProject>(new QgisProject);
  loaded->mPath = path;
  loaded->mCompressed = path.endsWith(QStringLiteral(".qgz"), Qt::CaseInsensitive);
  loaded->mProjectDirectory = QFileInfo(path).absolutePath();

  QByteArray xml;
  QString entryName;

  if (loaded->mCompressed)
  {
    QVector<ZipEntry> entries;
    if (!ZipArchive::read(path, &entries, error))
      return false;

    loaded->mZipEntries = entries;
    entryName = ZipArchive::projectEntryName(entries);
    if (entryName.isEmpty())
    {
      return fail(QStringLiteral("El proyecto «%1» no contiene ningun archivo .qgs.").arg(QFileInfo(path).fileName()),
                  QStringLiteral("Comprueba que el .qgz sea un proyecto de QGIS; si QGIS lo abre, vuelve a guardarlo desde QGIS."));
    }

    for (const ZipEntry &entry : entries)
    {
      if (entry.name == entryName)
      {
        xml = entry.data;
        break;
      }
    }
  }
  else
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
      return fail(QStringLiteral("No se pudo abrir el proyecto «%1»: %2").arg(QDir::toNativeSeparators(path), file.errorString()),
                  QStringLiteral("Comprueba que el archivo existe y que tienes permisos de lectura."));
    }
    xml = file.readAll();
  }

  XmlNode document;
  if (!XmlNode::parse(xml, &document, error))
    return false;

  if (document.name() != QStringLiteral("qgis"))
  {
    return fail(QStringLiteral("«%1» no parece un proyecto de QGIS.").arg(QFileInfo(path).fileName()),
                QStringLiteral("El elemento raiz deberia ser <qgis>. Comprueba que no sea un XML de otro programa."));
  }

  loaded->mDocument = document;
  loaded->mQgsEntryName = entryName;

  int projectEpsg = 0;
  QString projectWkt;
  if (const XmlNode *projectCrs = loaded->mDocument.child(QStringLiteral("projectCrs")))
  {
    projectEpsg = epsgFromSrs(projectCrs);
    if (projectEpsg <= 0)
      projectWkt = wktFromSrs(projectCrs);
  }
  loaded->mProjectEpsg = projectEpsg;

  // El lienzo puede usar un SRC distinto del proyecto. Si ninguno de los dos
  // tiene codigo EPSG (proyectos con WKT propio), se dibuja en EPSG:4326 y se
  // transforma el encuadre: las capas se reproyectan igual que en QGIS.
  int canvasEpsg = projectEpsg;
  QString canvasWkt = projectEpsg > 0 ? QString() : projectWkt;
  int extentEpsg = canvasEpsg;
  QString extentWkt = canvasWkt;

  if (const XmlNode *mapCanvas = loaded->mDocument.child(QStringLiteral("mapcanvas")))
  {
    if (const XmlNode *destination = mapCanvas->child(QStringLiteral("destinationsrs")))
    {
      const int epsg = epsgFromSrs(destination);
      if (epsg > 0)
      {
        canvasEpsg = epsg;
        extentEpsg = epsg;
        canvasWkt.clear();
        extentWkt.clear();
      }
      else
      {
        canvasWkt = wktFromSrs(destination);
        extentWkt = canvasWkt;
      }
    }

    double xmin = 0.0;
    double ymin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    const XmlNode *extent = mapCanvas->child(QStringLiteral("extent"));
    if (extent && readNumber(extent, QStringLiteral("xmin"), &xmin) && readNumber(extent, QStringLiteral("ymin"), &ymin)
        && readNumber(extent, QStringLiteral("xmax"), &xmax) && readNumber(extent, QStringLiteral("ymax"), &ymax))
    {
      Rectangle candidate(xmin, ymin, xmax, ymax);
      if (candidate.isValid() && extentEpsg <= 0 && !extentWkt.isEmpty())
      {
        if (transformRectangle(extentWkt, 4326, &candidate))
          extentEpsg = 4326;
        else
          candidate = Rectangle();
      }

      if (candidate.isValid())
      {
        loaded->mExtent = candidate;
        loaded->mExtentEpsg = extentEpsg > 0 ? extentEpsg : 4326;
        loaded->mHasExtent = true;
      }
    }
  }

  if (!loaded->mHasExtent)
  {
    if (const XmlNode *defaultExtent = loaded->mDocument.path(
          QStringList {QStringLiteral("ProjectViewSettings"), QStringLiteral("DefaultViewExtent")}))
    {
      bool ok = false;
      const double xmin = defaultExtent->attribute(QStringLiteral("xmin")).toDouble(&ok);
      double ymin = 0.0;
      double xmax = 0.0;
      double ymax = 0.0;
      bool yOk = false;
      bool xMaxOk = false;
      bool yMaxOk = false;
      if (ok)
      {
        ymin = defaultExtent->attribute(QStringLiteral("ymin")).toDouble(&yOk);
        xmax = defaultExtent->attribute(QStringLiteral("xmax")).toDouble(&xMaxOk);
        ymax = defaultExtent->attribute(QStringLiteral("ymax")).toDouble(&yMaxOk);
      }
      if (ok && yOk && xMaxOk && yMaxOk)
      {
        Rectangle candidate(xmin, ymin, xmax, ymax);
        int epsg = epsgFromSrs(defaultExtent);
        QString wkt = epsg > 0 ? QString() : wktFromSrs(defaultExtent);
        if (candidate.isValid() && epsg <= 0 && !wkt.isEmpty() && transformRectangle(wkt, 4326, &candidate))
          epsg = 4326;

        if (candidate.isValid())
        {
          loaded->mExtent = candidate;
          loaded->mHasExtent = true;
          loaded->mExtentEpsg = epsg > 0 ? epsg : 4326;
        }
      }
    }
  }

  loaded->mCanvasEpsg = canvasEpsg > 0 ? canvasEpsg : (loaded->mHasExtent ? loaded->mExtentEpsg : 4326);
  if (loaded->mCanvasEpsg <= 0)
    loaded->mCanvasEpsg = 4326;
  loaded->mCanvasSrsEpsg = canvasEpsg;
  loaded->mCanvasSrsWkt = canvasWkt;

  QVector<TreeLeaf> leaves;
  if (const XmlNode *rootGroup = loaded->mDocument.child(QStringLiteral("layer-tree-group")))
    collectTreeLeaves(*rootGroup, &leaves);

  QHash<QString, TreeLeaf> treeById;
  for (const TreeLeaf &leaf : leaves)
  {
    if (!treeById.contains(leaf.id))
      treeById.insert(leaf.id, leaf);
  }

  QVector<ProjectLayer> parsedLayers;
  QHash<QString, ProjectLayer> byId;
  QVector<UnavailableProjectLayer> unavailable;
  const XmlNode *projectLayers = loaded->mDocument.child(QStringLiteral("projectlayers"));
  int defaultStyleIndex = 0;

  // Las escalas de las etiquetas son denominadores independientes del SRC;
  // MiniGIS las compara con las unidades por pixel del lienzo, asi que la
  // conversion se hace con el SRC del proyecto, no con el de la capa.
  const bool canvasGeographic = isGeographicEpsg(loaded->mCanvasEpsg);

  if (projectLayers)
  {
    for (const XmlNode *maplayer : projectLayers->childrenNamed(QStringLiteral("maplayer")))
    {
      const QString declaredType = maplayer->attribute(QStringLiteral("type"));
      if (declaredType != QStringLiteral("vector") && declaredType != QStringLiteral("raster"))
        continue;

      ProjectLayer layer;
      layer.id = childText(maplayer, QStringLiteral("id"));
      layer.name = childText(maplayer, QStringLiteral("layername"));
      if (layer.name.isEmpty())
        layer.name = layer.id;
      layer.datasource = childText(maplayer, QStringLiteral("datasource"));
      layer.provider = childText(maplayer, QStringLiteral("provider"));
      layer.visible = treeById.contains(layer.id) ? treeById.value(layer.id).visible : true;
      layer.geometryKind = geometryKindFromMaplayer(*maplayer);
      layer.geometryName = maplayer->attribute(QStringLiteral("wkbType"));
      if (layer.geometryName.isEmpty())
        layer.geometryName = maplayer->attribute(QStringLiteral("geometry"));
      layer.crsEpsg = epsgFromSrs(maplayer->child(QStringLiteral("srs")));
      layer.crsWkt = wktFromSrs(maplayer->child(QStringLiteral("srs")));

      const ProjectDatasource datasource = parseProjectDatasource(layer.datasource, loaded->mProjectDirectory);
      layer.sourcePath = datasource.absolutePath;
      layer.layerName = datasource.layerName;

      QString reason;
      const bool providerOk = (declaredType == QStringLiteral("vector")
                                 && (layer.provider.isEmpty()
                                     || layer.provider.compare(QStringLiteral("ogr"), Qt::CaseInsensitive) == 0))
                              || (declaredType == QStringLiteral("raster")
                                  && (layer.provider.isEmpty()
                                      || layer.provider.compare(QStringLiteral("gdal"), Qt::CaseInsensitive) == 0));

      if (!datasource.valid)
        reason = QStringLiteral("el datasource esta vacio o no es una ruta valida");
      else if (!providerOk)
        reason = QStringLiteral("proveedor «%1» no soportado").arg(layer.provider);
      else if (!QFileInfo::exists(datasource.absolutePath))
        reason = QStringLiteral("no existe el archivo «%1»").arg(QDir::toNativeSeparators(datasource.absolutePath));

      if (!reason.isEmpty())
      {
        layer.type = ProjectLayer::Type::Unsupported;
        layer.unavailableReason = reason;
        unavailable.append(UnavailableProjectLayer {layer.id, layer.name, reason});
        byId.insert(layer.id, layer);
        continue;
      }

      const int srsEpsg = epsgFromSrs(maplayer->child(QStringLiteral("srs")));
      Q_UNUSED(srsEpsg);

      if (declaredType == QStringLiteral("vector"))
      {
        layer.type = ProjectLayer::Type::Vector;
        const GeometryKind kind = geometryKindFromMaplayer(*maplayer);
        layer.style = Style::defaultForKind(kind, defaultStyleIndex++);

        if (const XmlNode *renderer = maplayer->child(QStringLiteral("renderer-v2")))
          layer.styleRepresentable = qgis::readVectorStyle(*renderer, &layer.style);
        else
          layer.styleRepresentable = false;

        if (!qgis::readLabelStyle(*maplayer, canvasGeographic, &layer.labels))
        {
          layer.labelsRepresentable = false;
          layer.labels = LabelStyle();
          layer.labels.enabled = false;
        }
      }
      else
      {
        layer.type = ProjectLayer::Type::Raster;
        const XmlNode *pipe = maplayer->child(QStringLiteral("pipe"));
        const XmlNode *rasterRenderer = pipe ? pipe->child(QStringLiteral("rasterrenderer")) : nullptr;
        if (rasterRenderer)
          layer.styleRepresentable = qgis::readRasterStyle(*rasterRenderer, &layer.raster);
        else
          layer.styleRepresentable = false;
      }

      parsedLayers.append(layer);
      byId.insert(layer.id, layer);
    }
  }

  // Orden de apilado: el arbol de QGIS va de arriba abajo; MiniGIS dibuja de
  // fondo a frente. Las capas que no estan en el arbol se anaden al final.
  QSet<QString> used;
  QVector<ProjectLayer> ordered;
  for (int index = leaves.size() - 1; index >= 0; --index)
  {
    const QString id = leaves.at(index).id;
    const auto iterator = byId.constFind(id);
    if (iterator == byId.constEnd() || iterator->type == ProjectLayer::Type::Unsupported || used.contains(id))
      continue;
    ordered.append(iterator.value());
    used.insert(id);
  }
  for (const ProjectLayer &layer : parsedLayers)
  {
    if (used.contains(layer.id))
      continue;
    ordered.append(layer);
    used.insert(layer.id);
  }

  loaded->mLayers = ordered;
  loaded->mUnavailable = unavailable;

  *project = std::move(loaded);
  return true;
}

void QgisProject::rewriteDatasources(XmlNode *document, const QString &oldDirectory, const QString &newDirectory)
{
  if (!document)
    return;

  QHash<QString, QString> newDatasourceById;

  if (XmlNode *projectLayers = document->child(QStringLiteral("projectlayers")))
  {
    for (XmlNode *maplayer : projectLayers->childrenNamed(QStringLiteral("maplayer")))
    {
      XmlNode *idNode = maplayer->child(QStringLiteral("id"));
      XmlNode *datasourceNode = maplayer->child(QStringLiteral("datasource"));
      if (!idNode || !datasourceNode)
        continue;

      const ProjectDatasource datasource = parseProjectDatasource(datasourceNode->text(), oldDirectory);
      if (!datasource.valid)
        continue;

      const QString rewritten = relativeDatasource(datasource.absolutePath, newDirectory);
      datasourceNode->setText(rewritten);
      newDatasourceById.insert(idNode->text().trimmed(), rewritten);
    }
  }

  const auto updateTree = [&](const auto &self, XmlNode *group) -> void {
    for (XmlNode *leaf : group->childrenNamed(QStringLiteral("layer-tree-layer")))
    {
      const QString id = leaf->attribute(QStringLiteral("id"));
      const auto iterator = newDatasourceById.constFind(id);
      if (iterator != newDatasourceById.constEnd())
        leaf->setAttribute(QStringLiteral("source"), iterator.value());
    }
    for (XmlNode *child : group->childrenNamed(QStringLiteral("layer-tree-group")))
      self(self, child);
  };

  if (XmlNode *root = document->child(QStringLiteral("layer-tree-group")))
    updateTree(updateTree, root);
}

bool QgisProject::save(const QString &targetPath, const ProjectSnapshot &snapshot, Error *error)
{
  const auto fail = [error](const QString &message, const QString &hint) {
    if (error)
      *error = Error {message, hint};
    return false;
  };

  if (!isProjectPath(targetPath))
    return fail(QStringLiteral("«%1» no es un proyecto: usa la extension .qgz o .qgs.").arg(targetPath),
                QStringLiteral("Elige un nombre de archivo terminado en .qgz (comprimido) o .qgs (XML)."));

  const QString targetDirectory = QFileInfo(targetPath).absolutePath();
  if (!QFileInfo(targetDirectory).isDir())
    return fail(QStringLiteral("No existe la carpeta de destino: %1").arg(QDir::toNativeSeparators(targetDirectory)),
                QStringLiteral("Crea la carpeta o elige otra ubicacion para el proyecto."));

  const bool targetCompressed = targetPath.endsWith(QStringLiteral(".qgz"), Qt::CaseInsensitive);
  const QString oldDirectory = mProjectDirectory;

  // Se trabaja sobre una copia del documento: si la escritura falla, el
  // proyecto en memoria sigue apuntando a las rutas que tenia.
  XmlNode document = mDocument;
  if (QDir(oldDirectory).absolutePath().compare(QDir(targetDirectory).absolutePath(), Qt::CaseInsensitive) != 0)
    rewriteDatasources(&document, oldDirectory, targetDirectory);

  // El encuadre se guarda en el SRC real del `<mapcanvas>` (que puede ser un WKT
  // sin codigo EPSG); si el lienzo se dibujo en otro SRC, se transforma antes.
  // Si la vista no se ha movido desde la ultima lectura/guardado, no se toca:
  // asi no se acumula error al convertir de ida y vuelta.
  const auto extentsEqual = [](const Rectangle &left, const Rectangle &right) {
    const double scale = std::max({1.0, std::abs(left.xMin()), std::abs(left.yMin()),
                                   std::abs(left.xMax()), std::abs(left.yMax())});
    const double epsilon = scale * 1e-9;
    return std::abs(left.xMin() - right.xMin()) <= epsilon && std::abs(left.yMin() - right.yMin()) <= epsilon
           && std::abs(left.xMax() - right.xMax()) <= epsilon && std::abs(left.yMax() - right.yMax()) <= epsilon;
  };

  const bool extentUnchanged = snapshot.hasExtent && mHasExtent && snapshot.crsEpsg == mCanvasEpsg
                               && extentsEqual(snapshot.extent, mExtent);

  ProjectSnapshot effective = snapshot;
  if (extentUnchanged)
  {
    effective.hasExtent = false;
  }
  else if (effective.hasExtent)
  {
    const QString storedDefinition = mCanvasSrsEpsg > 0 ? QStringLiteral("EPSG:%1").arg(mCanvasSrsEpsg)
                                                        : mCanvasSrsWkt;
    if (storedDefinition.isEmpty())
    {
      effective.hasExtent = false;
    }
    else if (!(mCanvasSrsEpsg > 0 && effective.crsEpsg == mCanvasSrsEpsg))
    {
      Rectangle converted = effective.extent;
      const QString sourceDefinition = QStringLiteral("EPSG:%1").arg(effective.crsEpsg);
      if (transformRectangle(sourceDefinition, storedDefinition, &converted))
        effective.extent = converted;
      else
        effective.hasExtent = false;
    }
  }

  applySnapshotToDocument(&document, effective, mLayers, targetDirectory, isGeographicEpsg(mCanvasEpsg));

  const QByteArray xml = document.toXml();
  if (xml.isEmpty())
    return fail(QStringLiteral("No se pudo serializar el proyecto en XML."),
                QStringLiteral("Es un fallo interno de MiniGIS; informa del proyecto que estabas guardando."));

  QString projectEntry;
  QVector<ZipEntry> entries;
  if (targetCompressed)
  {
    if (mCompressed)
    {
      for (const ZipEntry &entry : mZipEntries)
      {
        if (entry.name != mQgsEntryName && !entry.isDirectory())
          entries.append(entry);
      }
    }
    projectEntry = QFileInfo(targetPath).completeBaseName() + QStringLiteral(".qgs");
    entries.append(ZipEntry {projectEntry, xml});
  }

  const QString tempPath = targetDirectory + QLatin1Char('/') + QLatin1Char('.')
                           + QFileInfo(targetPath).fileName() + QStringLiteral(".tmp");
  QFile::remove(tempPath);

  if (targetCompressed)
  {
    if (!ZipArchive::write(tempPath, entries, error))
    {
      QFile::remove(tempPath);
      return false;
    }
  }
  else
  {
    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      return fail(QStringLiteral("No se pudo escribir el proyecto: %1").arg(tempFile.errorString()),
                  QStringLiteral("Comprueba que la carpeta de destino tenga permisos de escritura."));
    }
    if (tempFile.write(xml) != xml.size() || !tempFile.flush())
    {
      const QString detail = tempFile.errorString();
      tempFile.close();
      QFile::remove(tempPath);
      return fail(QStringLiteral("No se pudo escribir el proyecto: %1").arg(detail),
                  QStringLiteral("Comprueba el espacio libre y los permisos de la carpeta."));
    }
    tempFile.close();
  }

  if (QFileInfo::exists(targetPath))
  {
    const QString backupPath = targetPath + QStringLiteral(".bak");
    QFile::remove(backupPath);
    if (!QFile::copy(targetPath, backupPath))
    {
      QFile::remove(tempPath);
      return fail(QStringLiteral("No se pudo crear la copia de seguridad «%1».").arg(QDir::toNativeSeparators(backupPath)),
                  QStringLiteral("Comprueba los permisos de la carpeta; el proyecto original no se ha tocado."));
    }
  }

  if (QFileInfo::exists(targetPath) && !QFile::remove(targetPath))
  {
    QFile::remove(tempPath);
    return fail(QStringLiteral("No se pudo sustituir «%1».").arg(QDir::toNativeSeparators(targetPath)),
                QStringLiteral("Cierra el archivo si lo tienes abierto en otro programa y vuelve a intentarlo."));
  }

  if (!QFile::rename(tempPath, targetPath))
  {
    QFile::remove(tempPath);
    return fail(QStringLiteral("No se pudo guardar «%1».").arg(QDir::toNativeSeparators(targetPath)),
                QStringLiteral("El proyecto anterior sigue en «%1.bak»; renombralo para recuperarlo.")
                  .arg(QDir::toNativeSeparators(targetPath)));
  }

  mPath = targetPath;
  mCompressed = targetCompressed;
  mQgsEntryName = targetCompressed ? projectEntry : QString();
  mProjectDirectory = targetDirectory;
  mDocument = document;
  if (!extentUnchanged && snapshot.hasExtent && effective.hasExtent)
  {
    mExtent = snapshot.extent;
    mExtentEpsg = snapshot.crsEpsg;
    mHasExtent = true;
  }
  if (targetCompressed)
    mZipEntries = entries;
  else
    mZipEntries.clear();

  return true;
}

bool QgisProject::save(const QString &targetPath, Error *error)
{
  ProjectSnapshot snapshot;
  snapshot.crsEpsg = mCanvasEpsg;
  snapshot.hasExtent = mHasExtent;
  snapshot.extent = mExtent;
  snapshot.layers = mLayers;
  return save(targetPath, snapshot, error);
}

QByteArray projectSnapshotSignature(const ProjectSnapshot &snapshot)
{
  QByteArray signature;
  const auto append = [&signature](const QString &text) {
    signature.append(text.toUtf8());
    signature.append('\n');
  };

  append(QStringLiteral("crs=%1").arg(snapshot.crsEpsg));
  if (snapshot.hasExtent)
  {
    append(QStringLiteral("extent=%1,%2,%3,%4")
             .arg(QString::number(snapshot.extent.xMin(), 'g', 17),
                  QString::number(snapshot.extent.yMin(), 'g', 17),
                  QString::number(snapshot.extent.xMax(), 'g', 17),
                  QString::number(snapshot.extent.yMax(), 'g', 17)));
  }

  for (const ProjectLayer &layer : snapshot.layers)
  {
    append(QStringLiteral("layer=%1|%2|%3|%4|%5")
             .arg(layer.id, QString::number(static_cast<int>(layer.type)), layer.name,
                  layer.visible ? QStringLiteral("1") : QStringLiteral("0"),
                  layer.sourcePath));
    if (layer.type == ProjectLayer::Type::Raster)
    {
      append(QStringLiteral("raster=%1,%2,%3,%4,%5,%6,%7,%8,%9")
               .arg(layer.raster.redBand)
               .arg(layer.raster.greenBand)
               .arg(layer.raster.blueBand)
               .arg(layer.raster.alphaBand)
               .arg(static_cast<int>(layer.raster.bandMode))
               .arg(static_cast<int>(layer.raster.stretch))
               .arg(layer.raster.opacity)
               .arg(layer.raster.useCustomRange ? QStringLiteral("1") : QStringLiteral("0"))
               .arg(QStringLiteral("%1,%2").arg(QString::number(layer.raster.minValue, 'g', 17),
                                                QString::number(layer.raster.maxValue, 'g', 17))));
      continue;
    }

    append(QStringLiteral("style=%1,%2,%3,%4,%5,%6")
             .arg(layer.style.fillColor.name(QColor::HexArgb), layer.style.strokeColor.name(QColor::HexArgb),
                  QString::number(layer.style.strokeWidth, 'g', 17),
                  QString::number(layer.style.pointSize, 'g', 17))
             .arg(layer.style.opacity)
             .arg(layer.style.fillEnabled ? QStringLiteral("1") : QStringLiteral("0")));
    QString labelsSignature = QStringLiteral("labels=%1|%2|%3|%4|%5|%6|%7|%8|%9")
                                .arg(layer.labels.enabled ? QStringLiteral("1") : QStringLiteral("0"),
                                     layer.labels.fields.join(QLatin1Char(',')),
                                     QString::number(layer.labels.fontSize, 'g', 17),
                                     layer.labels.color.name(QColor::HexArgb),
                                     QString::number(layer.labels.haloWidth, 'g', 17),
                                     QString::number(layer.labels.minimumScale, 'g', 17),
                                     QString::number(layer.labels.maximumScale, 'g', 17),
                                     QString::number(layer.labels.offsetX, 'g', 17),
                                     QString::number(layer.labels.offsetY, 'g', 17));
    labelsSignature += QStringLiteral("|%1").arg(layer.labels.priority);
    append(labelsSignature);
  }

  return signature;
}

} // namespace minigis
