#include "minigispgsource.h"

#include <QRegularExpression>
#include <QStringList>

namespace minigis
{
namespace
{

/** Formato que se repite en los mensajes de error. */
QString sourceFormat()
{
  return QStringLiteral("pg:dataware_FDAC/mgn_2025/poligonos_de_manzana");
}

} // namespace

QString PgConnection::displayName() const
{
  return QStringLiteral("%1 (%2:%3/%4)")
    .arg(name, server, QString::number(port), database);
}

QString PgConnection::fingerprint() const
{
  // El nombre participa en la clave: dos conexiones del mismo servidor con
  // usuarios distintos no deben compartir credencial recordada.
  return QStringLiteral("%1|%2|%3|%4").arg(name, server, QString::number(port), user);
}

QString PgSource::uri() const
{
  QString result = QStringLiteral("pg:") + connection + QLatin1Char('/') + schema;
  if (!table.isEmpty())
    result += QLatin1Char('/') + table;
  return result;
}

bool PgSource::looksLike(const QString &path)
{
  return path.startsWith(QStringLiteral("pg:"), Qt::CaseInsensitive);
}

bool PgSource::parse(const QString &uri, PgSource *source, Error *error)
{
  const auto fail = [error](const QString &message, const QString &hint) {
    if (error)
      *error = Error {message, hint};
    return false;
  };

  if (!source)
    return fail(QStringLiteral("No hay donde guardar la fuente PostGIS."),
                QStringLiteral("Es un defecto interno de MiniGIS: informa del fallo."));

  *source = PgSource();

  const QString text = uri.trimmed();
  if (!looksLike(text))
  {
    return fail(QStringLiteral("«%1» no es una conexion PostGIS.").arg(text),
                QStringLiteral("El formato es %1").arg(sourceFormat()));
  }

  // No se recorta cada segmento: los identificadores de PostgreSQL pueden
  // llevar espacios y comillas, y recortarlos abriria otra tabla en silencio.
  const QStringList parts = text.mid(3).split(QLatin1Char('/'), Qt::KeepEmptyParts);
  if (parts.size() != 2 && parts.size() != 3)
  {
    return fail(QStringLiteral("«%1» no indica conexion y esquema.").arg(text),
                QStringLiteral("Formato: pg:conexion/esquema o %1").arg(sourceFormat()));
  }

  for (const QString &part : parts)
  {
    if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral(".."))
    {
      return fail(QStringLiteral("La conexion PostGIS «%1» tiene un tramo vacio o invalido.").arg(text),
                  QStringLiteral("Formato: pg:conexion/esquema o %1").arg(sourceFormat()));
    }
  }

  source->connection = parts.at(0);
  source->schema = parts.at(1);
  if (parts.size() == 3)
    source->table = parts.at(2);
  return true;
}

QString escapeConninfo(const QString &value)
{
  // libpq admite comillas simples; dentro de ellas solo hay que proteger la
  // barra invertida y la propia comilla.
  QString escaped = value;
  escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
  return escaped;
}

QString buildConninfo(const PgSource &source, const PgConnection &connection,
                      const QString &password)
{
  QString conninfo = QStringLiteral("PG:host='%1' port=%2 dbname='%3' user='%4'")
                       .arg(escapeConninfo(connection.server), QString::number(connection.port),
                            escapeConninfo(connection.database), escapeConninfo(connection.user));

  // Sin contrasena la clave se omite a proposito: asi libpq puede usar
  // PGPASSWORD, PGPASSFILE o pgpass.conf, que es lo que hara la CLI.
  if (!password.isEmpty())
    conninfo += QStringLiteral(" password='%1'").arg(escapeConninfo(password));

  conninfo += QStringLiteral(" connect_timeout=10 sslmode=prefer");

  // La fuente de esquema no acota nada mas; la de tabla anade las dos claves.
  if (!source.schema.isEmpty())
    conninfo += QStringLiteral(" schemas='%1'").arg(escapeConninfo(source.schema));

  if (!source.table.isEmpty())
    conninfo += QStringLiteral(" tables='%1'").arg(escapeConninfo(source.table));

  return conninfo;
}

PgFailure classifyPgFailure(const QString &message)
{
  const QString text = message.toLower();

  if (text.contains(QStringLiteral("password authentication failed"))
      || text.contains(QStringLiteral("authentication failed"))
      || text.contains(QStringLiteral("no password supplied")))
    return PgFailure::Authentication;

  if (text.contains(QStringLiteral("could not connect"))
      || text.contains(QStringLiteral("connection refused"))
      || text.contains(QStringLiteral("timeout expired"))
      || text.contains(QStringLiteral("could not translate host name"))
      || text.contains(QStringLiteral("server closed the connection"))
      || text.contains(QStringLiteral("no route to host")))
    return PgFailure::Connection;

  if (text.contains(QStringLiteral("does not exist")) || text.contains(QStringLiteral("no such table"))
      || text.contains(QStringLiteral("undefined table")))
    return PgFailure::MissingObject;

  return PgFailure::Other;
}

QString scrubSecrets(const QString &text)
{
  static const QRegularExpression pattern(
    QStringLiteral("(?i)(password\\s*=\\s*)"
                   "('(?:[^'\\\\]|\\\\.)*'|\"(?:[^\"\\\\]|\\\\.)*\"|[^\\s;]+)"));

  QString result = text;
  result.replace(pattern, QStringLiteral("\\1***"));
  return result;
}

} // namespace minigis
