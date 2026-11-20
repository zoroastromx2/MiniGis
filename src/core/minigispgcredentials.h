#pragma once

#include <QHash>
#include <QMutex>
#include <QString>

#include "minigiserror.h"
#include "minigispgsource.h"

namespace minigis
{

/**
 * Contrasenas de las conexiones PostGIS.
 *
 * Regla del proyecto: la contrasena de Connections.xml esta cifrada con un
 * esquema propietario y MiniGIS no la descifra. Se pide al usuario y se guarda:
 *
 *   * en memoria, mientras la aplicacion vive (para no pedirla por cada tabla);
 *   * opcionalmente, cifrada con DPAPI de Windows en el perfil del usuario.
 *
 * El acceso es thread-safe porque el cargador resuelve la contrasena desde su
 * hilo de trabajo, no desde el de la interfaz.
 */
class PgCredentials
{
public:
  static PgCredentials &instance();

  /**
   * Contrasena conocida para esa conexion, o vacio si no hay ninguna (entonces
   * libpq podra usar PGPASSWORD, PGPASSFILE o pgpass.conf).
   */
  QString passwordFor(const PgConnection &connection);

  /**
   * Recuerda la contrasena de la sesion y, si `persist` es true, la cifra y la
   * deja en el perfil. Con `persist` false se olvida la copia persistida: es lo
   * que espera quien desmarca «Recordar».
   */
  bool remember(const PgConnection &connection, const QString &password, bool persist,
                Error *error = nullptr);

  /** Borra la contrasena de la sesion y la persistida. */
  void forget(const PgConnection &connection);

  /** Vacia solo la cache de la sesion (util en pruebas y al cerrar). */
  void clearSession();

  /** Archivo del almacen cifrado; util para pruebas y diagnostico. */
  static QString storePath();

private:
  PgCredentials() = default;

  void ensureLoaded();
  bool save(Error *error);

  QMutex mMutex;
  QHash<QString, QString> mSession;
  /** fingerprint -> blob DPAPI en base64. Nunca contiene texto en claro. */
  QHash<QString, QString> mPersistent;
  bool mLoaded = false;
};

} // namespace minigis
