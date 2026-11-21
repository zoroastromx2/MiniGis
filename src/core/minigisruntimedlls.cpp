#include "minigisruntimedlls.h"

#include "minigisvcpkgversions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QStringList>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <winver.h>
#endif

namespace minigis
{
namespace
{

#ifdef Q_OS_WIN

/** Version del recurso VS_VERSIONINFO del PE ("3.12.4"); vacia si no lo lleva. */
QString fileVersion(const QString &path)
{
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(reinterpret_cast<LPCWSTR>(path.utf16()), &handle);
  if (size == 0)
    return QString();

  QByteArray buffer;
  buffer.resize(static_cast<int>(size));
  if (!GetFileVersionInfoW(reinterpret_cast<LPCWSTR>(path.utf16()), 0, size, buffer.data()))
    return QString();

  VS_FIXEDFILEINFO *fixed = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(buffer.constData(), L"\\", reinterpret_cast<LPVOID *>(&fixed), &length)
      || fixed == nullptr || length == 0)
  {
    return QString();
  }

  const int major = HIWORD(fixed->dwFileVersionMS);
  const int minor = LOWORD(fixed->dwFileVersionMS);
  const int build = HIWORD(fixed->dwFileVersionLS);
  const int revision = LOWORD(fixed->dwFileVersionLS);
  if (major == 0 && minor == 0)
    return QString();

  // El cuarto numero solo se ensena cuando aporta algo: asi gdal.dll sale como
  // "3.12.4" y no como "3.12.4.0".
  QString version = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(build);
  if (revision != 0)
    version += QStringLiteral(".%1").arg(revision);
  return version;
}

/** Indice nombre de DLL -> puerto/version de vcpkg, generado al configurar. */
QHash<QString, VcpkgDllVersion> vcpkgVersionMap()
{
  QHash<QString, VcpkgDllVersion> map;
  for (const VcpkgDllVersion &entry : kVcpkgDllVersions)
  {
    if (entry.dll == nullptr || *entry.dll == '\0')
      continue;
    map.insert(QString::fromLatin1(entry.dll).toLower(), entry);
  }
  return map;
}

#endif // Q_OS_WIN

} // namespace

QList<RuntimeDllInfo> loadedRuntimeDlls()
{
  QList<RuntimeDllInfo> dlls;
#ifdef Q_OS_WIN
  const QString appDir = QDir::cleanPath(QCoreApplication::applicationDirPath());
  // QDir::cleanPath normaliza a "/" tambien en Windows: el prefijo se compara
  // en esa forma y solo al final se pasa la ruta relativa a separadores nativos.
  const QString prefix = appDir + QLatin1Char('/');
  const QHash<QString, VcpkgDllVersion> versions = vcpkgVersionMap();

  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE)
    return dlls;

  MODULEENTRY32W entry;
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry))
  {
    do
    {
      const QString modulePath = QDir::cleanPath(QString::fromWCharArray(entry.szExePath));
      if (!modulePath.startsWith(prefix, Qt::CaseInsensitive))
        continue;

      QString relative = modulePath.mid(prefix.size());
      relative.replace(QLatin1Char('/'), QLatin1Char('\\'));
      if (relative.isEmpty() || !relative.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive))
        continue;

      RuntimeDllInfo info;
      info.name = QString::fromWCharArray(entry.szModule);
      info.path = relative;
      info.version = fileVersion(modulePath);

      const auto known = versions.constFind(info.name.toLower());
      if (known != versions.constEnd())
      {
        info.component = QString::fromLatin1(known->port);
        // El recurso del PE manda: si alguien reemplaza la DLL por otra build,
        // el numero que se ve es el del archivo, no el del arbol de vcpkg.
        if (info.version.isEmpty())
          info.version = QString::fromLatin1(known->version);
      }

      dlls.append(info);
    } while (Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);

  // La raiz antes que las subcarpetas (plugins) y, dentro de cada grupo, por
  // nombre: es el orden en el que se buscan las DLLs al diagnosticar.
  std::sort(dlls.begin(), dlls.end(), [](const RuntimeDllInfo &left, const RuntimeDllInfo &right) {
    const bool leftNested = left.path.contains(QLatin1Char('\\'));
    const bool rightNested = right.path.contains(QLatin1Char('\\'));
    if (leftNested != rightNested)
      return !leftNested;
    return left.path.compare(right.path, Qt::CaseInsensitive) < 0;
  });
#endif // Q_OS_WIN
  return dlls;
}

QString loadedRuntimeDllVersion(const QString &name)
{
  const QString wanted = QFileInfo(name).fileName();
  for (const RuntimeDllInfo &dll : loadedRuntimeDlls())
  {
    if (dll.name.compare(wanted, Qt::CaseInsensitive) == 0)
      return dll.version;
  }
  return QString();
}

QString runtimeDllsText()
{
  QStringList lines;
  for (const RuntimeDllInfo &dll : loadedRuntimeDlls())
  {
    const QString version = dll.version.isEmpty() ? QStringLiteral("sin version") : dll.version;
    lines << QStringLiteral("%1  %2").arg(dll.path, version);
  }
  return lines.join(QLatin1Char('\n'));
}

} // namespace minigis
