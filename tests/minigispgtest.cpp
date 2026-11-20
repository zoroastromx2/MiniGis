/**
 * Pruebas de las piezas PostGIS que no necesitan servidor: URI pg:, catalogo
 * Connections.xml, cadena de conexion y saneado de la contrasena.
 *
 * El objetivo es que la parte delicada (donde viaja la contrasena y como se
 * nombra una tabla) quede clavada sin depender del bdware corporativo.
 *
 * Uso: minigispgtest <Connections.xml>
 */

#include "minigisconnections.h"
#include "minigispgcatalog.h"
#include "minigispgcredentials.h"
#include "minigispgsession.h"
#include "minigispgsource.h"
#include "minigisvectorlayer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>

using namespace minigis;

namespace
{

int gFailures = 0;
int gChecks = 0;

void check(bool condition, const QString &what)
{
  ++gChecks;
  if (condition)
    return;
  ++gFailures;
  std::fprintf(stderr, "[FALLO] %s\n", what.toUtf8().constData());
}

void checkEqual(const QString &actual, const QString &expected, const QString &what)
{
  check(actual == expected,
        QStringLiteral("%1 (se obtuvo «%2», se esperaba «%3»)").arg(what, actual, expected));
}

} // namespace

int main(int argc, char *argv[])
{
  QCoreApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("MiniGIS"));
  QCoreApplication::setApplicationName(QStringLiteral("MiniGIS"));
  // Las pruebas no deben tocar las credenciales reales del usuario.
  QStandardPaths::setTestModeEnabled(true);

  if (argc < 2)
  {
    std::fprintf(stderr, "Uso: minigispgtest <Connections.xml>\n");
    return 1;
  }
  const QString fixture = QString::fromLocal8Bit(argv[1]);

  // --- URI pg: ----------------------------------------------------------
  check(PgSource::looksLike(QStringLiteral("pg:a/b")), "looksLike reconoce pg:");
  check(PgSource::looksLike(QStringLiteral("PG:a/b")), "looksLike no distingue mayusculas");
  check(!PgSource::looksLike(QStringLiteral("D:\\datos\\capa.shp")), "looksLike rechaza una ruta");
  check(!PgSource::looksLike(QStringLiteral("file:///capa.shp")), "looksLike rechaza file:");

  PgSource source;
  Error error;
  check(PgSource::parse(QStringLiteral("pg:dataware_FDAC/mgn_2025/poligonos_de_manzana"), &source,
                        &error),
        "parse acepta conexion/esquema/tabla");
  checkEqual(source.connection, QStringLiteral("dataware_FDAC"), "parse conserva mayusculas");
  checkEqual(source.schema, QStringLiteral("mgn_2025"), "parse lee el esquema");
  checkEqual(source.table, QStringLiteral("poligonos_de_manzana"), "parse lee la tabla");
  checkEqual(source.uri(), QStringLiteral("pg:dataware_FDAC/mgn_2025/poligonos_de_manzana"),
             "uri reconstruye la entrada");

  PgSource schemaOnly;
  check(PgSource::parse(QStringLiteral("pg:dataware_FDAC/mgn_2025"), &schemaOnly, &error),
        "parse acepta conexion/esquema");
  check(!schemaOnly.hasTable(), "la fuente de esquema no tiene tabla");
  checkEqual(schemaOnly.uri(), QStringLiteral("pg:dataware_FDAC/mgn_2025"), "uri del esquema");

  check(!PgSource::parse(QStringLiteral("pg:solo_conexion"), &source, &error),
        "parse rechaza un solo segmento");
  check(!error.message.isEmpty() && !error.hint.isEmpty(), "el error de parse tiene causa y accion");
  check(!PgSource::parse(QStringLiteral("pg:a/b/c/d"), &source, &error),
        "parse rechaza cuatro segmentos");
  check(!PgSource::parse(QStringLiteral("pg:a//c"), &source, &error),
        "parse rechaza un segmento vacio");
  check(!PgSource::parse(QStringLiteral("pg:a/../c"), &source, &error), "parse rechaza ..");

  // --- cadena de conexion -----------------------------------------------
  PgConnection connection;
  connection.name = QStringLiteral("dataware_FDAC");
  connection.server = QStringLiteral("10.153.3.32");
  connection.port = 5433;
  connection.database = QStringLiteral("bdginegi");
  connection.user = QStringLiteral("datawarehouse");

  PgSource table;
  PgSource::parse(QStringLiteral("pg:dataware_FDAC/mgn_2025/poligonos_de_manzana"), &table, &error);
  const QString conninfo = buildConninfo(table, connection, QStringLiteral("secreto"));
  check(conninfo.contains(QStringLiteral("host='10.153.3.32'")), "conninfo lleva el servidor");
  check(conninfo.contains(QStringLiteral("port=5433")), "conninfo lleva el puerto");
  check(conninfo.contains(QStringLiteral("dbname='bdginegi'")), "conninfo lleva la base");
  check(conninfo.contains(QStringLiteral("user='datawarehouse'")), "conninfo lleva el usuario");
  check(conninfo.contains(QStringLiteral("password='secreto'")), "conninfo lleva la contrasena");
  check(conninfo.contains(QStringLiteral("schemas='mgn_2025'")), "conninfo acota el esquema");
  check(conninfo.contains(QStringLiteral("tables='poligonos_de_manzana'")),
        "conninfo acota la tabla");

  const QString withoutPassword = buildConninfo(table, connection, QString());
  check(!withoutPassword.contains(QStringLiteral("password=")),
        "sin contrasena no se inventa la clave");
  check(!buildConninfo(schemaOnly, connection, QString()).contains(QStringLiteral("tables=")),
        "la fuente de esquema no acota tabla");

  PgSource allSchemas;
  allSchemas.connection = QStringLiteral("c");
  check(!buildConninfo(allSchemas, connection, QString()).contains(QStringLiteral("schemas=")),
        "sin esquema no se acota el catalogo");

  const QString escaped = buildConninfo(table, connection, QStringLiteral("pa's\\word"));
  check(escaped.contains(QStringLiteral("password='pa\\'s\\\\word'")),
        "la contrasena se escapa para libpq");

  // --- saneado -----------------------------------------------------------
  checkEqual(scrubSecrets(QStringLiteral("password=abc")), QStringLiteral("password=***"),
             "scrubSecrets tapa un valor sin comillas");
  checkEqual(scrubSecrets(QStringLiteral("Password = 'a b'")), QStringLiteral("Password = ***"),
             "scrubSecrets tapa un valor entre comillas y conserva el resto");
  checkEqual(scrubSecrets(QStringLiteral("PG:host=x password=\"y\" port=5432")),
             QStringLiteral("PG:host=x password=*** port=5432"),
             "scrubSecrets no toca el resto de la cadena");
  checkEqual(scrubSecrets(QStringLiteral("sin secretos")), QStringLiteral("sin secretos"),
             "scrubSecrets deja intacto un texto normal");

  // --- catalogo Connections.xml -----------------------------------------
  PgConnectionCatalog catalog;
  check(PgConnectionCatalog::load(fixture, &catalog, &error), "se carga el fixture de conexiones");
  check(catalog.connections().size() == 2, "el fixture tiene dos conexiones PGIS");
  check(catalog.names().contains(QStringLiteral("prueba_pg")), "aparece prueba_pg");
  check(!catalog.find(QStringLiteral("capa_gpkg")), "la entrada GPKG no se lista");

  const PgConnection *pg = catalog.find(QStringLiteral("prueba_pg"));
  check(pg != nullptr, "se encuentra prueba_pg");
  if (pg)
  {
    checkEqual(pg->server, QStringLiteral("127.0.0.1"), "servidor de prueba_pg");
    check(pg->port == 5433, "puerto de prueba_pg");
    checkEqual(pg->database, QStringLiteral("bdprueba"), "base de prueba_pg");
    checkEqual(pg->user, QStringLiteral("usuario_prueba"), "usuario de prueba_pg");
    checkEqual(pg->fingerprint(), QStringLiteral("prueba_pg|127.0.0.1|5433|usuario_prueba"),
               "fingerprint estable");
    check(!pg->displayName().contains(QStringLiteral("cifrada")), "displayName no lleva secretos");
    check(catalog.warnings().isEmpty(), "el fixture no genera avisos");
  }

  Error missing;
  PgConnectionCatalog absent;
  check(!PgConnectionCatalog::load(QStringLiteral("C:/no/existe/Connections.xml"), &absent,
                                   &missing),
        "un catalogo inexistente falla");
  check(!missing.message.isEmpty() && !missing.hint.isEmpty(),
        "el fallo del catalogo es accionable");

  // --- credenciales ------------------------------------------------------
  PgConnection credentialConnection;
  credentialConnection.name = QStringLiteral("prueba_cred");
  credentialConnection.server = QStringLiteral("127.0.0.1");
  credentialConnection.port = 5432;
  credentialConnection.database = QStringLiteral("bd");
  credentialConnection.user = QStringLiteral("u");

  PgCredentials &credentials = PgCredentials::instance();
  credentials.clearSession();
  credentials.forget(credentialConnection);
  check(credentials.passwordFor(credentialConnection).isEmpty(),
        "sin recordar no hay contrasena");

  Error credentialError;
  check(credentials.remember(credentialConnection, QStringLiteral("s3creto"), false,
                             &credentialError),
        "remember sin persistir no falla");
  checkEqual(credentials.passwordFor(credentialConnection), QStringLiteral("s3creto"),
             "la sesion conserva la contrasena");

  credentials.clearSession();
  check(credentials.passwordFor(credentialConnection).isEmpty(),
        "sin persistir, la contrasena no sobrevive a la sesion");

#ifdef Q_OS_WIN
  check(credentials.remember(credentialConnection, QStringLiteral("s3creto"), true,
                             &credentialError),
        "remember con DPAPI no falla");
  credentials.clearSession();
  checkEqual(credentials.passwordFor(credentialConnection), QStringLiteral("s3creto"),
             "DPAPI recupera la contrasena tras limpiar la sesion");

  QFile store(PgCredentials::storePath());
  check(store.open(QIODevice::ReadOnly), "se puede leer el almacen de credenciales");
  const QByteArray stored = store.readAll();
  store.close();
  check(!stored.contains("s3creto"), "el almacen no guarda la contrasena en claro");

  credentials.forget(credentialConnection);
  credentials.clearSession();
  check(credentials.passwordFor(credentialConnection).isEmpty(), "forget borra la credencial");
#endif

  // --- sesion -------------------------------------------------------------
  PgSession &session = PgSession::instance();
  session.clear();
  check(!session.isActive(), "la sesion empieza inactiva");

  PgConnection sessionConnection;
  sessionConnection.name = QStringLiteral("prueba_sesion");
  sessionConnection.server = QStringLiteral("127.0.0.1");
  sessionConnection.port = 5432;
  sessionConnection.database = QStringLiteral("bd");
  sessionConnection.user = QStringLiteral("u");

  session.activate(sessionConnection, QStringLiteral("clave"));
  check(session.isActive(), "la sesion queda activa");
  checkEqual(session.connection().name, QStringLiteral("prueba_sesion"),
             "la sesion conserva la conexion");
  checkEqual(session.password(), QStringLiteral("clave"), "la sesion conserva la contrasena");
  session.setSchema(QStringLiteral("mgn_2025"));
  checkEqual(session.schema(), QStringLiteral("mgn_2025"), "la sesion conserva el esquema");

  session.clear();
  check(!session.isActive() && session.password().isEmpty() && session.schema().isEmpty(),
        "clear olvida la sesion");

  // --- codificacion ISO-8859-1 -------------------------------------------
  QTemporaryDir temporary;
  check(temporary.isValid(), "hay carpeta temporal");
  if (temporary.isValid())
  {
    QFile latin(temporary.filePath(QStringLiteral("Connections.xml")));
    const QString text = QStringLiteral(
      "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><servers>"
      "<ConStr NAME=\"Conexión\" TYPE=\"PGIS\" SERVER=\"127.0.0.1\" PORT=\"5433\" "
      "DATABASE=\"bd\" USER=\"u\"/></servers>");
    check(latin.open(QIODevice::WriteOnly), "se crea el XML en ISO-8859-1");
    check(latin.write(text.toLatin1()) > 0, "se escribe el XML en ISO-8859-1");
    latin.close();

    PgConnectionCatalog latinCatalog;
    check(PgConnectionCatalog::load(latin.fileName(), &latinCatalog, &error), "se lee ISO-8859-1");
    checkEqual(latinCatalog.names().value(0), QStringLiteral("Conexión"),
               "el nombre acentuado se decodifica bien");
  }

  // --- ruta del catalogo -------------------------------------------------
  checkEqual(PgConnectionCatalog::resolvePath(QStringLiteral("C:/explicito.xml")),
             QStringLiteral("C:/explicito.xml"), "la ruta explicita manda");
  qputenv("MINIGIS_CONNECTIONS", QByteArray("C:/entorno.xml"));
  checkEqual(PgConnectionCatalog::resolvePath(), QStringLiteral("C:/entorno.xml"),
             "la variable de entorno se usa si no hay ruta explicita");
  qunsetenv("MINIGIS_CONNECTIONS");

  // --- catalogo real (opt-in) --------------------------------------------
  // Con tres argumentos mas se comprueba contra un servidor; la contrasena la
  // toma libpq de PGPASSWORD, que es el mismo camino que usara la CLI.
  if (argc >= 5)
  {
    const QString realCatalogPath = QString::fromLocal8Bit(argv[2]);
    const QString connectionName = QString::fromLocal8Bit(argv[3]);
    const QString schemaName = QString::fromLocal8Bit(argv[4]);

    PgConnectionCatalog realCatalog;
    check(PgConnectionCatalog::load(realCatalogPath, &realCatalog, &error),
          "se carga el catalogo real");
    // Es lo que hace la CLI al arrancar con --connections: fija el catalogo
    // compartido del que tira VectorLayer al abrir cada capa.
    PgConnectionCatalog::setExplicitPath(realCatalogPath);

    const PgConnection *realConnection = realCatalog.find(connectionName);
    check(realConnection != nullptr, "la conexion pedida existe en el catalogo");
    if (realConnection)
    {
      PgFailure failure = PgFailure::None;
      Error catalogError;

      QStringList schemas;
      check(PgCatalog::listSchemas(*realConnection, QString(), &schemas, &failure, &catalogError),
            QStringLiteral("se listan los esquemas: %1").arg(catalogError.toText()));
      check(schemas.contains(schemaName), "el esquema pedido aparece en el catalogo");

      QVector<PgTableInfo> tables;
      check(PgCatalog::listTables(*realConnection, QString(), schemaName, &tables, &failure,
                                  &catalogError),
            QStringLiteral("se listan las tablas: %1").arg(catalogError.toText()));
      check(!tables.isEmpty(), "el esquema tiene tablas con geometria");

      for (const PgTableInfo &tableInfo : tables)
      {
        std::printf("[pg] %-55s %-14s %10lld  EPSG:%d\n", tableInfo.name.toUtf8().constData(),
                    tableInfo.geometryName.toUtf8().constData(),
                    static_cast<long long>(tableInfo.features), tableInfo.srid);
      }

      // La segunda lectura debe reutilizar la conexion de catalogo que se
      // conserva abierta: no vuelve a conectar ni a autenticarse.
      QElapsedTimer reuseTimer;
      reuseTimer.start();
      QVector<PgTableInfo> reused;
      check(PgCatalog::listTables(*realConnection, QString(), schemaName, &reused, &failure,
                                  &catalogError),
            QStringLiteral("la segunda lectura reutiliza la conexion viva: %1")
              .arg(catalogError.toText()));
      check(reused.size() == tables.size(), "la conexion viva devuelve las mismas tablas");
      std::printf("[pg] segunda lectura con la conexion viva: %lld ms\n",
                  static_cast<long long>(reuseTimer.elapsed()));

      // La capa se abre con el mismo VectorLayer que un archivo: mismo motor,
      // misma URI saneada y mismo grupo en el panel.
      QString sample = QStringLiteral("areas_geoestadisticas_estatales");
      bool sampleExists = false;
      for (const PgTableInfo &tableInfo : tables)
        sampleExists = sampleExists || tableInfo.name == sample;
      if (!sampleExists && !tables.isEmpty())
        sample = tables.first().name;

      PgSource layerSource;
      layerSource.connection = connectionName;
      layerSource.schema = schemaName;
      layerSource.table = sample;

      Error layerError;
      PgFailure layerFailure = PgFailure::None;
      const std::shared_ptr<VectorLayer> layer =
        VectorLayer::open(layerSource.uri(), 0, &layerError, &layerFailure);
      check(layer != nullptr,
            QStringLiteral("se abre la capa %1: %2").arg(sample, layerError.toText()));
      if (layer)
      {
        check(layer->isPostgis(), "la capa se marca como PostGIS");
        checkEqual(layer->sourcePath(), layerSource.uri(), "sourcePath es la URI saneada");
        checkEqual(layer->containerName(), connectionName + QStringLiteral("/") + schemaName,
                   "el grupo es conexion/esquema");
        check(layer->driverName() == QStringLiteral("PostgreSQL"), "el driver es PostgreSQL");
        if (sample == QStringLiteral("areas_geoestadisticas_estatales"))
          check(layer->featureCount() == 32, "la tabla de prueba tiene 32 elementos");
      }
    }
  }

  std::printf("[pg] %d comprobaciones, %d fallos\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
