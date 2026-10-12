#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "minigisstyle.h"

class OGRLayer;

namespace minigis
{

class VectorLayer;

/**
 * Copia en memoria de las geometrias de una capa, con indice espacial
 * (R-tree STR empaquetado) y niveles de simplificacion (Douglas-Peucker)
 * calculados bajo demanda.
 *
 * Motivacion: el renderizado clasico recorre la capa completa con OGR en cada
 * cuadro (OGR_L_GetNextFeature + OGR_G_GetX por vertice, con clonado y
 * reproyeccion de cada elemento). Con dos millones de vertices eso son millones
 * de llamadas a la API C por cuadro, sin descartar nada de lo que queda fuera
 * de la vista. Este almacen:
 *
 *   * lee la capa UNA vez (y reproyecta UNA vez, si procede),
 *   * guarda las coordenadas en buffers planos contiguos, sin objetos OGR,
 *   * indexa las envolventes en un R-tree para descartar por extension visible,
 *   * simplifica por nivel de zoom conservando la forma a escala de pixel.
 *
 * `build()` es la unica operacion costosa y puede ejecutarse en un hilo de
 * trabajo. Despues, el objeto es inmutable salvo por la cache de niveles (que
 * solo crece) y las consultas son de solo lectura, por lo que se puede usar
 * desde el hilo de la interfaz sin bloqueos.
 */
class GeometryStore
{
public:
  /** Envolvente y anillos de un elemento, en coordenadas del SRC de destino. */
  struct Feature
  {
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    uint32_t firstRing = 0;
    uint32_t ringCount = 0;
  };

  /**
   * Conjunto de anillos simplificado con una tolerancia dada (en unidades del
   * SRC de destino). El nivel 0 son los datos sin simplificar.
   */
  struct Level
  {
    int index = 0;
    double tolerance = 0.0;
    /** Coordenadas x,y intercaladas de todos los anillos del nivel. */
    std::vector<double> coords;
    /** Indice del primer vertice de cada anillo dentro de `coords` (no doble). */
    std::vector<uint32_t> ringFirst;
    /** Numero de vertices de cada anillo; 0 = anillo descartado por simplificar. */
    std::vector<uint32_t> ringCount;
    qint64 vertices = 0;
  };

  struct Stats
  {
    qint64 features = 0;
    qint64 vertices = 0;   // vertices del nivel 0
    qint64 rings = 0;
    qint64 bytes = 0;      // memoria aproximada de todos los buffers
    int cachedLevels = 0;
    int destinationEpsg = 0; // 0 = coordenadas en el SRC de origen
    bool reprojected = false;
    /** Reparto del tiempo de construccion (diagnostico de rendimiento). */
    qint64 readMs = 0;
    qint64 indexMs = 0;
    qint64 levelsMs = 0;
  };

  struct Options
  {
    /** Vertices maximos que se aceptan en memoria (0 = sin limite). */
    qint64 vertexBudget = 8'000'000;
    /** Niveles de simplificacion que se precalculan al construir (ademas del 0). */
    int precomputedLevels = 6;
    /** Memoria maxima de los niveles precalculados, en bytes. */
    qint64 precomputeBytes = 192LL * 1024 * 1024;
  };

  /**
   * Lee la capa completa. `progress` recibe (porcentaje 0..100, mensaje) y puede
   * devolver false para cancelar. Si se cancela o se supera `vertexBudget`
   * devuelve nullptr y explica el motivo en `error` (no es un fallo fatal: el
   * renderizador puede seguir usando el camino clasico).
   */
  static std::shared_ptr<GeometryStore> build(
    OGRLayer *layer, int destinationEpsg, const Options &options,
    const std::function<bool(int, const QString &)> &progress = {},
    QString *error = nullptr);

  /**
   * Igual que `build()`, tomando la capa OGR de un VectorLayer abierto. Es la
   * forma que usan el cargador en segundo plano y el banco de pruebas: evita que
   * el resto del programa tenga que conocer los handles de GDAL.
   */
  static std::shared_ptr<GeometryStore> buildFromLayer(
    const VectorLayer &layer, int destinationEpsg, const Options &options = Options(),
    const std::function<bool(int, const QString &)> &progress = {},
    QString *error = nullptr);

  Stats stats() const;

  const std::vector<Feature> &features() const { return mFeatures; }
  /** Tipo de geometria de cada anillo (GeometryKind como entero). */
  const std::vector<uint8_t> &ringKinds() const { return mRingKinds; }

  /**
   * Devuelve el nivel pedido, construyendolo si aun no existe. Nunca devuelve
   * nullptr para `index` dentro de [0, maximumLevel()]. El puntero es estable
   * mientras viva el almacen.
   */
  const Level *acquireLevel(int index) const;

  /** Nivel cuyo error de simplificacion no supera `tolerance` (unidades de mapa). */
  int levelForTolerance(double tolerance) const;

  int maximumLevel() const { return mMaximumLevel; }
  int destinationEpsg() const { return mDestinationEpsg; }

  /** Elementos cuya envolvente corta `env`; se anaden a `out` (no se limpia). */
  void query(double minX, double minY, double maxX, double maxY, std::vector<uint32_t> &out) const;

  qint64 cachedVertices() const;

private:
  GeometryStore() = default;

  /** Reparto del tiempo de construccion; se publica en stats(). */
  qint64 mReadMs = 0;
  qint64 mIndexMs = 0;
  qint64 mLevelsMs = 0;

  struct RNode
  {
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    /** Hoja: indice en mLeafIds. Nodo interno: indice en mChildIds. */
    uint32_t first = 0;
    uint32_t count = 0;
    bool leaf = true;
  };

  void buildIndex();
  std::shared_ptr<Level> buildLevel(int index, const Level &raw) const;

  std::vector<Feature> mFeatures;
  std::vector<uint8_t> mRingKinds;

  std::vector<RNode> mNodes;
  /** Elementos (indices de mFeatures) de cada hoja, agrupados por nodo. */
  std::vector<uint32_t> mLeafIds;
  /**
   * Hijos de cada nodo interno, agrupados por nodo. Se guarda aparte porque el
   * empaquetado STR ordena los nodos por centroide: sus indices dejan de ser
   * consecutivos y no se puede deducir el hijo a partir del primero.
   */
  std::vector<uint32_t> mChildIds;
  uint32_t mRoot = 0;

  int mDestinationEpsg = 0;
  bool mReprojected = false;
  double mFullSpan = 1.0;   // lado mayor de la extension completa
  double mBaseTolerance = 1e-9;
  int mMaximumLevel = 24;

  mutable std::mutex mMutex;
  mutable std::map<int, std::shared_ptr<Level>> mLevels;
  mutable std::vector<uint32_t> mStack; // pila reutilizada por query()
};

} // namespace minigis
