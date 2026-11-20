#include "minigisstyle.h"

#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace minigis
{
namespace
{

struct Palette
{
  QColor fill;
  QColor stroke;
};

// Paleta de seis colores, en la linea de los colores aleatorios de QGIS pero
// con contraste suficiente sobre fondo blanco.
const Palette kPalette[] = {
  {QColor(102, 153, 204), QColor(40, 80, 130)},
  {QColor(233, 150, 122), QColor(150, 70, 45)},
  {QColor(144, 190, 109), QColor(70, 110, 45)},
  {QColor(218, 165, 32), QColor(140, 100, 10)},
  {QColor(180, 150, 200), QColor(100, 70, 125)},
  {QColor(120, 190, 190), QColor(45, 110, 110)},
};

constexpr int kPaletteSize = int(sizeof(kPalette) / sizeof(kPalette[0]));

} // namespace

Style Style::defaultForKind(GeometryKind kind, int index)
{
  Style style;
  const Palette &palette = kPalette[((index % kPaletteSize) + kPaletteSize) % kPaletteSize];

  switch (kind)
  {
    case GeometryKind::Point:
      // Los puntos se dibujan rellenos y con un contorno mas oscuro.
      style.fillColor = palette.fill;
      style.strokeColor = palette.stroke;
      style.pointSize = 8.0;
      style.strokeWidth = 1.0;
      style.fillEnabled = true;
      break;

    case GeometryKind::Line:
      // Las lineas no se rellenan: solo trazo, algo mas grueso para verse.
      style.fillColor = QColor(Qt::transparent);
      style.strokeColor = palette.stroke;
      style.strokeWidth = 1.6;
      style.fillEnabled = false;
      break;

    case GeometryKind::Polygon:
      // Poligonos translucidos para que se vea lo que hay debajo.
      style.fillColor = QColor(palette.fill.red(), palette.fill.green(), palette.fill.blue(), 140);
      style.strokeColor = palette.stroke;
      style.strokeWidth = 1.2;
      style.fillEnabled = true;
      break;

    case GeometryKind::Unknown:
    default:
      style.fillColor = QColor(150, 150, 150, 140);
      style.strokeColor = QColor(60, 60, 60);
      break;
  }

  style.opacity = 100;
  return style;
}

QString Style::kindName(GeometryKind kind)
{
  switch (kind)
  {
    case GeometryKind::Point:
      return QStringLiteral("Puntos");
    case GeometryKind::Line:
      return QStringLiteral("Lineas");
    case GeometryKind::Polygon:
      return QStringLiteral("Poligonos");
    case GeometryKind::Unknown:
    default:
      return QStringLiteral("Desconocido");
  }
}

namespace
{

/** Valor de un bloque `NAME(...)` del style string, sin los parentesis. */
QString blockOf(const QString &text, const QString &name)
{
  const int start = text.indexOf(name + QLatin1Char('('), 0, Qt::CaseInsensitive);
  if (start < 0)
    return {};
  const int open = start + name.size();
  const int close = text.indexOf(QLatin1Char(')'), open);
  if (close < 0)
    return {};
  return text.mid(open + 1, close - open - 1);
}

QHash<QString, QString> parametersOf(const QString &block)
{
  QHash<QString, QString> parameters;
  const QStringList parts = block.split(QLatin1Char(','), Qt::SkipEmptyParts);
  for (const QString &part : parts)
  {
    const int colon = part.indexOf(QLatin1Char(':'));
    if (colon <= 0)
      continue;
    parameters.insert(part.left(colon).trimmed().toLower(), part.mid(colon + 1).trimmed());
  }
  return parameters;
}

QColor colorFromText(const QString &value, const QColor &fallback)
{
  QString hex = value.trimmed();
  if (hex.startsWith(QLatin1Char('#')))
    hex.remove(0, 1);
  if (hex.size() != 6 && hex.size() != 8)
    return fallback;

  // LIBKML entrega #RRGGBBAA: el alfa va AL FINAL. Comprobado con dos estilos
  // del ejemplo (rojo ff0000ff -> #FF0000FF y azul ffff0000 -> #0000FFFF).
  // Leer los seis ultimos digitos como RGB intercambiaria rojo y azul en
  // silencio, que es el peor tipo de error de simbologia.
  const QColor color(QLatin1Char('#') + hex.left(6));
  if (!color.isValid())
    return fallback;

  QColor result = color;
  if (hex.size() == 8)
  {
    bool ok = false;
    const int alpha = hex.right(2).toInt(&ok, 16);
    if (ok)
      result.setAlpha(alpha);
  }
  return result;
}

/** Anchura en pixeles, tolerando el sufijo `px` y acotada al maximo legible. */
double widthFromText(const QString &value, double fallback)
{
  QString text = value.trimmed().toLower();
  if (text.endsWith(QStringLiteral("px")))
    text.chop(2);

  bool ok = false;
  const double width = text.toDouble(&ok);
  if (!ok || width <= 0.0)
    return fallback;
  return std::min(width, 20.0);
}

} // namespace

Style styleFromOgrStyleString(const QString &styleString, const Style &fallback)
{
  if (styleString.trimmed().isEmpty())
    return fallback;

  Style style = fallback;

  const QString pen = blockOf(styleString, QStringLiteral("PEN"));
  if (!pen.isEmpty())
  {
    const QHash<QString, QString> parameters = parametersOf(pen);
    if (parameters.contains(QStringLiteral("c")))
      style.strokeColor = colorFromText(parameters.value(QStringLiteral("c")), style.strokeColor);
    if (parameters.contains(QStringLiteral("w")))
      style.strokeWidth = widthFromText(parameters.value(QStringLiteral("w")), style.strokeWidth);
  }

  const QString brush = blockOf(styleString, QStringLiteral("BRUSH"));
  if (!brush.isEmpty())
  {
    const QHash<QString, QString> parameters = parametersOf(brush);
    const QString fill = parameters.value(QStringLiteral("fc"));
    if (fill.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
    {
      style.fillEnabled = false;
    }
    else if (parameters.contains(QStringLiteral("fc")))
    {
      style.fillColor = colorFromText(fill, style.fillColor);
      style.fillEnabled = true;
    }

    if (parameters.contains(QStringLiteral("fo")))
    {
      QString percent = parameters.value(QStringLiteral("fo"));
      percent.remove(QLatin1Char('%'));
      bool ok = false;
      const double value = percent.toDouble(&ok);
      if (ok)
        style.opacity = std::clamp(static_cast<int>(std::lround(value)), 0, 100);
    }
  }

  return style;
}

} // namespace minigis
