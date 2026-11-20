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

/**
 * Traduce el style string de OGR (lo que devuelve OGR_F_GetStyleString) a la
 * simbologia de MiniGIS.
 *
 * Formato de referencia: `PEN(c:#RRGGBB,w:Npx)` y `BRUSH(fc:#RRGGBB,fo:N)`.
 * Devuelve `fallback` cuando el texto no trae nada aprovechable, para que una
 * capa sin estilos se quede con la simbologia por defecto.
 *
 * La anchura se acota a 20 px: un `width` desmedido de un KML taparia el mapa.
 */
Style styleFromOgrStyleString(const QString &styleString, const Style &fallback);

} // namespace minigis
