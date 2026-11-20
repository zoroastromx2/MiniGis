#include "minigispgsession.h"

namespace minigis
{

PgSession &PgSession::instance()
{
  static PgSession session;
  return session;
}

bool PgSession::isActive() const
{
  QMutexLocker locker(&mMutex);
  return mConnection.isValid();
}

PgConnection PgSession::connection() const
{
  QMutexLocker locker(&mMutex);
  return mConnection;
}

QString PgSession::password() const
{
  QMutexLocker locker(&mMutex);
  return mPassword;
}

QString PgSession::schema() const
{
  QMutexLocker locker(&mMutex);
  return mSchema;
}

void PgSession::setSchema(const QString &schema)
{
  QMutexLocker locker(&mMutex);
  mSchema = schema;
}

void PgSession::activate(const PgConnection &connection, const QString &password)
{
  QMutexLocker locker(&mMutex);
  mConnection = connection;
  mPassword = password;
}

void PgSession::clear()
{
  QMutexLocker locker(&mMutex);
  mConnection = PgConnection();
  mPassword.clear();
  mSchema.clear();
}

} // namespace minigis
