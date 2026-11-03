/**
 * Prueba de las piezas de persistencia: arbol XML y ZIP de un proyecto QGIS.
 *
 * Comprueba sin ventana que:
 *   * un .qgz real se lee y contiene su .qgs;
 *   * el XML se puede volver a serializar sin perder el nodo desconocido;
 *   * un ZIP escrito por MiniGIS se puede volver a leer con las mismas entradas.
 *
 * Uso: minigisproyectoiotest <proyecto.qgz> <salida.qgz>
 */

#include "minigisgdalinit.h"
#include "minigisqgisproject.h"
#include "minigisxmltree.h"
#include "minigisziparchive.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>

#include <cmath>
#include <memory>

#include <cstdio>

using namespace minigis;

namespace
{

int fail(const QString &message)
{
  std::fprintf(stderr, "[FALLO] %s\n", message.toUtf8().constData());
  return 1;
}

} // namespace

int main(int argc, char *argv[])
{
  if (argc < 3)
    return fail(QStringLiteral("Uso: minigisproyectoiotest <proyecto.qgz> <salida.qgz>"));

  const QString source = QString::fromLocal8Bit(argv[1]);
  const QString target = QString::fromLocal8Bit(argv[2]);

  initializeGdal();

  Error error;
  QVector<ZipEntry> entries;
  if (!ZipArchive::read(source, &entries, &error))
    return fail(error.message);

  const QString projectEntry = ZipArchive::projectEntryName(entries);
  if (projectEntry.isEmpty())
    return fail(QStringLiteral("El .qgz no contiene ninguna entrada .qgs"));

  QByteArray projectXml;
  for (const ZipEntry &entry : entries)
  {
    if (entry.name == projectEntry)
      projectXml = entry.data;
  }

  XmlNode document;
  if (!XmlNode::parse(projectXml, &document, &error))
    return fail(error.message);
  if (document.name() != QStringLiteral("qgis"))
    return fail(QStringLiteral("La raiz del .qgs no es <qgis>"));
  if (!document.child(QStringLiteral("minigisPrueba")))
    return fail(QStringLiteral("Se perdio el nodo desconocido al leer"));

  const QByteArray serialized = document.toXml();
  if (serialized.isEmpty())
    return fail(QStringLiteral("No se pudo serializar el arbol XML"));

  XmlNode reparsed;
  if (!XmlNode::parse(serialized, &reparsed, &error))
    return fail(QStringLiteral("El XML serializado no se puede volver a leer: %1").arg(error.message));
  if (!reparsed.child(QStringLiteral("minigisPrueba")))
    return fail(QStringLiteral("Se perdio el nodo desconocido al serializar"));
  if (!reparsed.child(QStringLiteral("projectlayers")))
    return fail(QStringLiteral("Se perdio <projectlayers> al serializar"));

  for (ZipEntry &entry : entries)
  {
    if (entry.name == projectEntry)
      entry.data = serialized;
  }

  const QString directory = QFileInfo(target).absolutePath();
  QDir().mkpath(directory);

  if (!ZipArchive::write(target, entries, &error))
    return fail(error.message);

  QVector<ZipEntry> written;
  if (!ZipArchive::read(target, &written, &error))
    return fail(error.message);
  if (written.size() != entries.size())
    return fail(QStringLiteral("El ZIP escrito tiene %1 entradas y se esperaban %2")
                  .arg(written.size())
                  .arg(entries.size()));

  QString writtenProject;
  for (const ZipEntry &entry : written)
  {
    if (entry.name == projectEntry)
      writtenProject = entry.name;
  }
  if (writtenProject != projectEntry)
    return fail(QStringLiteral("El ZIP escrito no conserva la entrada %1").arg(projectEntry));

  // Segunda parte: cargar el proyecto, cambiar el estado que guardaria la
  // interfaz (visibilidad, relleno, etiquetas, opacidad) y comprobar que se
  // escribe y se vuelve a leer.
  std::unique_ptr<QgisProject> loadedProject;
  if (!QgisProject::load(source, &loadedProject, &error))
    return fail(error.message);

  ProjectSnapshot snapshot;
  snapshot.crsEpsg = loadedProject->canvasEpsg();
  snapshot.hasExtent = loadedProject->hasExtent();
  snapshot.extent = loadedProject->extent();
  snapshot.layers = loadedProject->layers();

  for (ProjectLayer &layer : snapshot.layers)
  {
    if (layer.name == QStringLiteral("parcels"))
    {
      layer.visible = false;
      layer.style.fillColor = QColor(10, 20, 30);
      layer.style.strokeWidth = 2.0;
    }
    else if (layer.name == QStringLiteral("cities"))
    {
      layer.labels.fields = QStringList {QStringLiteral("name"), QStringLiteral("country")};
      layer.labels.fontSize = 12.0;
      layer.labels.haloEnabled = false;
    }
    else if (layer.name == QStringLiteral("orto"))
    {
      layer.raster.opacity = 50;
    }
  }

  const QString modifiedTarget = target + QStringLiteral(".modificado.qgz");

  // Quitar una capa y anadir otra: el proyecto guardado debe reflejar el mapa.
  for (int index = snapshot.layers.size() - 1; index >= 0; --index)
  {
    if (snapshot.layers.at(index).name == QStringLiteral("parcels_categorizado"))
      snapshot.layers.removeAt(index);
  }

  ProjectLayer added;
  added.type = ProjectLayer::Type::Vector;
  added.name = QStringLiteral("parcels_utm30");
  added.provider = QStringLiteral("ogr");
  added.sourcePath = QDir::cleanPath(
    QDir(QFileInfo(source).absolutePath()).absoluteFilePath(QStringLiteral("../shapefiles/parcels_utm30.shp")));
  added.geometryKind = GeometryKind::Polygon;
  added.geometryName = QStringLiteral("MultiPolygon");
  added.crsEpsg = 25830;
  added.style = Style::defaultForKind(GeometryKind::Polygon, 1);
  snapshot.layers.append(added);

  if (!loadedProject->save(modifiedTarget, snapshot, &error))
    return fail(error.message);

  std::unique_ptr<QgisProject> reloaded;
  if (!QgisProject::load(modifiedTarget, &reloaded, &error))
    return fail(error.message);

  if (!reloaded->document().child(QStringLiteral("minigisPrueba")))
    return fail(QStringLiteral("El guardado de cambios perdio el nodo desconocido"));

  bool parcelsOk = false;
  bool citiesOk = false;
  bool rasterOk = false;
  bool addedOk = false;
  bool removedOk = true;
  for (const ProjectLayer &layer : reloaded->layers())
  {
    if (layer.name == QStringLiteral("parcels"))
    {
      parcelsOk = !layer.visible && layer.style.fillColor == QColor(10, 20, 30)
                  && std::abs(layer.style.strokeWidth - 2.0) < 1e-9;
    }
    else if (layer.name == QStringLiteral("cities"))
    {
      citiesOk = layer.labels.enabled && layer.labels.fields.size() == 2
                 && layer.labels.fields.at(0) == QStringLiteral("name")
                 && layer.labels.fields.at(1) == QStringLiteral("country")
                 && std::abs(layer.labels.fontSize - 12.0) < 1e-9 && !layer.labels.haloEnabled;
    }
    else if (layer.name == QStringLiteral("orto"))
    {
      rasterOk = layer.raster.opacity == 50;
    }
    else if (layer.name == QStringLiteral("parcels_utm30"))
    {
      addedOk = !layer.id.isEmpty();
    }
    else if (layer.name == QStringLiteral("parcels_categorizado"))
    {
      removedOk = false;
    }
  }

  if (!parcelsOk)
    return fail(QStringLiteral("El cambio de visibilidad/estilo de parcels no se guardo"));
  if (!citiesOk)
    return fail(QStringLiteral("El cambio de etiquetas de cities no se guardo"));
  if (!rasterOk)
    return fail(QStringLiteral("El cambio de opacidad del raster no se guardo"));
  if (!addedOk)
    return fail(QStringLiteral("La capa anadida no se guardo con id"));
  if (!removedOk)
    return fail(QStringLiteral("La capa cerrada sigue en el proyecto"));

  // Un segundo guardado con los ids releidos no debe duplicar la capa nueva.
  ProjectSnapshot second;
  second.crsEpsg = reloaded->canvasEpsg();
  second.hasExtent = reloaded->hasExtent();
  second.extent = reloaded->extent();
  second.layers = reloaded->layers();
  if (!reloaded->save(modifiedTarget, second, &error))
    return fail(error.message);

  std::unique_ptr<QgisProject> twice;
  if (!QgisProject::load(modifiedTarget, &twice, &error))
    return fail(error.message);
  int addedCount = 0;
  for (const ProjectLayer &layer : twice->layers())
  {
    if (layer.name == QStringLiteral("parcels_utm30"))
      ++addedCount;
  }
  if (addedCount != 1)
    return fail(QStringLiteral("El segundo guardado duplico la capa nueva (%1 copias)").arg(addedCount));

  // Proyecto con SRC local sin codigo EPSG: el encuadre debe sobrevivir a la
  // ida y vuelta, no convertirse en un punto ni perderse.
  const QString localProject = QDir(QFileInfo(source).absolutePath())
                                 .absoluteFilePath(QStringLiteral("qgis_local.qgz"));
  if (QFileInfo::exists(localProject))
  {
    std::unique_ptr<QgisProject> local;
    if (!QgisProject::load(localProject, &local, &error))
      return fail(QStringLiteral("No se pudo leer el proyecto con SRC local: %1").arg(error.message));
    if (local->crsEpsg() != 0 || local->canvasEpsg() != 4326)
      return fail(QStringLiteral("El SRC local no se detecto como personalizado (crs=%1 canvas=%2)")
                    .arg(local->crsEpsg())
                    .arg(local->canvasEpsg()));
    if (!local->hasExtent() || local->extent().width() <= 0.01 || local->extent().height() <= 0.01)
      return fail(QStringLiteral("El encuadre del SRC local no se transformo a grados"));

    ProjectSnapshot localSnapshot;
    localSnapshot.crsEpsg = local->canvasEpsg();
    localSnapshot.hasExtent = local->hasExtent();
    localSnapshot.extent = local->extent();
    localSnapshot.layers = local->layers();

    const QString localTarget = target + QStringLiteral(".local.qgz");
    if (!local->save(localTarget, localSnapshot, &error))
      return fail(QStringLiteral("No se pudo guardar el proyecto con SRC local: %1").arg(error.message));

    std::unique_ptr<QgisProject> localReloaded;
    if (!QgisProject::load(localTarget, &localReloaded, &error))
      return fail(QStringLiteral("No se pudo releer el proyecto con SRC local: %1").arg(error.message));
    if (!localReloaded->hasExtent() || localReloaded->extent().width() <= 0.01)
      return fail(QStringLiteral("El encuadre del SRC local se perdio al guardar"));
    if (std::abs(localReloaded->extent().width() - local->extent().width()) > local->extent().width() * 0.01)
      return fail(QStringLiteral("El encuadre del SRC local cambio demasiado al guardar (%1 -> %2)")
                    .arg(local->extent().width(), 0, 'g', 10)
                    .arg(localReloaded->extent().width(), 0, 'g', 10));

    // Mover la vista (como al hacer zoom) y guardar: el encuadre debe quedar en
    // el SRC local y volver a mostrarse aproximadamente igual al reabrir.
    ProjectSnapshot moved;
    moved.crsEpsg = local->canvasEpsg();
    moved.hasExtent = true;
    const Rectangle original = local->extent();
    moved.extent = Rectangle(original.centerX() - original.width() / 4.0,
                             original.centerY() - original.height() / 4.0,
                             original.centerX() + original.width() / 4.0,
                             original.centerY() + original.height() / 4.0);
    moved.layers = local->layers();

    if (!local->save(localTarget, moved, &error))
      return fail(QStringLiteral("No se pudo guardar el proyecto local con otra vista: %1").arg(error.message));

    std::unique_ptr<QgisProject> movedReloaded;
    if (!QgisProject::load(localTarget, &movedReloaded, &error))
      return fail(QStringLiteral("No se pudo releer el proyecto local movido: %1").arg(error.message));
    if (!movedReloaded->hasExtent()
        || std::abs(movedReloaded->extent().width() - moved.extent.width()) > moved.extent.width() * 0.02)
      return fail(QStringLiteral("El encuadre local movido no se conservo (%1 -> %2)")
                    .arg(moved.extent.width(), 0, 'g', 10)
                    .arg(movedReloaded->extent().width(), 0, 'g', 10));

    std::printf("[OK] SRC local: encuadre conservado al guardar y al mover la vista\n");
  }

  std::printf("[OK] cambios guardados y releidos: %s\n", qPrintable(modifiedTarget));
  return 0;
}
