#pragma once

#include <QMutex>
#include <QString>

#include "minigispgsource.h"

namespace minigis
{

/**
 * Conexion PostGIS activa de la aplicacion.
 *
 * Evita pedir credenciales y volver a conectar cada vez que se abre el dialogo:
 * guarda en memoria la conexion, la contrasena y el ultimo esquema mientras la
 * aplicacion vive. Al reiniciar, el dialogo recupera la contrasena recordada
 * (DPAPI) y se conecta solo.
 *
 * No guarda la contrasena en disco: eso lo hace PgCredentials, cifrada.
 */
class PgSession
{
public:
  static PgSession &instance();

  bool isActive() const;

  PgConnection connection() const;
  QString password() const;
  QString schema() const;

  void setSchema(const QString &schema);

  /** Activa la sesion tras una conexion correcta. */
  void activate(const PgConnection &connection, const QString &password);

  /** Olvida la sesion (no borra la contrasena recordada en disco). */
  void clear();

private:
  PgSession() = default;

  mutable QMutex mMutex;
  PgConnection mConnection;
  QString mPassword;
  QString mSchema;
};

} // namespace minigis
