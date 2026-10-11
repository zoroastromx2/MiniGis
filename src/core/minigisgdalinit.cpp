#include "minigisgdalinit.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <gdal.h>
#include <gdal_version.h>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <ogr_srs_api.h>

namespace minigis
{
namespace
{

QString g_lastGdalError;

void CPL_STDCALL captureGdalError(CPLErr errorClass, int /*errorNo*/, const char *message)
{
  if (errorClass == CE_Debug || errorClass == CE_None || !message)
    return;

  const QString text = QString::fromUtf8(message).trimmed();
  if (!text.isEmpty())
    g_lastGdalError = text;
}

/** Directorios candidatos junto al ejecutable (arbol de compilacion o despliegue). */
QStringList appRelativeCandidates(const QString &suffix)
{
  QStringList candidates;
  if (!QCoreApplication::instance())
    return candidates;

  const QString appDir = QCoreApplication::applicationDirPath();
  candidates << appDir + QLatin1Char('/') + suffix
             << appDir + QStringLiteral("/../") + suffix
             << appDir + QStringLiteral("/../../") + suffix;
  return candidates;
}

QStringList environmentCandidates(const char *variableName)
{
  QStringList candidates;
  const QByteArray value = qgetenv(variableName);
  if (!value.isEmpty())
    candidates << QString::fromLocal8Bit(value);
  return candidates;
}

/** Primer directorio candidato que contiene `probeFileName`. */
QString findDataDir(const QString &probeFileName, const QStringList &candidates)
{
  for (const QString &candidate : candidates)
  {
    if (candidate.isEmpty())
      continue;
    const QString clean = QDir::cleanPath(candidate);
    if (QFileInfo::exists(clean + QLatin1Char('/') + probeFileName))
      return clean;
  }
  return {};
}

} // namespace

QString RuntimeInfo::summary() const
{
  QStringList lines;
  lines << QStringLiteral("GDAL      : %1").arg(gdalVersion.isEmpty() ? QStringLiteral("desconocido") : gdalVersion);
  lines << QStringLiteral("PROJ      : %1").arg(projVersion.isEmpty() ? QStringLiteral("desconocido") : projVersion);
  lines << QStringLiteral("PROJ_DATA : %1")
               .arg(projDataFound ? QDir::toNativeSeparators(projDataDir)
                                  : QStringLiteral("NO ENCONTRADO (reproyeccion limitada)"));
  lines << QStringLiteral("GDAL_DATA : %1")
               .arg(gdalDataFound ? QDir::toNativeSeparators(gdalDataDir)
                                  : QStringLiteral("no encontrado (solo afecta a algunos drivers)"));
  for (const QString &note : notes)
    lines << QStringLiteral("Aviso     : %1").arg(note);
  return lines.join(QLatin1Char('\n'));
}

RuntimeInfo initializeGdal()
{
  RuntimeInfo info;

  static bool configured = false;
  if (!configured)
  {
    configured = true;

    CPLSetErrorHandler(captureGdalError);

    // --- Directorio de datos de PROJ (proj.db) ---------------------------
    QStringList projCandidates;
    projCandidates << environmentCandidates("PROJ_DATA") << environmentCandidates("PROJ_LIB");
    projCandidates << appRelativeCandidates(QStringLiteral("proj"))
                   << appRelativeCandidates(QStringLiteral("share/proj"));
#ifdef MINIGIS_VCPKG_SHARE_DIR
    projCandidates << QStringLiteral(MINIGIS_VCPKG_SHARE_DIR "/proj");
#endif

    const QString projDir = findDataDir(QStringLiteral("proj.db"), projCandidates);
    if (!projDir.isEmpty())
    {
      info.projDataDir = projDir;
      info.projDataFound = true;

      const QByteArray projDirUtf8 = projDir.toUtf8();
      qputenv("PROJ_DATA", projDirUtf8);
      const char *paths[2] = {projDirUtf8.constData(), nullptr};
      OSRSetPROJSearchPaths(paths);
    }
    else
    {
      info.notes << QStringLiteral(
        "No se encontro proj.db; las reproyecciones entre SRC pueden fallar. "
        "Define la variable de entorno PROJ_DATA apuntando a la carpeta que lo contiene.");
    }

    // --- Directorio de datos de GDAL -------------------------------------
    QStringList gdalCandidates;
    gdalCandidates << environmentCandidates("GDAL_DATA");
    gdalCandidates << appRelativeCandidates(QStringLiteral("gdal"))
                   << appRelativeCandidates(QStringLiteral("share/gdal"));
#ifdef MINIGIS_VCPKG_SHARE_DIR
    gdalCandidates << QStringLiteral(MINIGIS_VCPKG_SHARE_DIR "/gdal");
#endif

    const QString gdalDir = findDataDir(QStringLiteral("s57attributes.csv"), gdalCandidates);
    if (!gdalDir.isEmpty())
    {
      info.gdalDataDir = gdalDir;
      info.gdalDataFound = true;
      CPLSetConfigOption("GDAL_DATA", gdalDir.toUtf8().constData());
    }

    GDALAllRegister();
  }

  info.gdalVersion = QString::fromUtf8(GDALVersionInfo("RELEASE_NAME"));

#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM >= 3000300
  int major = 0;
  int minor = 0;
  int patch = 0;
  OSRGetPROJVersion(&major, &minor, &patch);
  if (major > 0)
    info.projVersion = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
#endif

  return info;
}

QString lastGdalError()
{
  return g_lastGdalError;
}

void clearLastGdalError()
{
  g_lastGdalError.clear();
}

QStringList vectorDriverNames()
{
  QStringList names;
  const int driverCount = GDALGetDriverCount();
  for (int i = 0; i < driverCount; ++i)
  {
    GDALDriverH driver = GDALGetDriver(i);
    if (!driver)
      continue;
    if (GDALGetMetadataItem(driver, GDAL_DCAP_VECTOR, nullptr))
      names << QString::fromUtf8(GDALGetDescription(driver));
  }
  names.sort(Qt::CaseInsensitive);
  return names;
}

} // namespace minigis
