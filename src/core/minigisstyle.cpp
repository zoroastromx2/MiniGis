#include "minigisstyle.h"

#include <QStringList>

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

} // namespace minigis
