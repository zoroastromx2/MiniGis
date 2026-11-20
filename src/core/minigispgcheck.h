#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include "minigispgcatalog.h"

namespace minigis
{

/**
 * Resultado de `--check-pg`: lo que hay en un esquema y si cada tabla abre.
 *
 * Existe para que el inventario de tablas sea verificable sin ventana. `ok` es
 * false si no se pudo conectar, si el esquema esta vacio o si alguna tabla no
 * abre; `failure` distingue el motivo para elegir el codigo de salida.
 */
struct PgCheckReport
{
  bool ok = false;
  PgSource source;
  PgConnection connection;
  QVector<PgTableInfo> tables;
  PgFailure failure = PgFailure::None;
  QString errorMessage;
  QString errorHint;
  qint64 elapsedMs = 0;

  QJsonObject toJson() const;
  QString toText() const;
};

/** Diagnostico de un esquema: lista tablas y abre cada una por separado. */
PgCheckReport checkPgSchema(const PgSource &source);

} // namespace minigis
