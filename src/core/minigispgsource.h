#pragma once

#include <QString>

#include "minigiserror.h"

namespace minigis
{

/**
 * Datos de una conexion PostGIS declarada en un Connections.xml de Mapa Digital.
 *
 * No guarda la contrasena a proposito: el archivo la trae cifrada con un
 * esquema propietario que MiniGIS no puede descifrar. La contrasena se pide al
 * usuario y vive en PgCredentials, nunca en esta estructura ni en un proyecto.
 */
struct PgConnection
{
  QString name;
  QString server;
  int port = 5432;
  QString database;
  QString user;

  bool isValid() const
  {
    return !name.isEmpty() && !server.isEmpty() && !database.isEmpty();
  }

  /** Texto corto para el dialogo y para --info: "dataware_FDAC (10.0.0.1:5433/bd)". */
  QString displayName() const;

  /** Clave estable de la credencial recordada; no incluye la contrasena. */
  QString fingerprint() const;
};

/**
 * Fuente PostGIS: pg:<conexion>/<esquema>[/<tabla>].
 *
 * Es la identidad de una capa PostGIS. No lleva credenciales, se puede guardar
 * en un reciente o en un mensaje y se convierte en la cadena de conexion solo
 * en el momento de abrir (ver buildConninfo).
 */
class PgSource
{
public:
  QString connection;
  QString schema;
  /** Vacio cuando la fuente describe el catalogo de un esquema, no una tabla. */
  QString table;

  bool isValid() const { return !connection.isEmpty() && !schema.isEmpty(); }
  bool hasTable() const { return !table.isEmpty(); }

  /** Reconstruye la URI. Las tres partes se conservan tal cual, con mayusculas. */
  QString uri() const;

  /** True si la ruta empieza por el esquema pg: (sin distinguir mayusculas). */
  static bool looksLike(const QString &path);

  /**
   * Interpreta una URI pg:. Acepta dos segmentos (conexion y esquema) y tres
   * (conexion, esquema y tabla); cualquier otra forma devuelve Error con la
   * causa y el formato esperado.
   */
  static bool parse(const QString &uri, PgSource *source, Error *error = nullptr);
};

/** Escapa un valor para la cadena de conexion de libpq (\\ y '). */
QString escapeConninfo(const QString &value);

/**
 * Motivo de un fallo de PostGIS, para elegir el mensaje y el codigo de salida.
 * `MissingObject` cubre esquema o tabla inexistente; `Other`, lo que GDAL
 * informe y no sepamos clasificar.
 */
enum class PgFailure
{
  None,
  Catalog,           // no se pudo leer Connections.xml
  UnknownConnection, // el nombre no esta en el catalogo
  Connection,
  Authentication,
  MissingObject,
  DriverMissing,
  Other
};

/** Clasifica el mensaje que devuelve GDAL/libpq al no poder abrir. */
PgFailure classifyPgFailure(const QString &message);

/**
 * Construye la cadena PG:... que entiende el driver PostgreSQL de GDAL.
 *
 * La contrasena solo aparece aqui y en la llamada a GDALOpenEx. El resultado no
 * debe guardarse, imprimirse ni serializarse: sourcePath() usa la URI.
 */
QString buildConninfo(const PgSource &source, const PgConnection &connection,
                      const QString &password);

/**
 * Sustituye cualquier password=... por password=*** antes de que un texto
 * llegue a un mensaje, a un log o a la salida JSON. Tolera mayusculas, espacios
 * alrededor del igual, comillas simples o dobles y valores sin comillas.
 */
QString scrubSecrets(const QString &text);

} // namespace minigis
