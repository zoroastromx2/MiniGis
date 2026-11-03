#include "minigisqgissymbology.h"

#include "minigisxmltree.h"

#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace minigis
{
namespace qgis
{

namespace
{

/** Metros por unidad de un SRC geografico, convencion de QGIS para escalas. */
constexpr double kMetersPerDegree = 111319.49079327358;

bool readDouble(const QString &text, double *value)
{
  bool ok = false;
  const double parsed = text.toDouble(&ok);
  if (!ok)
    return false;
  *value = parsed;
  return true;
}

bool readInt(const QString &text, int *value)
{
  bool ok = false;
  const int parsed = text.toInt(&ok);
  if (!ok)
    return false;
  *value = parsed;
  return true;
}

double lengthToPixels(double value, const QString &unit)
{
  const QString normalized = unit.trimmed().toUpper();
  if (normalized == QStringLiteral("MM") || normalized.isEmpty())
    return millimetersToPixels(value);
  if (normalized == QStringLiteral("POINT") || normalized == QStringLiteral("POINTS"))
    return pointsToPixels(value);
  if (normalized == QStringLiteral("PIXEL") || normalized == QStringLiteral("PIXELS"))
    return value;
  // MapUnit no se puede convertir sin conocer la escala; se deja el valor.
  return value;
}

QHash<QString, QString> optionMap(const XmlNode &layer)
{
  QHash<QString, QString> values;

  // QGIS escribe las opciones dentro de un unico <Option type="Map">; algunos
  // documentos antiguos las ponen directamente como hijos de la capa.
  const auto collect = [&values](const XmlNode &parent, const auto &self) -> void {
    for (const XmlNode *option : parent.childrenNamed(QStringLiteral("Option")))
    {
      const QString name = option->attribute(QStringLiteral("name"));
      if (!name.isEmpty())
        values.insert(name, option->attribute(QStringLiteral("value")));
      else
        self(*option, self);
    }
  };
  collect(layer, collect);

  return values;
}

/** Unidades de QGIS para el tamano de la fuente a la forma de MiniGIS. */
void readFontSize(const XmlNode &textStyle, LabelStyle *style)
{
  double size = 10.0;
  readDouble(textStyle.attribute(QStringLiteral("fontSize")), &size);
  if (size <= 0.0)
    size = 10.0;

  const QString unit = textStyle.attribute(QStringLiteral("fontSizeUnit"), QStringLiteral("Point"));
  if (unit.compare(QStringLiteral("MapUnit"), Qt::CaseInsensitive) == 0)
  {
    style->sizeMode = LabelStyle::SizeMode::MapUnits;
    style->fontSize = size;
    return;
  }

  style->sizeMode = LabelStyle::SizeMode::Screen;
  if (unit.compare(QStringLiteral("MM"), Qt::CaseInsensitive) == 0)
    style->fontSize = size * (96.0 / 25.4);
  else if (unit.compare(QStringLiteral("Pixel"), Qt::CaseInsensitive) == 0)
    style->fontSize = size * (72.0 / 96.0);
  else
    style->fontSize = size;
}

/**
 * Divide una expresion `concat(...)` en sus argumentos de nivel superior.
 * Respeta las comillas dobles (campos) y simples (literales).
 */
bool splitConcatArgs(const QString &inner, QStringList *arguments)
{
  QString current;
  QChar quote;
  for (int index = 0; index < inner.size(); ++index)
  {
    const QChar character = inner.at(index);
    if (!quote.isNull())
    {
      current.append(character);
      if (character == quote)
      {
        // Una comilla doblada es una comilla literal dentro del texto.
        if (index + 1 < inner.size() && inner.at(index + 1) == quote)
        {
          current.append(quote);
          ++index;
        }
        else
        {
          quote = QChar();
        }
      }
      continue;
    }

    if (character == QLatin1Char('"') || character == QLatin1Char('\''))
    {
      quote = character;
      current.append(character);
      continue;
    }

    if (character == QLatin1Char(','))
    {
      arguments->append(current.trimmed());
      current.clear();
      continue;
    }

    current.append(character);
  }

  if (!quote.isNull())
    return false;

  if (!current.trimmed().isEmpty())
    arguments->append(current.trimmed());
  return true;
}

QString unquote(const QString &argument, QChar quote)
{
  QString text = argument;
  if (text.size() < 2 || text.startsWith(quote) == false || text.endsWith(quote) == false)
    return QString();
  text = text.mid(1, text.size() - 2);
  text.replace(QString(quote) + quote, QString(quote));
  return text;
}

/**
 * Interpreta la expresion que escribe MiniGIS para varios campos:
 * `concat("uno", ' · ', "dos")`. Devuelve false para cualquier otra expresion.
 */
bool parseConcatExpression(const QString &expression, QStringList *fields, QString *separator)
{
  const QString text = expression.trimmed();
  if (!text.startsWith(QStringLiteral("concat("), Qt::CaseInsensitive) || !text.endsWith(QLatin1Char(')')))
    return false;

  QStringList arguments;
  if (!splitConcatArgs(text.mid(7, text.size() - 8), &arguments) || arguments.isEmpty())
    return false;

  QStringList parsedFields;
  QString parsedSeparator;
  for (const QString &argument : arguments)
  {
    if (argument.startsWith(QLatin1Char('"')) && argument.endsWith(QLatin1Char('"')))
    {
      const QString field = unquote(argument, QLatin1Char('"'));
      if (field.isEmpty())
        return false;
      parsedFields << field;
    }
    else if (argument.startsWith(QLatin1Char('\'')) && argument.endsWith(QLatin1Char('\'')))
    {
      if (parsedSeparator.isEmpty())
        parsedSeparator = unquote(argument, QLatin1Char('\''));
    }
    else
    {
      return false;
    }
  }

  if (parsedFields.isEmpty())
    return false;

  *fields = parsedFields;
  if (!parsedSeparator.isEmpty())
    *separator = parsedSeparator;
  return true;
}

int qgisPriorityToMiniGis(int priority)
{
  return std::clamp(10 - priority, 0, 10);
}

QString number(double value)
{
  return QString::number(value, 'g', 17);
}

XmlNode makeOption(const QString &name, const QString &value)
{
  XmlNode option(QStringLiteral("Option"));
  option.setAttribute(QStringLiteral("type"), QStringLiteral("QString"));
  option.setAttribute(QStringLiteral("name"), name);
  option.setAttribute(QStringLiteral("value"), value);
  return option;
}

XmlNode makeDataDefinedProperties()
{
  XmlNode properties(QStringLiteral("data_defined_properties"));
  XmlNode map(QStringLiteral("Option"));
  map.setAttribute(QStringLiteral("type"), QStringLiteral("Map"));
  map.appendChild(makeOption(QStringLiteral("name"), QString()));
  map.appendChild(XmlNode(QStringLiteral("Option")));
  map.appendChild(makeOption(QStringLiteral("type"), QStringLiteral("collection")));
  properties.appendChild(map);
  return properties;
}

XmlNode makeOptionsMap(const QVector<QPair<QString, QString>> &options)
{
  XmlNode map(QStringLiteral("Option"));
  map.setAttribute(QStringLiteral("type"), QStringLiteral("Map"));
  for (const auto &option : options)
    map.appendChild(makeOption(option.first, option.second));
  return map;
}

QString escapedExpressionField(const QString &field)
{
  QString escaped = field;
  escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
  return QStringLiteral("\"%1\"").arg(escaped);
}

QString labelExpression(const QStringList &fields, const QString &separator)
{
  QString escapedSeparator = separator;
  escapedSeparator.replace(QLatin1Char('\''), QStringLiteral("''"));
  QStringList parts;
  parts << escapedExpressionField(fields.first());
  for (int index = 1; index < fields.size(); ++index)
  {
    parts << QStringLiteral("'%1'").arg(escapedSeparator);
    parts << escapedExpressionField(fields.at(index));
  }
  return QStringLiteral("concat(%1)").arg(parts.join(QStringLiteral(", ")));
}

QString symbolTypeForKind(GeometryKind kind)
{
  switch (kind)
  {
    case GeometryKind::Point:
      return QStringLiteral("marker");
    case GeometryKind::Line:
      return QStringLiteral("line");
    case GeometryKind::Polygon:
      return QStringLiteral("fill");
    case GeometryKind::Unknown:
      break;
  }
  return QStringLiteral("marker");
}

QString classForKind(GeometryKind kind)
{
  switch (kind)
  {
    case GeometryKind::Point:
      return QStringLiteral("SimpleMarker");
    case GeometryKind::Line:
      return QStringLiteral("SimpleLine");
    case GeometryKind::Polygon:
      return QStringLiteral("SimpleFill");
    case GeometryKind::Unknown:
      break;
  }
  return QStringLiteral("SimpleMarker");
}

} // namespace

QColor parseColor(const QString &text, bool *ok)
{
  const QStringList parts = text.split(QLatin1Char(','));
  if (parts.size() < 3)
  {
    if (ok)
      *ok = false;
    return QColor();
  }

  int values[4] = {0, 0, 0, 255};
  const int count = std::min(4, static_cast<int>(parts.size()));
  for (int index = 0; index < count; ++index)
  {
    bool parsed = false;
    const int value = parts.at(index).trimmed().toInt(&parsed);
    if (!parsed)
    {
      if (ok)
        *ok = false;
      return QColor();
    }
    values[index] = std::clamp(value, 0, 255);
  }

  if (ok)
    *ok = true;
  return QColor(values[0], values[1], values[2], values[3]);
}

QString colorText(const QColor &color)
{
  return QStringLiteral("%1,%2,%3,%4").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

double millimetersToPixels(double millimeters)
{
  return millimeters * (96.0 / 25.4);
}

double pixelsToMillimeters(double pixels)
{
  return pixels * (25.4 / 96.0);
}

double pointsToPixels(double points)
{
  return points * (96.0 / 72.0);
}

double scaleToMapUnitsPerPixel(double scale, bool geographicCrs)
{
  if (scale <= 0.0)
    return 0.0;
  const double metersPerUnit = geographicCrs ? kMetersPerDegree : 1.0;
  return scale * 0.0254 / (96.0 * metersPerUnit);
}

double mapUnitsPerPixelToScale(double mapUnitsPerPixel, bool geographicCrs)
{
  if (mapUnitsPerPixel <= 0.0)
    return 0.0;
  const double metersPerUnit = geographicCrs ? kMetersPerDegree : 1.0;
  return mapUnitsPerPixel * 96.0 * metersPerUnit / 0.0254;
}

bool readVectorStyle(const XmlNode &renderer, Style *style)
{
  if (!style || renderer.name() != QStringLiteral("renderer-v2"))
    return false;
  if (renderer.attribute(QStringLiteral("type")) != QStringLiteral("singleSymbol"))
    return false;

  const XmlNode *symbols = renderer.child(QStringLiteral("symbols"));
  if (!symbols)
    return false;

  const QVector<const XmlNode *> symbolList = symbols->childrenNamed(QStringLiteral("symbol"));
  if (symbolList.size() != 1)
    return false;

  const XmlNode &symbol = *symbolList.first();
  const QVector<const XmlNode *> symbolLayers = symbol.childrenNamed(QStringLiteral("layer"));
  if (symbolLayers.size() != 1)
    return false;

  const XmlNode &layer = *symbolLayers.first();
  const QString layerClass = layer.attribute(QStringLiteral("class"));
  const QHash<QString, QString> options = optionMap(layer);

  double symbolAlpha = 1.0;
  readDouble(symbol.attribute(QStringLiteral("alpha"), QStringLiteral("1")), &symbolAlpha);
  symbolAlpha = std::clamp(symbolAlpha, 0.0, 1.0);

  const auto colorOption = [&options](const QString &name) {
    const auto iterator = options.constFind(name);
    return iterator == options.constEnd() ? QString() : iterator.value();
  };

  Style result = *style;
  result.fillEnabled = true;

  if (layerClass == QStringLiteral("SimpleFill"))
  {
    bool colorOk = false;
    const QColor fill = parseColor(colorOption(QStringLiteral("color")), &colorOk);
    if (!colorOk)
      return false;
    bool outlineOk = false;
    const QColor outline = parseColor(colorOption(QStringLiteral("outline_color")), &outlineOk);
    if (!outlineOk)
      return false;

    double width = 0.0;
    readDouble(colorOption(QStringLiteral("outline_width")), &width);
    const QString widthUnit = colorOption(QStringLiteral("outline_width_unit"));
    const QString outlineStyle = colorOption(QStringLiteral("outline_style"));
    const QString fillStyle = colorOption(QStringLiteral("style"));

    result.fillEnabled = fillStyle.compare(QStringLiteral("no"), Qt::CaseInsensitive) != 0;
    result.fillColor = QColor(fill.red(), fill.green(), fill.blue(), 255);
    result.strokeColor = QColor(outline.red(), outline.green(), outline.blue(), 255);
    result.strokeWidth = outlineStyle.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0
                           ? 0.0
                           : lengthToPixels(width, widthUnit);
    result.opacity = static_cast<int>(std::lround(symbolAlpha * fill.alphaF() * 100.0));
    result.opacity = std::clamp(result.opacity, 0, 100);
    *style = result;
    return true;
  }

  if (layerClass == QStringLiteral("SimpleLine"))
  {
    bool colorOk = false;
    const QColor line = parseColor(colorOption(QStringLiteral("line_color")), &colorOk);
    if (!colorOk)
      return false;

    double width = 0.0;
    readDouble(colorOption(QStringLiteral("line_width")), &width);
    const QString widthUnit = colorOption(QStringLiteral("line_width_unit"));

    result.fillEnabled = false;
    result.strokeColor = QColor(line.red(), line.green(), line.blue(), 255);
    result.strokeWidth = lengthToPixels(width, widthUnit);
    result.opacity = static_cast<int>(std::lround(symbolAlpha * line.alphaF() * 100.0));
    result.opacity = std::clamp(result.opacity, 0, 100);
    *style = result;
    return true;
  }

  if (layerClass == QStringLiteral("SimpleMarker"))
  {
    bool colorOk = false;
    const QColor fill = parseColor(colorOption(QStringLiteral("color")), &colorOk);
    if (!colorOk)
      return false;
    bool outlineOk = false;
    const QColor outline = parseColor(colorOption(QStringLiteral("outline_color")), &outlineOk);
    if (!outlineOk)
      return false;

    double size = 0.0;
    readDouble(colorOption(QStringLiteral("size")), &size);
    const QString sizeUnit = colorOption(QStringLiteral("size_unit"));
    double width = 0.0;
    readDouble(colorOption(QStringLiteral("outline_width")), &width);
    const QString widthUnit = colorOption(QStringLiteral("outline_width_unit"));

    result.fillColor = QColor(fill.red(), fill.green(), fill.blue(), 255);
    result.strokeColor = QColor(outline.red(), outline.green(), outline.blue(), 255);
    result.pointSize = std::max(1.0, lengthToPixels(size, sizeUnit));
    result.strokeWidth = lengthToPixels(width, widthUnit);
    result.opacity = static_cast<int>(std::lround(symbolAlpha * fill.alphaF() * 100.0));
    result.opacity = std::clamp(result.opacity, 0, 100);
    *style = result;
    return true;
  }

  return false;
}

bool readLabelStyle(const XmlNode &maplayer, bool geographicCrs, LabelStyle *style)
{
  if (!style)
    return false;

  LabelStyle result;
  result.enabled = false;
  result.fields.clear();

  const QString labelsEnabled = maplayer.attribute(QStringLiteral("labelsEnabled"), QStringLiteral("0"));
  if (labelsEnabled != QStringLiteral("1") && labelsEnabled.compare(QStringLiteral("true"), Qt::CaseInsensitive) != 0)
  {
    *style = result;
    return true;
  }

  const XmlNode *labeling = maplayer.child(QStringLiteral("labeling"));
  if (!labeling)
  {
    *style = result;
    return true;
  }

  const QString labelingType = labeling->attribute(QStringLiteral("type"), QStringLiteral("simple"));
  const XmlNode *settings = nullptr;

  if (labelingType == QStringLiteral("simple"))
  {
    settings = labeling->child(QStringLiteral("settings"));
  }
  else if (labelingType == QStringLiteral("basic"))
  {
    const XmlNode *styles = labeling->child(QStringLiteral("styles"));
    if (!styles)
      return false;

    const QVector<const XmlNode *> styleNodes = styles->childrenNamed(QStringLiteral("style"));
    int enabledCount = 0;
    for (const XmlNode *styleNode : styleNodes)
    {
      if (styleNode->attribute(QStringLiteral("enabled"), QStringLiteral("1")) != QStringLiteral("0"))
      {
        ++enabledCount;
        settings = styleNode->child(QStringLiteral("settings"));
      }
    }
    if (enabledCount != 1)
      return false;
  }
  else
  {
    return false;
  }

  if (!settings)
    return false;

  const XmlNode *textStyle = settings->child(QStringLiteral("text-style"));
  if (!textStyle)
    return false;

  const QString fieldName = textStyle->attribute(QStringLiteral("fieldName"));
  const QString isExpression = textStyle->attribute(QStringLiteral("isExpression"), QStringLiteral("0"));
  if (isExpression == QStringLiteral("1") || isExpression.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
  {
    if (!parseConcatExpression(fieldName, &result.fields, &result.separator))
      return false;
  }
  else if (!fieldName.isEmpty())
  {
    result.fields = QStringList {fieldName};
  }

  result.enabled = true;
  result.fontFamily = textStyle->attribute(QStringLiteral("fontFamily"));
  readFontSize(*textStyle, &result);

  const int weight = textStyle->attribute(QStringLiteral("fontWeight"), QStringLiteral("50")).toInt();
  result.bold = weight >= 75;
  result.italic = textStyle->attribute(QStringLiteral("fontItalic"), QStringLiteral("0")) == QStringLiteral("1");

  bool colorOk = false;
  const QColor textColor = parseColor(textStyle->attribute(QStringLiteral("textColor")), &colorOk);
  if (colorOk)
    result.color = textColor;

  if (const XmlNode *buffer = textStyle->child(QStringLiteral("text-buffer")))
  {
    result.haloEnabled = buffer->attribute(QStringLiteral("bufferDraw"), QStringLiteral("0")) == QStringLiteral("1");
    double size = 1.5;
    readDouble(buffer->attribute(QStringLiteral("bufferSize")), &size);
    result.haloWidth = lengthToPixels(size, buffer->attribute(QStringLiteral("bufferSizeUnits"), QStringLiteral("MM")));
    bool haloOk = false;
    const QColor haloColor = parseColor(buffer->attribute(QStringLiteral("bufferColor")), &haloOk);
    if (haloOk)
      result.haloColor = haloColor;
  }

  if (const XmlNode *placement = settings->child(QStringLiteral("placement")))
  {
    const int placementValue = placement->attribute(QStringLiteral("placement"), QStringLiteral("0")).toInt();
    if (placementValue == 1)
      result.placement = LabelStyle::Placement::OverPoint;
    else if (placementValue == 2)
      result.placement = LabelStyle::Placement::Line;
    else
      result.placement = LabelStyle::Placement::AroundPoint;

    double offsetX = 0.0;
    double offsetY = 0.0;
    readDouble(placement->attribute(QStringLiteral("xOffset")), &offsetX);
    readDouble(placement->attribute(QStringLiteral("yOffset")), &offsetY);
    const QString offsetUnit = placement->attribute(QStringLiteral("offsetUnits"), QStringLiteral("MM"));
    result.offsetX = lengthToPixels(offsetX, offsetUnit);
    // En QGIS el eje Y crece hacia arriba; en MiniGIS, hacia abajo.
    result.offsetY = -lengthToPixels(offsetY, offsetUnit);
    result.priority = qgisPriorityToMiniGis(placement->attribute(QStringLiteral("priority"), QStringLiteral("5")).toInt());
  }

  if (const XmlNode *rendering = settings->child(QStringLiteral("rendering")))
  {
    const bool scaleVisibility = rendering->attribute(QStringLiteral("scaleVisibility"), QStringLiteral("0")) == QStringLiteral("1");
    if (scaleVisibility)
    {
      double scaleMin = 0.0;
      double scaleMax = 0.0;
      readDouble(rendering->attribute(QStringLiteral("scaleMin")), &scaleMin);
      readDouble(rendering->attribute(QStringLiteral("scaleMax")), &scaleMax);
      // En QGIS scaleMin es el limite mas alejado y scaleMax el mas cercano;
      // MiniGIS guarda el alejado en minimumScale y el cercano en maximumScale.
      result.minimumScale = scaleToMapUnitsPerPixel(scaleMin, geographicCrs);
      result.maximumScale = scaleToMapUnitsPerPixel(scaleMax, geographicCrs);
    }

    if (rendering->attribute(QStringLiteral("drawLabels"), QStringLiteral("1")) == QStringLiteral("0"))
      result.enabled = false;

    const int maximumLabels = rendering->attribute(QStringLiteral("maxNumLabels"), QStringLiteral("2000")).toInt();
    result.maximumLabels = maximumLabels;

    double minimumFeatureSize = 0.0;
    if (readDouble(rendering->attribute(QStringLiteral("minFeatureSize")), &minimumFeatureSize))
      result.minimumFeatureSize = millimetersToPixels(minimumFeatureSize);
  }

  // Ajustes propios que QGIS no modela; se guardan en customproperties para no
  // perderlos al reabrir el proyecto en MiniGIS.
  if (const XmlNode *customProperties = maplayer.child(QStringLiteral("customproperties")))
  {
    for (const XmlNode *map : customProperties->childrenNamed(QStringLiteral("Option")))
    {
      for (const XmlNode *option : map->childrenNamed(QStringLiteral("Option")))
      {
        const QString name = option->attribute(QStringLiteral("name"));
        const QString value = option->attribute(QStringLiteral("value"));
        if (name == QStringLiteral("MiniGIS/labelMinimumDistance"))
        {
          double distance = result.minimumDistance;
          if (readDouble(value, &distance))
            result.minimumDistance = distance;
        }
        else if (name == QStringLiteral("MiniGIS/labelDiscardDuplicates"))
        {
          result.discardDuplicates = value != QStringLiteral("0");
        }
      }
    }
  }

  *style = result;
  return true;
}

bool readRasterStyle(const XmlNode &rasterRenderer, RasterStyle *style)
{
  if (!style)
    return false;

  const QString type = rasterRenderer.attribute(QStringLiteral("type"));
  RasterStyle result;

  if (type == QStringLiteral("multibandcolor"))
  {
    int red = 0;
    int green = 0;
    int blue = 0;
    if (!readInt(rasterRenderer.attribute(QStringLiteral("redBand")), &red)
        || !readInt(rasterRenderer.attribute(QStringLiteral("greenBand")), &green)
        || !readInt(rasterRenderer.attribute(QStringLiteral("blueBand")), &blue))
      return false;

    result.bandMode = RasterStyle::BandMode::Rgb;
    result.redBand = red;
    result.greenBand = green;
    result.blueBand = blue;
  }
  else if (type == QStringLiteral("singlebandgray"))
  {
    int gray = 0;
    if (!readInt(rasterRenderer.attribute(QStringLiteral("grayBand")), &gray))
      return false;
    result.bandMode = RasterStyle::BandMode::Grayscale;
    result.redBand = gray;
    result.greenBand = gray;
    result.blueBand = gray;
  }
  else
  {
    return false;
  }

  int alphaBand = -1;
  if (readInt(rasterRenderer.attribute(QStringLiteral("alphaBand")), &alphaBand) && alphaBand > 0)
    result.alphaBand = alphaBand;

  double opacity = 1.0;
  if (readDouble(rasterRenderer.attribute(QStringLiteral("opacity")), &opacity))
    result.opacity = std::clamp(static_cast<int>(std::lround(opacity * 100.0)), 0, 100);

  struct Contrast
  {
    bool present = false;
    QString algorithm;
    double minimum = 0.0;
    double maximum = 0.0;
  };

  const auto readContrast = [&rasterRenderer](const QString &name) {
    Contrast contrast;
    const XmlNode *node = rasterRenderer.child(name);
    if (!node)
      return contrast;
    contrast.present = true;
    contrast.algorithm = node->child(QStringLiteral("algorithm")) ? node->child(QStringLiteral("algorithm"))->text() : QString();
    readDouble(node->child(QStringLiteral("minValue")) ? node->child(QStringLiteral("minValue"))->text() : QString(),
               &contrast.minimum);
    readDouble(node->child(QStringLiteral("maxValue")) ? node->child(QStringLiteral("maxValue"))->text() : QString(),
               &contrast.maximum);
    return contrast;
  };

  QVector<Contrast> contrasts;
  if (result.bandMode == RasterStyle::BandMode::Rgb)
  {
    contrasts << readContrast(QStringLiteral("redContrastEnhancement"));
    contrasts << readContrast(QStringLiteral("greenContrastEnhancement"));
    contrasts << readContrast(QStringLiteral("blueContrastEnhancement"));
  }
  else
  {
    contrasts << readContrast(QStringLiteral("redContrastEnhancement"));
    if (!contrasts.first().present)
      contrasts << readContrast(QStringLiteral("grayContrastEnhancement"));
  }

  bool anyPresent = false;
  bool allStretch = true;
  bool allClip = true;
  double minimum = std::numeric_limits<double>::max();
  double maximum = std::numeric_limits<double>::lowest();

  for (const Contrast &contrast : contrasts)
  {
    anyPresent = anyPresent || contrast.present;
    if (!contrast.present || contrast.algorithm != QStringLiteral("StretchToMinimumMaximum"))
      allStretch = false;
    if (!contrast.present || contrast.algorithm != QStringLiteral("ClipToMinimumMaximum"))
      allClip = false;
    minimum = std::min(minimum, contrast.minimum);
    maximum = std::max(maximum, contrast.maximum);
  }

  if (allClip)
  {
    result.stretch = RasterStyle::Stretch::PercentClip;
    const XmlNode *origin = rasterRenderer.child(QStringLiteral("minMaxOrigin"));
    if (origin)
    {
      double low = 2.0;
      double high = 98.0;
      readDouble(origin->child(QStringLiteral("cumulativeCutLower")) ? origin->child(QStringLiteral("cumulativeCutLower"))->text() : QString(), &low);
      readDouble(origin->child(QStringLiteral("cumulativeCutUpper")) ? origin->child(QStringLiteral("cumulativeCutUpper"))->text() : QString(), &high);
      result.percentLow = low * 100.0;
      result.percentHigh = high * 100.0;
    }
  }
  else if (allStretch && anyPresent)
  {
    result.stretch = RasterStyle::Stretch::MinMax;
    if (minimum <= maximum)
    {
      result.useCustomRange = true;
      result.minValue = minimum;
      result.maxValue = maximum;
    }
  }
  else
  {
    result.stretch = RasterStyle::Stretch::None;
  }

  *style = result;
  return true;
}

XmlNode buildVectorRenderer(const Style &style, GeometryKind geometryKind)
{
  const QString symbolType = symbolTypeForKind(geometryKind);
  XmlNode renderer(QStringLiteral("renderer-v2"));
  renderer.setAttribute(QStringLiteral("type"), QStringLiteral("singleSymbol"));
  renderer.setAttribute(QStringLiteral("forceraster"), QStringLiteral("0"));
  renderer.setAttribute(QStringLiteral("symbollevels"), QStringLiteral("0"));
  renderer.setAttribute(QStringLiteral("enableorderby"), QStringLiteral("0"));
  renderer.setAttribute(QStringLiteral("referencescale"), QStringLiteral("-1"));

  XmlNode symbols(QStringLiteral("symbols"));
  XmlNode symbol(QStringLiteral("symbol"));
  symbol.setAttribute(QStringLiteral("type"), symbolType);
  symbol.setAttribute(QStringLiteral("name"), QStringLiteral("0"));
  symbol.setAttribute(QStringLiteral("alpha"), number(std::clamp(style.opacity, 0, 100) / 100.0));
  symbol.setAttribute(QStringLiteral("clip_to_extent"), QStringLiteral("1"));
  symbol.setAttribute(QStringLiteral("force_rhr"), QStringLiteral("0"));
  symbol.setAttribute(QStringLiteral("is_animated"), QStringLiteral("0"));
  symbol.setAttribute(QStringLiteral("frame_rate"), QStringLiteral("10"));
  symbol.appendChild(makeDataDefinedProperties());

  XmlNode layer(QStringLiteral("layer"));
  layer.setAttribute(QStringLiteral("class"), classForKind(geometryKind));
  layer.setAttribute(QStringLiteral("enabled"), QStringLiteral("1"));
  layer.setAttribute(QStringLiteral("locked"), QStringLiteral("0"));
  layer.setAttribute(QStringLiteral("pass"), QStringLiteral("0"));
  layer.setAttribute(QStringLiteral("id"), QString());

  QVector<QPair<QString, QString>> options;
  const QString outlineWidth = number(pixelsToMillimeters(std::max(0.0, style.strokeWidth)));

  if (geometryKind == GeometryKind::Polygon)
  {
    options << qMakePair(QStringLiteral("color"), colorText(style.fillColor));
    options << qMakePair(QStringLiteral("joinstyle"), QStringLiteral("bevel"));
    options << qMakePair(QStringLiteral("offset"), QStringLiteral("0,0"));
    options << qMakePair(QStringLiteral("offset_unit"), QStringLiteral("MM"));
    options << qMakePair(QStringLiteral("outline_color"), colorText(style.strokeColor));
    options << qMakePair(QStringLiteral("outline_style"),
                         style.strokeWidth > 0.0 ? QStringLiteral("solid") : QStringLiteral("no"));
    options << qMakePair(QStringLiteral("outline_width"), outlineWidth);
    options << qMakePair(QStringLiteral("outline_width_unit"), QStringLiteral("MM"));
    options << qMakePair(QStringLiteral("style"), style.fillEnabled ? QStringLiteral("solid") : QStringLiteral("no"));
  }
  else if (geometryKind == GeometryKind::Line)
  {
    options << qMakePair(QStringLiteral("line_color"), colorText(style.strokeColor));
    options << qMakePair(QStringLiteral("line_style"), QStringLiteral("solid"));
    options << qMakePair(QStringLiteral("line_width"), outlineWidth);
    options << qMakePair(QStringLiteral("line_width_unit"), QStringLiteral("MM"));
    options << qMakePair(QStringLiteral("capstyle"), QStringLiteral("square"));
    options << qMakePair(QStringLiteral("joinstyle"), QStringLiteral("bevel"));
    options << qMakePair(QStringLiteral("offset"), QString());
  }
  else
  {
    options << qMakePair(QStringLiteral("name"), QStringLiteral("circle"));
    options << qMakePair(QStringLiteral("color"), colorText(style.fillColor));
    options << qMakePair(QStringLiteral("outline_color"), colorText(style.strokeColor));
    options << qMakePair(QStringLiteral("outline_style"),
                         style.strokeWidth > 0.0 ? QStringLiteral("solid") : QStringLiteral("no"));
    options << qMakePair(QStringLiteral("outline_width"), outlineWidth);
    options << qMakePair(QStringLiteral("outline_width_unit"), QStringLiteral("MM"));
    options << qMakePair(QStringLiteral("size"), number(pixelsToMillimeters(std::max(1.0, style.pointSize))));
    options << qMakePair(QStringLiteral("size_unit"), QStringLiteral("MM"));
    options << qMakePair(QStringLiteral("angle"), QStringLiteral("0"));
    options << qMakePair(QStringLiteral("offset"), QStringLiteral("0,0"));
    options << qMakePair(QStringLiteral("offset_unit"), QStringLiteral("MM"));
    options << qMakePair(QStringLiteral("scale_method"), QStringLiteral("diameter"));
    options << qMakePair(QStringLiteral("horizontal_anchor_point"), QStringLiteral("1"));
    options << qMakePair(QStringLiteral("vertical_anchor_point"), QStringLiteral("1"));
  }

  layer.appendChild(makeOptionsMap(options));
  layer.appendChild(makeDataDefinedProperties());
  symbol.appendChild(layer);
  symbols.appendChild(symbol);
  renderer.appendChild(symbols);
  renderer.appendChild(XmlNode(QStringLiteral("rotation")));
  renderer.appendChild(XmlNode(QStringLiteral("sizescale")));
  return renderer;
}

XmlNode buildLabeling(const LabelStyle &style, bool geographicCrs)
{
  XmlNode labeling(QStringLiteral("labeling"));
  labeling.setAttribute(QStringLiteral("type"), QStringLiteral("simple"));

  XmlNode settings(QStringLiteral("settings"));
  settings.setAttribute(QStringLiteral("calloutType"), QStringLiteral("simple"));

  XmlNode textStyle(QStringLiteral("text-style"));
  const bool expression = style.fields.size() > 1;
  const QString fieldName = expression
                              ? labelExpression(style.fields, style.separator)
                              : (style.fields.isEmpty() ? QString() : style.fields.first());
  textStyle.setAttribute(QStringLiteral("fieldName"), fieldName);
  textStyle.setAttribute(QStringLiteral("isExpression"), expression ? QStringLiteral("1") : QStringLiteral("0"));
  if (!style.fontFamily.isEmpty())
    textStyle.setAttribute(QStringLiteral("fontFamily"), style.fontFamily);
  textStyle.setAttribute(QStringLiteral("fontSize"), number(style.fontSize));
  textStyle.setAttribute(QStringLiteral("fontSizeUnit"),
                          style.sizeMode == LabelStyle::SizeMode::Screen ? QStringLiteral("Point")
                                                                         : QStringLiteral("MapUnit"));
  textStyle.setAttribute(QStringLiteral("fontWeight"), style.bold ? QStringLiteral("75") : QStringLiteral("50"));
  textStyle.setAttribute(QStringLiteral("fontItalic"), style.italic ? QStringLiteral("1") : QStringLiteral("0"));
  textStyle.setAttribute(QStringLiteral("textColor"), colorText(style.color));
  textStyle.setAttribute(QStringLiteral("textOpacity"), QStringLiteral("1"));
  textStyle.setAttribute(QStringLiteral("fontUnderline"), QStringLiteral("0"));
  textStyle.setAttribute(QStringLiteral("fontStrikeout"), QStringLiteral("0"));
  textStyle.setAttribute(QStringLiteral("namedStyle"), QString());
  textStyle.setAttribute(QStringLiteral("blendMode"), QStringLiteral("0"));
  textStyle.setAttribute(QStringLiteral("multilineHeight"), QStringLiteral("1"));

  XmlNode buffer(QStringLiteral("text-buffer"));
  buffer.setAttribute(QStringLiteral("bufferDraw"), style.haloEnabled ? QStringLiteral("1") : QStringLiteral("0"));
  buffer.setAttribute(QStringLiteral("bufferSize"), number(pixelsToMillimeters(style.haloWidth)));
  buffer.setAttribute(QStringLiteral("bufferSizeUnits"), QStringLiteral("MM"));
  buffer.setAttribute(QStringLiteral("bufferColor"), colorText(style.haloColor));
  buffer.setAttribute(QStringLiteral("bufferOpacity"), QStringLiteral("1"));
  buffer.setAttribute(QStringLiteral("bufferNoFill"), QStringLiteral("1"));
  buffer.setAttribute(QStringLiteral("bufferJoinStyle"), QStringLiteral("128"));
  buffer.setAttribute(QStringLiteral("bufferBlendMode"), QStringLiteral("0"));
  textStyle.appendChild(buffer);
  textStyle.appendChild(XmlNode(QStringLiteral("families")));
  settings.appendChild(textStyle);

  settings.appendChild(XmlNode(QStringLiteral("text-format")));

  XmlNode placement(QStringLiteral("placement"));
  int placementValue = 0;
  if (style.placement == LabelStyle::Placement::OverPoint)
    placementValue = 1;
  else if (style.placement == LabelStyle::Placement::Line)
    placementValue = 2;
  placement.setAttribute(QStringLiteral("placement"), QString::number(placementValue));
  placement.setAttribute(QStringLiteral("xOffset"), number(pixelsToMillimeters(style.offsetX)));
  placement.setAttribute(QStringLiteral("yOffset"), number(pixelsToMillimeters(-style.offsetY)));
  placement.setAttribute(QStringLiteral("offsetUnits"), QStringLiteral("MM"));
  placement.setAttribute(QStringLiteral("quadOffset"), QStringLiteral("4"));
  placement.setAttribute(QStringLiteral("priority"), QString::number(std::clamp(10 - style.priority, 0, 10)));
  placement.setAttribute(QStringLiteral("dist"), QStringLiteral("0"));
  placement.setAttribute(QStringLiteral("distUnits"), QStringLiteral("MM"));
  settings.appendChild(placement);

  XmlNode rendering(QStringLiteral("rendering"));
  const double scaleMin = mapUnitsPerPixelToScale(style.minimumScale, geographicCrs);
  const double scaleMax = mapUnitsPerPixelToScale(style.maximumScale, geographicCrs);
  const bool scaleVisibility = scaleMin > 0.0 || scaleMax > 0.0;
  rendering.setAttribute(QStringLiteral("drawLabels"), QStringLiteral("1"));
  rendering.setAttribute(QStringLiteral("scaleVisibility"), scaleVisibility ? QStringLiteral("1") : QStringLiteral("0"));
  rendering.setAttribute(QStringLiteral("scaleMin"), number(scaleMin));
  rendering.setAttribute(QStringLiteral("scaleMax"), number(scaleMax));
  rendering.setAttribute(QStringLiteral("maxNumLabels"), QString::number(style.maximumLabels));
  rendering.setAttribute(QStringLiteral("minFeatureSize"), number(pixelsToMillimeters(style.minimumFeatureSize)));
  rendering.setAttribute(QStringLiteral("obstacle"), QStringLiteral("1"));
  settings.appendChild(rendering);

  labeling.appendChild(settings);
  return labeling;
}

XmlNode buildRasterRenderer(const RasterStyle &style)
{
  XmlNode renderer(QStringLiteral("rasterrenderer"));
  const bool gray = style.bandMode == RasterStyle::BandMode::Grayscale;
  renderer.setAttribute(QStringLiteral("type"), gray ? QStringLiteral("singlebandgray") : QStringLiteral("multibandcolor"));
  renderer.setAttribute(QStringLiteral("opacity"), number(std::clamp(style.opacity, 0, 100) / 100.0));
  renderer.setAttribute(QStringLiteral("alphaBand"), QString::number(style.alphaBand > 0 ? style.alphaBand : -1));
  renderer.setAttribute(QStringLiteral("nodataColor"), QString());

  if (gray)
  {
    renderer.setAttribute(QStringLiteral("grayBand"), QString::number(style.redBand));
  }
  else
  {
    renderer.setAttribute(QStringLiteral("redBand"), QString::number(style.redBand));
    renderer.setAttribute(QStringLiteral("greenBand"), QString::number(style.greenBand));
    renderer.setAttribute(QStringLiteral("blueBand"), QString::number(style.blueBand));
  }

  XmlNode origin(QStringLiteral("minMaxOrigin"));
  origin.appendChild(XmlNode(QStringLiteral("limits")));
  origin.appendChild(XmlNode(QStringLiteral("extent")));
  origin.appendChild(XmlNode(QStringLiteral("statAccuracy")));
  origin.appendChild(XmlNode(QStringLiteral("cumulativeCutLower")));
  origin.appendChild(XmlNode(QStringLiteral("cumulativeCutUpper")));
  origin.appendChild(XmlNode(QStringLiteral("stdDevFactor")));
  origin.child(QStringLiteral("limits"))->setText(QStringLiteral("None"));
  origin.child(QStringLiteral("extent"))->setText(QStringLiteral("WholeRaster"));
  origin.child(QStringLiteral("statAccuracy"))->setText(QStringLiteral("Estimated"));
  origin.child(QStringLiteral("cumulativeCutLower"))->setText(QStringLiteral("0.02"));
  origin.child(QStringLiteral("cumulativeCutUpper"))->setText(QStringLiteral("0.98"));
  origin.child(QStringLiteral("stdDevFactor"))->setText(QStringLiteral("2"));
  renderer.appendChild(origin);

  if (style.stretch != RasterStyle::Stretch::None)
  {
    const QString algorithm = style.stretch == RasterStyle::Stretch::PercentClip
                                ? QStringLiteral("ClipToMinimumMaximum")
                                : QStringLiteral("StretchToMinimumMaximum");
    const double minimum = style.useCustomRange ? style.minValue : 0.0;
    const double maximum = style.useCustomRange ? style.maxValue : 255.0;

    const auto contrast = [&](const QString &name) {
      XmlNode node(name);
      XmlNode minNode(QStringLiteral("minValue"));
      minNode.setText(number(minimum));
      XmlNode maxNode(QStringLiteral("maxValue"));
      maxNode.setText(number(maximum));
      XmlNode algorithmNode(QStringLiteral("algorithm"));
      algorithmNode.setText(algorithm);
      node.appendChild(minNode);
      node.appendChild(maxNode);
      node.appendChild(algorithmNode);
      return node;
    };

    if (gray)
    {
      renderer.appendChild(contrast(QStringLiteral("redContrastEnhancement")));
    }
    else
    {
      renderer.appendChild(contrast(QStringLiteral("redContrastEnhancement")));
      renderer.appendChild(contrast(QStringLiteral("greenContrastEnhancement")));
      renderer.appendChild(contrast(QStringLiteral("blueContrastEnhancement")));
    }
  }

  return renderer;
}

} // namespace qgis
} // namespace minigis