#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include "minigiserror.h"
#include "minigisrectangle.h"
#include "minigisstyle.h"

namespace minigis
{

/**
 * Lo que un archivo KML/KMZ entrega por OGR frente a lo que el almacen de
 * geometrias conserva.
 *
 * Por que existe: LIBKML declara la capa con tipo de geometria desconocido, asi
 * que la clasificacion se hace por elemento. Un elemento cuya geometria no
 * encajaba en esa clasificacion se descartaba sin aviso y el mapa salia "bien"
 * con datos de menos. Este diagnostico compara las dos orillas para que la
 * perdida sea visible y comprobable sin mirar una imagen.
 */
struct KmlLayerReport
{
  QString name;
  qint64 features = 0;         // elementos que entrega OGR
  qint64 withoutGeometry = 0;  // placemarks que solo traen atributos
  qint64 unsupportedParts = 0; // partes de geometria que no sabemos dibujar
  qint64 points = 0;
  qint64 lines = 0;
  qint64 polygons = 0;
  qint64 storeFeatures = -1;   // -1 = no se pudo construir el almacen
  /** True si la segunda lectura de una misma fila no vuelve al origen. */
  bool attributeCache = true;
  int distinctStyles = 0;      // estilos distintos declarados por los elementos
  int appliedStyles = 0;       // estilos que el almacen guardo (1 si se replego)
  QString styleString;         // style string dominante, vacio si no hay estilos
  int crsEpsg = 0;
  Rectangle extent;

  /** Nombre legible de lo que hay: Punto, Linea, Poligono, Mixta o Sin geometria. */
  QString geometryName() const;

  /** Elementos que traen geometria y que el almacen deberia conservar. */
  qint64 drawableFeatures() const { return features - withoutGeometry; }

  /**
   * True si el almacen conservo menos elementos de los que traen geometria. Es
   * el sintoma exacto de la perdida silenciosa; con el almacen no construido
   * (presupuesto superado) no se puede afirmar nada y devuelve false.
   */
  bool missingInStore() const;
};

struct KmlReport
{
  QString path;
  QString driver;
  QVector<KmlLayerReport> layers;
  int declaredStyles = 0;
  int appliedStyles = 0;
  QString styleMode = QStringLiteral("none");
  /** Simbologia que se aplica; solo tiene sentido si `appliedStyles` > 0. */
  Style style;

  bool ok() const;
  QJsonObject toJson() const;
  QString toText() const;
};

/**
 * Inspecciona un KML/KMZ: abre cada capa, cuenta la geometria por elemento con
 * la API C de OGR y la compara con lo que conserva el almacen.
 *
 * Devuelve false solo si GDAL no puede abrir el archivo (y rellena `error`). Un
 * archivo que abre pero no tiene capas devuelve true con `layers` vacio, para
 * que la CLI pueda distinguir el codigo de salida de "sin capas".
 */
bool inspectKml(const QString &path, KmlReport *report, Error *error);

} // namespace minigis
