#pragma once

#include <QColor>
#include <QString>

#include "minigislabelstyle.h"
#include "minigisrasterstyle.h"
#include "minigisstyle.h"
#include "minigisxmltree.h"

namespace minigis
{

/**
 * Lectura y escritura de la simbologia de QGIS hacia los estilos de MiniGIS.
 *
 * QGIS guarda mucha mas informacion de la que MiniGIS dibuja. La lectura solo
 * acepta el subconjunto que se puede representar sin perdida razonable
 * (`singleSymbol` con una capa simple, etiquetas de un unico estilo y
 * renderizadores de bandas). Cuando el documento no encaja devuelve false y
 * quien llama conserva el XML original en vez de reescribirlo.
 */
namespace qgis
{

/** Colores "r,g,b,a" o "r,g,b,a,rgb:..." de QGIS. */
QColor parseColor(const QString &text, bool *ok = nullptr);

/** Color de MiniGIS a la forma que escribe QGIS. */
QString colorText(const QColor &color);

/** Milimetros a pixeles a 96 ppp (la densidad con la que dibuja QGIS). */
double millimetersToPixels(double millimeters);
double pixelsToMillimeters(double pixels);

/** Puntos tipograficos a pixeles (96 ppp). */
double pointsToPixels(double points);

/**
 * Denominador de escala de QGIS a unidades de mapa por pixel de MiniGIS.
 * `geographicCrs` usa la convencion de QGIS (grados -> metros) para escalas.
 */
double scaleToMapUnitsPerPixel(double scale, bool geographicCrs);

/** Inversa de `scaleToMapUnitsPerPixel`. */
double mapUnitsPerPixelToScale(double mapUnitsPerPixel, bool geographicCrs);

/**
 * Lee un `<renderer-v2 type="singleSymbol">` de una capa vectorial.
 * Devuelve false si el renderizador no es representable.
 */
bool readVectorStyle(const XmlNode &renderer, Style *style);

/**
 * Lee las etiquetas de un `<maplayer>`. Devuelve false si el formato no se
 * puede representar (varios estilos, expresion desconocida...).
 */
bool readLabelStyle(const XmlNode &maplayer, bool geographicCrs, LabelStyle *style);

/**
 * Lee un `<rasterrenderer>` multibanda o de banda unica gris.
 * Devuelve false para paletas, pseudocolor, hillshade y similares.
 */
bool readRasterStyle(const XmlNode &rasterRenderer, RasterStyle *style);

/** Construye un `<renderer-v2 type="singleSymbol">` para una capa vectorial. */
XmlNode buildVectorRenderer(const Style &style, GeometryKind geometryKind);

/** Construye un `<labeling type="simple">` con el subconjunto de MiniGIS. */
XmlNode buildLabeling(const LabelStyle &style, bool geographicCrs);

/** Construye un `<rasterrenderer>` multibanda o de banda unica gris. */
XmlNode buildRasterRenderer(const RasterStyle &style);

} // namespace qgis
} // namespace minigis
