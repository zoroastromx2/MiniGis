#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

#include <memory>
#include <vector>

namespace minigis
{

class VectorLayer;

/** Un elemento devuelto por Identificar, con sus atributos ya leidos. */
struct IdentifyHit
{
  std::shared_ptr<VectorLayer> layer;
  qint64 fid = -1;
  /** True si el clic cae dentro del elemento (no solo cerca). */
  bool inside = false;
  /** Distancia al elemento en unidades del SRC de destino; 0 si esta dentro. */
  double distance = 0.0;
  /** Un valor por campo, en el orden de `layer->fields()`. */
  QVector<QString> values;
};

/** Limites de una consulta de Identificar. */
struct IdentifyOptions
{
  /** Radio de busqueda en unidades del SRC de destino. */
  double tolerance = 0.0;
  /** Elementos devueltos por capa; 0 = sin limite. */
  int maximumPerLayer = 10;
  /** Elementos devueltos en total; 0 = sin limite. */
  int maximumTotal = 50;
  /** Candidatos probados por capa; 0 = sin limite. */
  int maximumCandidatesPerLayer = 256;
  /** Presupuesto de tiempo de la consulta; 0 = sin limite. */
  qint64 timeBudgetMs = 250;
};

/** Lo que ha costado la consulta; el panel lo usa para avisar de un recorte. */
struct IdentifyStats
{
  int layersSearched = 0;
  qint64 candidatesTested = 0;
  /** Geometrias leidas del archivo (modo envolventes o camino OGR). */
  qint64 featuresRead = 0;
  qint64 elapsedMs = 0;
  /** True si se agoto el presupuesto de tiempo. */
  bool budgetExceeded = false;
};

struct IdentifyRequest
{
  /** Punto a identificar, en el SRC de destino. */
  QPointF mapPoint;
  int destinationEpsg = 4326;
  /**
   * Capas a consultar en orden de apilado del lienzo: la primera es el fondo y
   * la ultima la que se dibuja encima. Solo deben pasarse las visibles.
   */
  std::vector<std::shared_ptr<VectorLayer>> layers;
  /** Capa prioritaria (la seleccionada en el arbol); puede ser nula. */
  std::shared_ptr<VectorLayer> priorityLayer;
  IdentifyOptions options;
};

struct IdentifyResult
{
  QPointF mapPoint;
  int destinationEpsg = 4326;
  double tolerance = 0.0;
  QVector<IdentifyHit> hits;
  IdentifyStats stats;
  /** True si se recorto la busqueda (topes, presupuesto o candidatos). */
  bool truncated = false;
};

/**
 * Busca los elementos que contienen el punto o estan a menos de su tolerancia.
 *
 * El orden visible es: primero la capa prioritaria y despues las capas de arriba
 * abajo (el ultimo elemento de `layers` es el que se dibuja encima). Dentro de
 * una capa van primero los elementos que contienen el punto, despues por
 * distancia y, a igual distancia, por FID.
 *
 * Los atributos se leen solo para los resultados que se van a mostrar, no para
 * todos los candidatos. La consulta esta acotada por `IdentifyOptions`: no
 * recorre la capa completa aunque el almacen no exista.
 */
IdentifyResult identifyAtPoint(const IdentifyRequest &request);

} // namespace minigis
