#pragma once

#include <QColor>
#include <QString>

namespace minigis
{

/** Tipo de geometria de una capa, ya simplificado. */
enum class GeometryKind
{
  Unknown,
  Point,
  Line,
  Polygon
};

/**
 * Simbologia sencilla de una capa (equivalente minimo a QgsSymbol + QgsRenderer).
 * El objetivo es poder distinguir capas y tipos de geometria de un vistazo.
 */
class Style
{
public:
  QColor fillColor {90, 140, 200, 255};
  QColor strokeColor {35, 65, 105, 255};
  double strokeWidth = 1.0;
  double pointSize = 7.0;
  int opacity = 100; // 0..100
  bool fillEnabled = true;

  /** Estilo por defecto para un tipo de geometria; `index` varia la paleta. */
  static Style defaultForKind(GeometryKind kind, int index);

  static QString kindName(GeometryKind kind);
};

} // namespace minigis
