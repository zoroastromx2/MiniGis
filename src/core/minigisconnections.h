#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "minigiserror.h"
#include "minigispgsource.h"

namespace minigis
{

/**
 * Catalogo de conexiones de un Connections.xml de Mapa Digital.
 *
 * Solo entiende las entradas TYPE="PGIS": las de GeoPackage/SQLite son de otro
 * producto y no se listan. El atributo PASSWORD no se lee nunca; el archivo lo
 * guarda cifrado con un esquema propietario y MiniGIS pide la contrasena al
 * usuario (ver PgCredentials).
 *
 * El archivo real esta en ISO-8859-1; QXmlStreamReader detecta la codificacion
 * de la declaracion XML, de modo que no hay que convertir a mano.
 */
class PgConnectionCatalog
{
public:
  /**
   * Lee `path` y sustituye el contenido del catalogo. Devuelve false y rellena
   * `error` si el archivo no existe, no es XML o no declara ninguna conexion
   * PGIS.
   */
  static bool load(const QString &path, PgConnectionCatalog *catalog, Error *error = nullptr);

  /**
   * Ruta del catalogo, por orden: la indicada en la CLI (--connections), la
   * variable de entorno MINIGIS_CONNECTIONS y el ajuste guardado por el GUI.
   * Devuelve vacio si no hay ninguna.
   */
  static QString resolvePath(const QString &explicitPath = QString());

  /** Ruta explicita fijada al arrancar (--connections); vacia si no hay. */
  static void setExplicitPath(const QString &path);
  static QString explicitPath();

  /**
   * Catalogo compartido de la ruta resuelta. Se carga en la primera llamada y
   * se reutiliza: abrir cada capa PostGIS no puede releer el XML ni bloquear
   * con un QMutex por tabla. Devuelve nullptr y rellena `error` si no hay ruta
   * o el archivo no se puede leer.
   */
  static const PgConnectionCatalog *shared(Error *error = nullptr);

  const QVector<PgConnection> &connections() const { return mConnections; }
  QStringList names() const;

  /** Puntero prestado a la conexion con ese nombre, o nullptr. */
  const PgConnection *find(const QString &name) const;

  /** Problemas no fatales (entradas incompletas, nombres repetidos). */
  QStringList warnings() const { return mWarnings; }

private:
  QVector<PgConnection> mConnections;
  QStringList mWarnings;
};

} // namespace minigis
