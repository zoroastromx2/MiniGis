#include "minigispgcheck.h"

#include "minigisconnections.h"
#include "minigispgcredentials.h"

#include <QElapsedTimer>
#include <QJsonArray>

#include <gdal.h>

namespace minigis
{

PgCheckReport checkPgSchema(const PgSource &source)
{
  QElapsedTimer timer;
  timer.start();

  PgCheckReport report;
  report.source = source;

  Error error;
  const PgConnectionCatalog *catalog = PgConnectionCatalog::shared(&error);
  if (!catalog)
  {
    report.failure = PgFailure::Catalog;
    report.errorMessage = error.message;
    report.errorHint = error.hint;
    report.elapsedMs = timer.elapsed();
    return report;
  }

  const PgConnection *connection = catalog->find(source.connection);
  if (!connection)
  {
    report.failure = PgFailure::UnknownConnection;
    report.errorMessage = QStringLiteral("No existe la conexion «%1».").arg(source.connection);
    report.errorHint = QStringLiteral("Disponibles: %1")
                         .arg(catalog->names().isEmpty()
                                ? QStringLiteral("(ninguna)")
                                : catalog->names().join(QStringLiteral(", ")));
    report.elapsedMs = timer.elapsed();
    return report;
  }

  report.connection = *connection;
  const QString password = PgCredentials::instance().passwordFor(*connection);

  PgFailure failure = PgFailure::None;
  if (!PgCatalog::listTables(*connection, password, source.schema, &report.tables, &failure, &error))
  {
    report.failure = failure;
    report.errorMessage = error.message;
    report.errorHint = error.hint;
    report.elapsedMs = timer.elapsed();
    return report;
  }

  // `listTables` ya abre cada capa con GDAL: una tabla que no se puede describir
  // no aparece en el catalogo. Reabrir cada una con su propia conexion costaba
  // ~2 s con 16 tablas y no anadia informacion.
  report.ok = !report.tables.isEmpty();
  if (!report.ok)
  {
    report.errorMessage = QStringLiteral("El esquema «%1» no tiene tablas con geometria.")
                            .arg(source.schema);
    report.errorHint = QStringLiteral("Elige otro esquema; --check-pg solo lista tablas "
                                      "registradas en geometry_columns.");
  }

  report.elapsedMs = timer.elapsed();
  return report;
}

QJsonObject PgCheckReport::toJson() const
{
  QJsonObject root;
  root.insert(QStringLiteral("ok"), ok);

  QJsonObject connectionObject;
  connectionObject.insert(QStringLiteral("name"),
                          connection.name.isEmpty() ? source.connection : connection.name);
  connectionObject.insert(QStringLiteral("server"), connection.server);
  connectionObject.insert(QStringLiteral("port"), connection.port);
  connectionObject.insert(QStringLiteral("database"), connection.database);
  connectionObject.insert(QStringLiteral("user"), connection.user);
  root.insert(QStringLiteral("connection"), connectionObject);
  root.insert(QStringLiteral("schema"), source.schema);

  QJsonArray layers;
  for (const PgTableInfo &table : tables)
  {
    QJsonObject object;
    object.insert(QStringLiteral("name"), table.name);
    object.insert(QStringLiteral("geometry"), table.geometryName);
    object.insert(QStringLiteral("features"), static_cast<double>(table.features));
    object.insert(QStringLiteral("srid"), table.srid);
    object.insert(QStringLiteral("openOk"), table.openOk);
    layers.append(object);
  }
  root.insert(QStringLiteral("layers"), layers);

  if (!errorMessage.isEmpty() || !errorHint.isEmpty())
  {
    QJsonObject error;
    error.insert(QStringLiteral("message"), scrubSecrets(errorMessage));
    error.insert(QStringLiteral("hint"), scrubSecrets(errorHint));
    root.insert(QStringLiteral("error"), error);
  }

  root.insert(QStringLiteral("elapsedMs"), static_cast<double>(elapsedMs));
  return root;
}

QString PgCheckReport::toText() const
{
  QStringList lines;

  if (!connection.isValid())
  {
    lines << QStringLiteral("Conexión    : %1").arg(source.connection);
    lines << QStringLiteral("Esquema     : %1").arg(source.schema);
    lines << QStringLiteral("Resultado   : FALLO — %1").arg(errorMessage);
    if (!errorHint.isEmpty())
      lines << QStringLiteral("Sugerencia  : %1").arg(errorHint);
    return lines.join(QLatin1Char('\n'));
  }

  lines << QStringLiteral("Conexión    : %1 (%2:%3/%4, %5)")
             .arg(connection.name, connection.server, QString::number(connection.port),
                  connection.database, connection.user);
  lines << QStringLiteral("Esquema     : %1").arg(source.schema);
  lines << QStringLiteral("Tablas      : %1").arg(tables.size());

  for (const PgTableInfo &table : tables)
  {
    lines << QStringLiteral("  %1  %2  %3 elementos  EPSG:%4  %5")
               .arg(table.name, table.geometryName)
               .arg(table.features)
               .arg(table.srid)
               .arg(table.openOk ? QStringLiteral("ok") : QStringLiteral("NO ABRE"));
  }

  if (ok)
  {
    lines << QStringLiteral("Resultado   : OK en %1 ms").arg(elapsedMs);
  }
  else
  {
    lines << QStringLiteral("Resultado   : FALLO — %1")
               .arg(errorMessage.isEmpty() ? QStringLiteral("ninguna tabla se pudo abrir")
                                           : errorMessage);
    if (!errorHint.isEmpty())
      lines << QStringLiteral("Sugerencia  : %1").arg(errorHint);
    lines << QStringLiteral("Tiempo      : %1 ms").arg(elapsedMs);
  }

  return lines.join(QLatin1Char('\n'));
}

} // namespace minigis
