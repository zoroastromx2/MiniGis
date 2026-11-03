#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

#include "minigiserror.h"
#include "minigislabelstyle.h"
#include "minigisrasterstyle.h"
#include "minigisrectangle.h"
#include "minigisstyle.h"
#include "minigisxmltree.h"
#include "minigisziparchive.h"

namespace minigis
{

/** `datasource` de una capa resuelto a una ruta local y una capa OGR. */
struct ProjectDatasource
{
  bool valid = false;
  /** Ruta tal cual, sin `file://` ni claves `|...`. */
  QString rawPath;
  /** Ruta absoluta resuelta respecto al directorio del proyecto. */
  QString absolutePath;
  /** Nombre de capa pedido con `|layername=`; vacio si el archivo tiene una. */
  QString layerName;
};

/**
 * Resuelve `ruta`, `ruta|layername=nombre` o `file://...` respecto al
 * directorio del proyecto. Las claves que MiniGIS no entiende (`|geometrytype=`,
 * `|uniqueid=`...) se ignoran.
 */
ProjectDatasource parseProjectDatasource(const QString &datasource, const QString &projectDirectory);

/**
 * Una capa declarada en el proyecto, ya interpretada.
 *
 * `type == Unsupported` significa que no se abrira (archivo que falta, proveedor
 * remoto, formato no soportado) pero que su XML debe conservarse intacto.
 */
struct ProjectLayer
{
  enum class Type
  {
    Vector,
    Raster,
    Unsupported
  };

  QString id;
  QString name;
  QString datasource;
  QString provider;
  QString sourcePath;
  QString layerName;
  Type type = Type::Unsupported;
  GeometryKind geometryKind = GeometryKind::Unknown;
  /** Nombre de geometria del maplayer (Point, MultiPolygon...). */
  QString geometryName;
  /** SRC de la capa, para generar un `<maplayer>` nuevo. */
  int crsEpsg = 0;
  QString crsWkt;
  bool visible = true;
  /** false si la simbologia no se puede representar y hay que conservarla. */
  bool styleRepresentable = true;
  /** false si las etiquetas no se pueden representar y hay que conservarlas. */
  bool labelsRepresentable = true;
  /** Motivo por el que la capa no esta disponible (vacio si lo esta). */
  QString unavailableReason;

  Style style;
  LabelStyle labels;
  RasterStyle raster;
};

/** Capa declarada que no se puede abrir, con el motivo para el usuario. */
struct UnavailableProjectLayer
{
  QString id;
  QString name;
  QString reason;
};

/**
 * Estado actual del mapa que se va a guardar en el proyecto: capas en orden de
 * fondo a frente (con los ids del proyecto o vacios si son nuevas), SRC y
 * encuadre del lienzo.
 */
struct ProjectSnapshot
{
  int crsEpsg = 4326;
  bool hasExtent = false;
  Rectangle extent;
  QVector<ProjectLayer> layers;
};

/**
 * Firma estable del estado guardable. Se usa para saber si hay cambios sin
 * depender de señales ni de banderas que se puedan olvidar.
 */
QByteArray projectSnapshotSignature(const ProjectSnapshot &snapshot);

/**
 * Proyecto QGIS en memoria.
 *
 * Solo interpreta el subconjunto que MiniGIS puede mostrar; el resto del
 * documento se conserva para volver a escribirlo sin perdidas. Las capas se
 * exponen de fondo a frente, que es el orden de apilado de `MapCanvas`.
 */
class QgisProject
{
public:
  /** True para `.qgz` y `.qgs` (sin distinguir mayusculas). */
  static bool isProjectPath(const QString &path);

  /**
   * Abre un proyecto. En caso de fallo devuelve false y rellena `error` con la
   * causa y la accion sugerida. Un `.qgz` sin entrada `.qgs` o un XML mal
   * formado son errores del proyecto, no de una capa.
   */
  static bool load(const QString &path, std::unique_ptr<QgisProject> *project, Error *error);

  QString path() const { return mPath; }
  bool isCompressed() const { return mCompressed; }
  QString qgsEntryName() const { return mQgsEntryName; }

  int crsEpsg() const { return mProjectEpsg; }
  /** SRC del lienzo del proyecto (`<mapcanvas><destinationsrs>`), o el del proyecto. */
  int canvasEpsg() const { return mCanvasEpsg; }

  bool hasExtent() const { return mHasExtent; }
  Rectangle extent() const { return mExtent; }
  int extentEpsg() const { return mExtentEpsg; }

  /** Capas abribles, de fondo a frente. */
  const QVector<ProjectLayer> &layers() const { return mLayers; }

  /** Capas declaradas que no se pueden abrir; su XML se conserva. */
  const QVector<UnavailableProjectLayer> &unavailableLayers() const { return mUnavailable; }

  /** Numero de capas declaradas (abribles + no disponibles). */
  int declaredLayerCount() const { return static_cast<int>(mLayers.size() + mUnavailable.size()); }

  /**
   * Guarda el proyecto en `targetPath` (`.qgz` o `.qgs`). Si cambia la carpeta,
   * reescribe los `datasource` para que sigan apuntando a los mismos archivos.
   * La escritura es atomica y deja `<archivo>.bak` con la version anterior.
   */
  bool save(const QString &targetPath, Error *error);

  /**
   * Guarda aplicando el estado del mapa: visibilidad, simbologia, etiquetas,
   * estilo raster, encuadre, capas nuevas y capas cerradas. Lo que no ha
   * cambiado se conserva tal cual.
   */
  bool save(const QString &targetPath, const ProjectSnapshot &snapshot, Error *error);

  /** Documento completo, para el guardado. */
  const XmlNode &document() const { return mDocument; }

private:
  QgisProject() = default;

  /** Reescribe los datasource al guardar en otra carpeta (sobre una copia). */
  static void rewriteDatasources(XmlNode *document, const QString &oldDirectory, const QString &newDirectory);  QString mPath;
  bool mCompressed = false;
  QString mQgsEntryName;
  QString mProjectDirectory;
  XmlNode mDocument;
  int mProjectEpsg = 0;
  int mCanvasEpsg = 4326;
  /** SRC real del `<mapcanvas>` guardado (EPSG o WKT propio). */
  int mCanvasSrsEpsg = 0;
  QString mCanvasSrsWkt;
  bool mHasExtent = false;
  Rectangle mExtent;
  int mExtentEpsg = 4326;
  QVector<ProjectLayer> mLayers;
  QVector<UnavailableProjectLayer> mUnavailable;
  /** Entradas del .qgz original (sin la del .qgs), para conservarlas al guardar. */
  QVector<ZipEntry> mZipEntries;
};

} // namespace minigis
