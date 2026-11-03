#include "minigisziparchive.h"

#include <QFile>

#include <limits>

#include <unzip.h>
#include <zip.h>

namespace minigis
{

namespace
{

/** Ninguna entrada de un proyecto deberia acercarse a este tamano. */
constexpr qint64 kMaximumEntryBytes = 256LL * 1024LL * 1024LL;

Error zipReadError(const QString &path, const QString &detail)
{
  return Error {QStringLiteral("No se pudo leer el ZIP «%1»: %2").arg(path, detail),
                QStringLiteral("Comprueba que el .qgz no este danado; si QGIS lo abre, vuelve a guardarlo desde QGIS.")};
}

Error zipWriteError(const QString &path, const QString &detail)
{
  return Error {QStringLiteral("No se pudo escribir el ZIP «%1»: %2").arg(path, detail),
                QStringLiteral("Comprueba que la carpeta de destino exista y tenga permisos de escritura.")};
}

} // namespace

bool ZipArchive::read(const QString &path, QVector<ZipEntry> *entries, Error *error)
{
  if (!entries)
    return false;
  entries->clear();

  const QByteArray nativePath = QFile::encodeName(path);
  unzFile archive = unzOpen64(nativePath.constData());
  if (!archive)
  {
    if (error)
      *error = zipReadError(path, QStringLiteral("no es un archivo ZIP o no se puede abrir"));
    return false;
  }

  const auto fail = [&](const QString &detail) {
    unzClose(archive);
    if (error)
      *error = zipReadError(path, detail);
    return false;
  };

  int result = unzGoToFirstFile(archive);
  while (result == UNZ_OK)
  {
    unz_file_info64 info {};
    char nameBuffer[1024] = {0};
    if (unzGetCurrentFileInfo64(archive, &info, nameBuffer, sizeof(nameBuffer) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
      return fail(QStringLiteral("una entrada no tiene cabecera valida"));

    const QString entryName = QString::fromUtf8(nameBuffer);
    if (!entryName.endsWith(QLatin1Char('/')))
    {
      if (static_cast<qint64>(info.uncompressed_size) > kMaximumEntryBytes)
        return fail(QStringLiteral("la entrada «%1» es demasiado grande").arg(entryName));

      if (unzOpenCurrentFile(archive) != UNZ_OK)
        return fail(QStringLiteral("no se pudo abrir la entrada «%1»").arg(entryName));

      QByteArray data;
      data.reserve(static_cast<int>(qMin<qint64>(info.uncompressed_size, 1024 * 1024)));
      char buffer[64 * 1024];
      int read = 0;
      while ((read = unzReadCurrentFile(archive, buffer, sizeof(buffer))) > 0)
        data.append(buffer, read);

      const int closeResult = unzCloseCurrentFile(archive);
      if (read < 0)
        return fail(QStringLiteral("error al descomprimir la entrada «%1»").arg(entryName));
      if (closeResult != UNZ_OK)
        return fail(QStringLiteral("la entrada «%1» no supera la comprobacion de integridad").arg(entryName));

      entries->append(ZipEntry {entryName, data});
    }

    result = unzGoToNextFile(archive);
  }

  if (result != UNZ_END_OF_LIST_OF_FILE)
  {
    unzClose(archive);
    if (error)
      *error = zipReadError(path, QStringLiteral("el directorio del ZIP esta danado"));
    return false;
  }

  unzClose(archive);
  return true;
}

bool ZipArchive::write(const QString &path, const QVector<ZipEntry> &entries, Error *error)
{
  const QByteArray nativePath = QFile::encodeName(path);
  zipFile archive = zipOpen64(nativePath.constData(), APPEND_STATUS_CREATE);
  if (!archive)
  {
    if (error)
      *error = zipWriteError(path, QStringLiteral("no se pudo crear el archivo"));
    return false;
  }

  const auto fail = [&](const QString &detail) {
    zipClose(archive, nullptr);
    QFile::remove(path);
    if (error)
      *error = zipWriteError(path, detail);
    return false;
  };

  for (const ZipEntry &entry : entries)
  {
    if (entry.isDirectory() || entry.name.isEmpty())
      continue;

    const QByteArray name = entry.name.toUtf8();
    zip_fileinfo info {};
    if (zipOpenNewFileInZip(archive, name.constData(), &info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED,
                            Z_DEFAULT_COMPRESSION) != ZIP_OK)
      return fail(QStringLiteral("no se pudo anadir la entrada «%1»").arg(entry.name));

    if (!entry.data.isEmpty()
        && zipWriteInFileInZip(archive, entry.data.constData(), static_cast<unsigned int>(entry.data.size())) != ZIP_OK)
      return fail(QStringLiteral("no se pudo escribir la entrada «%1»").arg(entry.name));

    if (zipCloseFileInZip(archive) != ZIP_OK)
      return fail(QStringLiteral("no se pudo cerrar la entrada «%1»").arg(entry.name));
  }

  if (zipClose(archive, nullptr) != ZIP_OK)
  {
    QFile::remove(path);
    if (error)
      *error = zipWriteError(path, QStringLiteral("no se pudo cerrar el archivo"));
    return false;
  }

  return true;
}

QString ZipArchive::projectEntryName(const QVector<ZipEntry> &entries)
{
  for (const ZipEntry &entry : entries)
  {
    if (!entry.isDirectory() && entry.name.endsWith(QStringLiteral(".qgs"), Qt::CaseInsensitive))
      return entry.name;
  }
  return QString();
}

} // namespace minigis
