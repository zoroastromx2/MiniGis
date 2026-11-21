#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include "minigiserror.h"
#include "minigisrectangle.h"
#include "minigisvectorlayer.h"

namespace minigis
{

/**
 * Lo que un GeoParquet entrega por OGR frente a lo que conserva el almacen de
 * geometrias.
 *
 * Por que existe: el motor de dibujo puede perder un elemento sin avisar (una
 * geometria que no encaja, un tipo mixto, una parte no soportada) y el mapa sale
 * igual de «bien» con un elemento de menos. Comparar las dos orillas es la unica
 * forma de detectarlo sin mirar pixeles; es el mismo contrato que --check-kml.
 */
struct GeoParquetLayerReport
{
  QString name;
  qint64 features = 0;         // elementos que entrega OGR
  qint64 withoutGeometry = 0;  // filas sin geometria
  qint64 unsupportedParts = 0; // partes que el almacen no sabe dibujar
  qint64 points = 0;
  qint64 lines = 0;
  qint64 polygons = 0;
  qint64 storeFeatures = -1;   // -1 = no se pudo construir el almacen
  /** True si la segunda lectura de una misma fila no vuelve al origen. */
  bool attributeCache = true;
  /** True si se comprobo que una pagina de la tabla coincide con las filas sueltas. */
  bool attributePageChecked = false;
  bool attributePage = true;
  int crsEpsg = 0;
  QString crsName;
  QString crsDescription;
  QString fidColumn;
  Rectangle extent;
  Rectangle extent4326;
  QVector<VectorLayer::FieldInfo> fields;

  /** Nombre legible de lo que hay: Punto, Linea, Poligono, Mixta o Sin geometria. */
  QString geometryName() const;

  /** Elementos que traen geometria y que el almacen deberia conservar. */
  qint64 drawableFeatures() const { return features - withoutGeometry; }

  /**
   * True si el almacen conservo menos elementos de los que traen geometria. Con
   * el almacen no construido (presupuesto superado) no se puede afirmar nada y
   * devuelve false.
   */
  bool missingInStore() const;
};

struct GeoParquetReport
{
  QString path;
  QString driver;
  QVector<GeoParquetLayerReport> layers;

  bool ok() const;
  QJsonObject toJson() const;
  QString toText() const;
};

/**
 * Inspecciona un GeoParquet: abre cada capa, cuenta la geometria por elemento
 * con la API C de OGR, construye el almacen y compara las dos orillas.
 *
 * Devuelve false solo si GDAL no puede abrir el archivo o si no es dibujable
 * (tabla sin geometria); en ese caso rellena `error` y, si procede, `issue`.
 * Un archivo que abre pero no tiene capas devuelve true con `layers` vacio.
 */
bool inspectGeoParquet(const QString &path, GeoParquetReport *report, Error *error,
                       VectorOpenIssue *issue = nullptr);

} // namespace minigis
