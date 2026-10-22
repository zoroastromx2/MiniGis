#pragma once

#include <QString>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "minigisstyle.h"

class OGRLayer;
class OGRSpatialReference;
class OGRCoordinateTransformation;

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
  /**
   * Como se guarda la capa.
   *
   *   * `InMemory`: la geometria completa en buffers planos (rapido, pero necesita
   *     ~24 bytes por vertice). Es el modo normal.
   *   * `Envelopes`: solo las envolventes (~40 bytes por elemento) mas los
   *     desplazamientos del archivo; la geometria de lo que se ve se lee bajo
   *     demanda y se cachea. Es el modo para capas que no caben: 3,2 M de
   *     edificios ocupan ~130 MB de indice en lugar de ~750 MB de geometria, y
   *     el cuadro deja de depender del tamano de la capa.
   */
  enum class Mode
  {
    InMemory,
    Envelopes
  };

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
    qint64 vertices = 0;   // vertices del nivel 0 (0 en modo envolventes)
    qint64 rings = 0;
    qint64 bytes = 0;      // memoria aproximada de todos los buffers
    int cachedLevels = 0;
    int destinationEpsg = 0; // 0 = coordenadas en el SRC de origen
    bool reprojected = false;
    /** Reparto del tiempo de construccion (diagnostico de rendimiento). */
    qint64 readMs = 0;
    qint64 indexMs = 0;
    qint64 levelsMs = 0;
    Mode mode = Mode::InMemory;
    /** Modo envolventes: geometria leida bajo demanda y elementos en cache. */
    qint64 cachedGeometries = 0;
    qint64 geometriesRead = 0;
    qint64 cachedGeometriesBytes = 0;
  };

  /**
   * Geometria de los elementos visibles, lista para dibujar: un nivel
   * simplificado (igual que en modo memoria) mas los anillos de cada elemento,
   * indexados por su posicion en la lista de candidatos de la consulta.
   */
  struct Subset
  {
    std::shared_ptr<const Level> level;
    std::vector<uint32_t> entryFirstRing;
    std::vector<uint32_t> entryRingCount;
    /** Tipo de geometria de cada anillo del subconjunto. */
    std::vector<uint8_t> entryRingKind;
    /**
     * Elementos que NO se leen y se dibujan como punto (basta su envolvente). Se
     * usan cuando la escala es tan lejana que cada elemento ocupa menos de un
     * pixel: leer su geometria seria I/O aleatoria para dibujar algo
     * indistinguible. Una capa de 3,2 M de edificios vista entera pasa de 81 s
     * por cuadro a decenas de milisegundos.
     */
    std::vector<uint32_t> dotEntries;
    qint64 readFeatures = 0;    // leidos del archivo en este cuadro
    qint64 missingFeatures = 0; // visibles que no se han leido
    bool geometryRead = true;   // false = a esta escala solo se dibujan puntos
  };

  /**
   * Geometria de un elemento leida del archivo en modo `Envelopes`. Es publica
   * por comodidad de implementacion (la usan los ayudantes del .cpp), no como
   * parte del contrato de uso.
   */
  struct CachedGeometry
  {
    std::vector<double> coords;
    std::vector<uint32_t> ringFirst;
    std::vector<uint32_t> ringCount;
    std::vector<uint8_t> ringKind;
  };

  struct Options
  {
    /** Vertices maximos que se aceptan en memoria (0 = sin limite). */
    qint64 vertexBudget = 8'000'000;
    /** Niveles de simplificacion que se precalculan al construir (ademas del 0). */
    int precomputedLevels = 6;
    /** Memoria maxima de los niveles precalculados, en bytes. */
    qint64 precomputeBytes = 192LL * 1024 * 1024;
    /** Elementos que se mantienen en la cache de geometria (modo `Envelopes`). */
    qint64 cacheBudget = 300000;
    /** Si el modo `InMemory` no cabe, se intenta el indice de envolventes. */
    bool allowEnvelopeFallback = true;
  };

  /**
   * Lee la capa completa. `progress` recibe (porcentaje 0..100, mensaje) y puede
   * devolver false para cancelar. Si se cancela o se supera `vertexBudget`
   * devuelve nullptr y explica el motivo en `error` (no es un fallo fatal: el
   * renderizador puede seguir usando el camino clasico).
   *
   * El SRC de origen se pasa aparte (y no se lee de `layer`) porque una capa
   * puede no declararlo y heredar el de otra capa del archivo: en ese caso
   * `OGRLayer::GetSpatialRef()` devuelve nulo y sin la reproyeccion las
   * geometrias caen fuera de la vista.
   */
  static std::shared_ptr<GeometryStore> build(
    OGRLayer *layer, const OGRSpatialReference *sourceSrs, int destinationEpsg, const Options &options,
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

  /**
   * Construye un indice de solo envolventes (modo `Envelopes`) recorriendo el
   * .shp sin leer la geometria. Solo para el driver ESRI Shapefile; para el resto
   * de formatos devuelve nullptr con el motivo en `error`.
   *
   * El almacen resultante **presta** la capa OGR (no es dueño del dataset): las
   * lecturas bajo demanda se hacen con `OGR_L_GetFeature(fid)` desde el hilo que
   * dibuja, y la capa debe seguir viva mientras el almacen se use.
   */
  static std::shared_ptr<GeometryStore> buildEnvelopeIndex(
    const VectorLayer &layer, int destinationEpsg, const Options &options,
    const std::function<bool(int, const QString &)> &progress = {},
    QString *error = nullptr);

  Mode mode() const { return mMode; }

  /**
   * Prepara para dibujar los elementos indicados: lee del archivo los que falten
   * (hasta `readBudget`) y devuelve el nivel simplificado a `tolerance`.
   *
   * `minimumGeometrySize` es el tamano minimo (en unidades de mapa) que debe
   * tener un elemento para que valga la pena leer su geometria: por debajo de el
   * se dibujan puntos con la envolvente, hasta `dotBudget` elementos. Pasar 0
   * desactiva esa degradacion (siempre se intenta leer la geometria).
   *
   * Nunca devuelve nullptr en modo envolventes.
   */
  std::shared_ptr<const Subset> acquireSubset(const std::vector<uint32_t> &candidates, double tolerance,
                                              qint64 readBudget, double minimumGeometrySize = 0.0,
                                              qint64 dotBudget = 0) const;

  Stats stats() const;

  const std::vector<Feature> &features() const { return mFeatures; }
  /** Tipo de geometria de cada anillo (GeometryKind como entero). */
  const std::vector<uint8_t> &ringKinds() const { return mRingKinds; }

  /** Anillo inicial y numero de anillos de cada elemento, en arrays planos. */
  const std::vector<uint32_t> &entryFirstRing() const { return mEntryFirstRing; }
  const std::vector<uint32_t> &entryRingCount() const { return mEntryRingCount; }

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

  /** Lee del archivo la geometria de `fid` y la guarda en la cache. */
  bool fetchGeometry(uint32_t fid) const;
  void evictCacheIfNeeded() const;

  std::vector<Feature> mFeatures;
  std::vector<uint8_t> mRingKinds;

  /** Arrays planos por elemento (mismo contenido que mFeatures, mejor recorrido). */
  std::vector<uint32_t> mEntryFirstRing;
  std::vector<uint32_t> mEntryRingCount;

  Mode mMode = Mode::InMemory;

  // Modo envolventes: capa prestada, reproyeccion propia y cache de geometria.
  OGRLayer *mBorrowedLayer = nullptr;
  std::unique_ptr<OGRCoordinateTransformation> mTransformation;
  mutable std::unordered_map<uint32_t, std::shared_ptr<CachedGeometry>> mCache;
  mutable std::deque<uint32_t> mCacheOrder;
  mutable qint64 mCacheBytes = 0;
  qint64 mCacheBudget = 300000;
  mutable qint64 mGeometriesRead = 0;
  mutable std::vector<uint32_t> mMissingScratch;

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
  /**
   * False cuando simplificar no reduce nada (capas de puntos): se dibuja siempre
   * el nivel 0 y no se construyen niveles duplicados.
   */
  bool mSimplificationUseful = true;

  mutable std::mutex mMutex;
  mutable std::map<int, std::shared_ptr<Level>> mLevels;
  mutable std::vector<uint32_t> mStack; // pila reutilizada por query()
};

} // namespace minigis
