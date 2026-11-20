#include "minigisconnections.h"

#include <QFile>
#include <QMutex>
#include <QSettings>
#include <QXmlStreamReader>

namespace minigis
{
namespace
{

QString attribute(const QXmlStreamAttributes &attributes, const char *name)
{
  return attributes.value(QLatin1String(name)).toString().trimmed();
}

// Un unico catalogo compartido: el proceso usa un Connections.xml. El mutex
// protege la carga porque las capas PostGIS se abren desde el hilo del cargador.
QMutex gMutex;
QString gExplicitPath;
QString gLoadedPath;
PgConnectionCatalog gSharedCatalog;

} // namespace

bool PgConnectionCatalog::load(const QString &path, PgConnectionCatalog *catalog, Error *error)
{
  const auto fail = [error](const QString &message, const QString &hint) {
    if (error)
      *error = Error {message, hint};
    return false;
  };

  if (!catalog)
    return fail(QStringLiteral("No hay donde guardar el catalogo de conexiones."),
                QStringLiteral("Es un defecto interno de MiniGIS: informa del fallo."));

  *catalog = PgConnectionCatalog();

  const QString cleanPath = path.trimmed();
  if (cleanPath.isEmpty())
  {
    return fail(QStringLiteral("No se ha indicado el archivo de conexiones."),
                QStringLiteral("Usa --connections <ruta> o define MINIGIS_CONNECTIONS con la ruta "
                               "del Connections.xml de Mapa Digital."));
  }

  QFile file(cleanPath);
  if (!file.open(QIODevice::ReadOnly))
  {
    return fail(QStringLiteral("No se pudo abrir el catalogo de conexiones «%1».").arg(cleanPath),
                QStringLiteral("Comprueba la ruta y que el archivo se pueda leer."));
  }

  QXmlStreamReader xml(&file);
  while (!xml.atEnd())
  {
    xml.readNext();

    if (!xml.isStartElement() || xml.name() != QLatin1String("ConStr"))
      continue;

    const QXmlStreamAttributes attributes = xml.attributes();

    // Solo PostgreSQL/PostGIS. Las entradas GPKG describen archivos de otro
    // producto y no son una conexion que MiniGIS pueda abrir.
    if (attribute(attributes, "TYPE").compare(QStringLiteral("PGIS"), Qt::CaseInsensitive) != 0)
      continue;

    PgConnection connection;
    connection.name = attribute(attributes, "NAME");
    connection.server = attribute(attributes, "SERVER");
    connection.database = attribute(attributes, "DATABASE");
    connection.user = attribute(attributes, "USER");

    bool portOk = false;
    const int port = attribute(attributes, "PORT").toInt(&portOk);
    connection.port = (portOk && port > 0) ? port : 5432;

    if (!connection.isValid())
    {
      catalog->mWarnings << QStringLiteral("Conexion «%1» incompleta: faltan servidor, base o nombre.")
                              .arg(connection.name.isEmpty() ? QStringLiteral("(sin nombre)")
                                                             : connection.name);
      continue;
    }

    if (catalog->find(connection.name))
    {
      catalog->mWarnings << QStringLiteral("Conexion «%1» repetida: se conserva la primera.")
                              .arg(connection.name);
      continue;
    }

    catalog->mConnections.append(connection);
  }

  if (xml.hasError())
  {
    return fail(QStringLiteral("«%1» no es un XML valido: %2").arg(cleanPath, xml.errorString()),
                QStringLiteral("Revisa el archivo; el catalogo de Mapa Digital es <servers> con "
                               "elementos <ConStr>."));
  }

  if (catalog->mConnections.isEmpty())
  {
    return fail(QStringLiteral("«%1» no declara ninguna conexion PGIS.").arg(cleanPath),
                QStringLiteral("Anade una entrada <ConStr TYPE=\"PGIS\" ...> o elige otro archivo."));
  }

  return true;
}

QString PgConnectionCatalog::resolvePath(const QString &explicitPath)
{
  if (!explicitPath.trimmed().isEmpty())
    return explicitPath.trimmed();

  const QByteArray environment = qgetenv("MINIGIS_CONNECTIONS");
  if (!environment.isEmpty())
    return QString::fromLocal8Bit(environment);

  QSettings settings;
  return settings.value(QStringLiteral("postgis/xmlPath")).toString();
}

void PgConnectionCatalog::setExplicitPath(const QString &path)
{
  QMutexLocker locker(&gMutex);
  gExplicitPath = path;
  // Al cambiar de archivo se fuerza la recarga del catalogo compartido.
  gLoadedPath.clear();
}

QString PgConnectionCatalog::explicitPath()
{
  QMutexLocker locker(&gMutex);
  return gExplicitPath;
}

const PgConnectionCatalog *PgConnectionCatalog::shared(Error *error)
{
  QMutexLocker locker(&gMutex);

  const QString path = resolvePath(gExplicitPath);
  if (path.isEmpty())
  {
    if (error)
      *error = Error {QStringLiteral("No se ha indicado el archivo de conexiones."),
                      QStringLiteral("Usa --connections <ruta> o define MINIGIS_CONNECTIONS con "
                                     "la ruta del Connections.xml de Mapa Digital.")};
    return nullptr;
  }

  if (path == gLoadedPath)
    return &gSharedCatalog;

  if (!load(path, &gSharedCatalog, error))
    return nullptr;

  gLoadedPath = path;
  return &gSharedCatalog;
}

QStringList PgConnectionCatalog::names() const
{
  QStringList result;
  result.reserve(mConnections.size());
  for (const PgConnection &connection : mConnections)
    result << connection.name;
  return result;
}

const PgConnection *PgConnectionCatalog::find(const QString &name) const
{
  for (const PgConnection &connection : mConnections)
  {
    if (connection.name.compare(name, Qt::CaseInsensitive) == 0)
      return &connection;
  }
  return nullptr;
}

} // namespace minigis
