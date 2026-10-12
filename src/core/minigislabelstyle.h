#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

namespace minigis
{

/**
 * Configuracion de las etiquetas de una capa.
 *
 * Las etiquetas son la parte del dibujado que mas se nota cuando esta mal: una
 * etiqueta sobre otra, texto ilegible sobre el relleno, o miles de etiquetas
 * diminutas que convierten el mapa en ruido. Por eso la configuracion incluye,
 * ademas de los campos y la fuente, las tres cosas que hacen que un etiquetado sea
 * util:
 *
 *   * **Escala**: a que escalas se muestran (rango) y como se mide la fuente (en
 *     puntos de pantalla, que es lo habitual, o en unidades del mapa, de modo que
 *     el texto crece al acercarse).
 *   * **Colocacion**: donde se pone el texto respecto al elemento y con que
 *     separacion, mas un tamano minimo del elemento para no etiquetar lo que no
 *     se ve.
 *   * **Legibilidad y saturacion**: halo (borde claro) para que el texto se lea
 *     sobre cualquier relleno, distancia minima entre etiquetas para que no se
 *     solapen, descarte de textos repetidos y un limite de etiquetas por cuadro
 *     con prioridad para decidir cuales entran.
 *
 * La lista `fields` guarda NOMBRES de campo (no indices): asi la configuracion
 * sobrevive a cambios en el orden de los campos de la capa.
 */
class LabelStyle
{
public:
  /** Como se interpreta el tamano de la fuente. */
  enum class SizeMode
  {
    Screen,   // puntos de pantalla: el texto no cambia al acercarse (lo habitual)
    MapUnits  // unidades del mapa: el texto crece al acercarse
  };

  /** Donde se coloca el texto respecto al elemento. */
  enum class Placement
  {
    AroundPoint, // encima del elemento (con desplazamiento)
    OverPoint,   // centrado en el elemento
    Line         // sobre el punto medio del elemento (lineas)
  };

  bool enabled = false;

  /** Campos que se muestran, en orden. */
  QStringList fields;
  /** Texto que se inserta entre los campos. */
  QString separator = QStringLiteral(" · ");

  SizeMode sizeMode = SizeMode::Screen;
  /** Familia tipografica; vacio = la fuente de la aplicacion. */
  QString fontFamily;
  /** Tamano: en puntos si `sizeMode` es Screen, en unidades de mapa si no. */
  double fontSize = 10.0;
  bool bold = false;
  bool italic = false;
  QColor color {25, 25, 25};

  /** Halo: borde alrededor del texto para que se lea sobre cualquier relleno. */
  bool haloEnabled = true;
  QColor haloColor {255, 255, 255, 220};
  double haloWidth = 1.6;

  /**
   * Rango de escala en el que se muestran las etiquetas, en unidades de mapa por
   * pixel (la misma magnitud que muestra la barra de estado). 0 = sin limite.
   * `minimumScale` es el limite "alejado" (mpp grande) y `maximumScale` el
   * "cercano" (mpp pequeno), como en cualquier SIG.
   */
  double minimumScale = 0.0;
  double maximumScale = 0.0;

  Placement placement = Placement::AroundPoint;
  /** Desplazamiento del texto en pixeles respecto al ancla. */
  double offsetX = 0.0;
  double offsetY = -8.0;

  /** Elementos que se etiquetan como maximo por capa y cuadro (0 = sin limite). */
  qint64 maximumLabels = 3000;
  /** Distancia minima entre etiquetas, en pixeles (0 = no se comprueba). */
  double minimumDistance = 10.0;
  /** Tamano minimo del elemento (en pixeles) para etiquetarlo (0 = sin minimo). */
  double minimumFeatureSize = 6.0;
  /** No repetir el mismo texto dentro de la misma vista. */
  bool discardDuplicates = true;
  /** Menor = se coloca antes; sirve para decidir que etiquetas entran. */
  int priority = 0;

  /** True si a esa escala (unidades de mapa por pixel) deben verse. */
  bool isVisibleAtScale(double mapUnitsPerPixel) const
  {
    if (minimumScale > 0.0 && mapUnitsPerPixel > minimumScale)
      return false;
    if (maximumScale > 0.0 && mapUnitsPerPixel < maximumScale)
      return false;
    return true;
  }

  /** Resumen corto para la barra de estado y los listados. */
  QString summary() const
  {
    if (!enabled)
      return QStringLiteral("sin etiquetas");
    if (fields.isEmpty())
      return QStringLiteral("etiquetas sin campos");

    return QStringLiteral("etiquetas: %1 (%2 pt)")
      .arg(fields.join(separator), QString::number(fontSize, 'g', 3));
  }
};

} // namespace minigis
