#pragma once

#include <QString>

namespace minigis
{

/**
 * Error con causa y accion sugerida.
 *
 * Regla de MiniGIS: un error sobre el que el usuario no puede actuar es un
 * defecto. Por eso todo fallo rellena `message` (que ha pasado) y `hint`
 * (que hacer a continuacion).
 */
struct Error
{
  QString message;
  QString hint;

  bool isEmpty() const { return message.isEmpty(); }

  QString toText() const
  {
    if (message.isEmpty())
      return {};
    if (hint.isEmpty())
      return message;
    return message + QLatin1Char('\n') + QStringLiteral("Sugerencia: ") + hint;
  }
};

} // namespace minigis
