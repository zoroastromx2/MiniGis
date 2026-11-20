#include "minigispgcredentials.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace minigis
{
namespace
{

#ifdef Q_OS_WIN

QByteArray protectForUser(const QByteArray &data)
{
  DATA_BLOB input;
  input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(data.constData()));
  input.cbData = static_cast<DWORD>(data.size());

  DATA_BLOB output {};
  if (!CryptProtectData(&input, L"MiniGIS PostGIS", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output))
    return {};

  const QByteArray result(reinterpret_cast<const char *>(output.pbData),
                          static_cast<int>(output.cbData));
  LocalFree(output.pbData);
  return result;
}

QByteArray unprotectForUser(const QByteArray &blob)
{
  DATA_BLOB input;
  input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(blob.constData()));
  input.cbData = static_cast<DWORD>(blob.size());

  DATA_BLOB output {};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &output))
    return {};

  const QByteArray result(reinterpret_cast<const char *>(output.pbData),
                          static_cast<int>(output.cbData));
  LocalFree(output.pbData);
  return result;
}

#else

// Sin DPAPI se degrada a no recordar: la contrasena vive solo en la sesion.
QByteArray protectForUser(const QByteArray &)
{
  return {};
}

QByteArray unprotectForUser(const QByteArray &)
{
  return {};
}

#endif

} // namespace

PgCredentials &PgCredentials::instance()
{
  static PgCredentials credentials;
  return credentials;
}

QString PgCredentials::storePath()
{
  const QString directory =
    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return directory + QStringLiteral("/postgis-credentials.json");
}

void PgCredentials::ensureLoaded()
{
  if (mLoaded)
    return;
  mLoaded = true;

  QFile file(storePath());
  if (!file.open(QIODevice::ReadOnly))
    return;

  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject())
    return;

  const QJsonObject credentials = document.object().value(QStringLiteral("credentials")).toObject();
  for (auto it = credentials.constBegin(); it != credentials.constEnd(); ++it)
    mPersistent.insert(it.key(), it.value().toString());
}

bool PgCredentials::save(Error *error)
{
  const auto fail = [error](const QString &message, const QString &hint) {
    if (error)
      *error = Error {message, hint};
    return false;
  };

  const QString path = storePath();
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return fail(QStringLiteral("No se pudo crear la carpeta de credenciales «%1».")
                  .arg(QFileInfo(path).absolutePath()),
                QStringLiteral("Comprueba los permisos del perfil de usuario."));

  QJsonObject credentials;
  for (auto it = mPersistent.constBegin(); it != mPersistent.constEnd(); ++it)
    credentials.insert(it.key(), it.value());

  QJsonObject root;
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("credentials"), credentials);

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return fail(QStringLiteral("No se pudo escribir «%1».").arg(path),
                QStringLiteral("Comprueba los permisos del perfil de usuario."));

  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  if (!file.commit())
    return fail(QStringLiteral("No se pudo guardar «%1».").arg(path),
                QStringLiteral("Comprueba los permisos del perfil de usuario."));

  return true;
}

QString PgCredentials::passwordFor(const PgConnection &connection)
{
  if (!connection.isValid())
    return {};

  const QString key = connection.fingerprint();
  QMutexLocker locker(&mMutex);

  const auto session = mSession.constFind(key);
  if (session != mSession.constEnd())
    return session.value();

  ensureLoaded();

  const auto stored = mPersistent.constFind(key);
  if (stored == mPersistent.constEnd())
    return {};

  const QByteArray password = unprotectForUser(QByteArray::fromBase64(stored.value().toLatin1()));
  if (password.isEmpty())
    return {};

  const QString text = QString::fromUtf8(password);
  mSession.insert(key, text);
  return text;
}

bool PgCredentials::remember(const PgConnection &connection, const QString &password, bool persist,
                             Error *error)
{
  if (!connection.isValid())
  {
    if (error)
      *error = Error {QStringLiteral("La conexion no tiene nombre, servidor o base."),
                      QStringLiteral("Revisa el Connections.xml o elige otra conexion.")};
    return false;
  }

  const QString key = connection.fingerprint();
  QMutexLocker locker(&mMutex);

  mSession.insert(key, password);
  ensureLoaded();

  if (!persist)
  {
    // Desmarcar «Recordar» tambien olvida la copia anterior.
    if (mPersistent.remove(key))
      return save(error);
    return true;
  }

  const QByteArray blob = protectForUser(password.toUtf8());
  if (blob.isEmpty())
  {
    if (error)
      *error = Error {QStringLiteral("Windows no pudo cifrar la contrasena de «%1».")
                        .arg(connection.name),
                      QStringLiteral("Se usara solo en esta sesion; no se ha guardado nada.")};
    return false;
  }

  mPersistent.insert(key, QString::fromLatin1(blob.toBase64()));
  return save(error);
}

void PgCredentials::forget(const PgConnection &connection)
{
  QMutexLocker locker(&mMutex);
  ensureLoaded();

  const QString key = connection.fingerprint();
  mSession.remove(key);
  if (mPersistent.remove(key))
    save(nullptr);
}

void PgCredentials::clearSession()
{
  QMutexLocker locker(&mMutex);
  mSession.clear();
}

} // namespace minigis
