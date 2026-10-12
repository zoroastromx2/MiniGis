#include "minigisshapefileindex.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cstring>

namespace minigis
{
namespace
{

/** Codigo de archivo que debe aparecer en la cabecera de todo .shp ("9994"). */
constexpr int kFileCode = 9994;

/** Cabecera del archivo: 100 bytes. */
constexpr qint64 kHeaderSize = 100;

/** Cabecera de registro: numero (4) + longitud en palabras de 16 bits (4). */
constexpr qint64 kRecordHeaderSize = 8;

/** Tipo de forma (4) + envolvente (4 dobles). */
constexpr qint64 kEnvelopeSize = 36;

/** Tipos de forma con envolvente conocida (Point, PolyLine, Polygon y sus variantes Z/M). */
bool hasEnvelope(int shapeType)
{
  switch (shapeType)
  {
    case 0:  // Null shape: sin envolvente, pero ocupa un registro (y un FID)
    case 1:
    case 3:
    case 5:
    case 8:
    case 11:
    case 13:
    case 15:
    case 18:
    case 21:
    case 23:
    case 25:
    case 28:
    case 31: // MultiPatch: la envolvente esta en el mismo sitio
      return true;
    default:
      return false;
  }
}

/** Los tipos 1/11/21 (punto) guardan la coordenada donde iria la envolvente. */
bool isPointType(int shapeType)
{
  return shapeType == 1 || shapeType == 11 || shapeType == 21;
}

qint32 readBigEndianInt32(const uchar *data)
{
  return static_cast<qint32>((static_cast<quint32>(data[0]) << 24) | (static_cast<quint32>(data[1]) << 16)
                             | (static_cast<quint32>(data[2]) << 8) | static_cast<quint32>(data[3]));
}

/**
 * Entero de 32 bits en orden little-endian.
 *
 * En el .shp conviven los dos ordenes: el codigo de archivo y la longitud de la
 * cabecera van en big-endian (como en el .shx), pero la version, el tipo de forma
 * y todo el contenido de los registros van en little-endian. Leer el tipo de
 * forma en el orden equivocado da 0x05000000 en lugar de 5, que fue justo el
 * fallo que detecto el diagnostico --index-shp.
 */
qint32 readLittleEndianInt32(const uchar *data)
{
  return static_cast<qint32>(static_cast<quint32>(data[0]) | (static_cast<quint32>(data[1]) << 8)
                             | (static_cast<quint32>(data[2]) << 16) | (static_cast<quint32>(data[3]) << 24));
}

double readLittleEndianDouble(const uchar *data)
{
  double value = 0.0;
  std::memcpy(&value, data, sizeof(double));
  return value;
}

} // namespace

bool ShapefileIndex::supports(const QString &driverName, const QString &sourcePath)
{
  if (driverName != QStringLiteral("ESRI Shapefile"))
    return false;

  const QFileInfo info(sourcePath);
  if (!info.exists() || !info.isFile())
    return false;

  return info.suffix().compare(QStringLiteral("shp"), Qt::CaseInsensitive) == 0;
}

bool ShapefileIndex::scan(const QString &shpPath, std::vector<Entry> &entries, Stats &stats,
                          const std::function<bool(int, const QString &)> &progress, QString *error)
{
  const auto fail = [error](const QString &message) {
    if (error)
      *error = message;
    return false;
  };

  QElapsedTimer timer;
  timer.start();

  entries.clear();
  stats = Stats();

  QFile file(shpPath);
  if (!file.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("No se pudo abrir «%1» para indexar.").arg(shpPath));

  const qint64 fileSize = file.size();
  if (fileSize < kHeaderSize)
    return fail(QStringLiteral("El archivo «%1» es demasiado pequeno para ser un shapefile.")
                  .arg(QFileInfo(shpPath).fileName()));

  // El archivo se recorre entero (612 MB en la capa de edificios): se mapea en
  // memoria para que el recorrido sea puramente secuencial y sin llamadas al
  // sistema por registro. Si el mapeo falla, se cae a lectura por bloques.
  const uchar *data = file.map(0, fileSize);
  QByteArray blockBuffer;
  qint64 blockBase = 0;

  const auto ensureAvailable = [&](qint64 offset, qint64 length, const uchar **pointer) -> bool {
    if (data)
    {
      *pointer = data + offset;
      return true;
    }

    // Reserva: se lee un bloque que cubra el registro completo.
    if (offset < blockBase || offset + length > blockBase + blockBuffer.size())
    {
      const qint64 blockSize = std::max<qint64>(length, 8 * 1024 * 1024);
      if (!file.seek(offset))
        return false;
      blockBuffer = file.read(blockSize);
      blockBase = offset;
      if (blockBuffer.size() < length)
        return false;
    }

    *pointer = reinterpret_cast<const uchar *>(blockBuffer.constData()) + (offset - blockBase);
    return true;
  };

  const uchar *header = nullptr;
  if (!ensureAvailable(0, kHeaderSize, &header))
    return fail(QStringLiteral("No se pudo leer la cabecera de «%1».").arg(QFileInfo(shpPath).fileName()));

  if (readBigEndianInt32(header) != kFileCode)
    return fail(QStringLiteral("«%1» no es un shapefile valido (falta el codigo de archivo).")
                  .arg(QFileInfo(shpPath).fileName()));

  const qint32 declaredLength = readBigEndianInt32(header + 24);
  const qint64 declaredBytes = static_cast<qint64>(declaredLength) * 2;
  if (declaredBytes <= kHeaderSize || declaredBytes > fileSize)
    return fail(QStringLiteral("La cabecera de «%1» declara un tamano (%2 bytes) que no cuadra con el "
                               "archivo (%3 bytes).")
                  .arg(QFileInfo(shpPath).fileName())
                  .arg(declaredBytes)
                  .arg(fileSize));

  // Version y tipo de forma van en little-endian; el codigo y la longitud, en big-endian.
  stats.shapeType = readLittleEndianInt32(header + 32);
  stats.bytes = declaredBytes;

  if (!hasEnvelope(stats.shapeType))
    return fail(QStringLiteral("El tipo de forma %1 de «%2» no esta soportado por el indice.")
                  .arg(stats.shapeType)
                  .arg(QFileInfo(shpPath).fileName()));

  entries.reserve(static_cast<size_t>(declaredBytes / 200));

  qint64 offset = kHeaderSize;
  int lastPercent = -1;

  while (offset + kRecordHeaderSize + 4 <= declaredBytes)
  {
    const uchar *recordHeader = nullptr;
    if (!ensureAvailable(offset, kRecordHeaderSize, &recordHeader))
      break;

    const qint32 contentWords = readBigEndianInt32(recordHeader + 4);
    if (contentWords <= 0)
      break;

    const qint64 contentBytes = static_cast<qint64>(contentWords) * 2;
    const qint64 contentOffset = offset + kRecordHeaderSize;
    if (contentOffset + contentBytes > declaredBytes)
      break;

    if (contentBytes < 4)
      break;

    const uchar *shapeHeader = nullptr;
    if (!ensureAvailable(contentOffset, std::min<qint64>(contentBytes, kEnvelopeSize), &shapeHeader))
      break;

    const qint32 shapeType = readLittleEndianInt32(shapeHeader);

    // Todo registro consume un FID, incluso los vacios: el indice debe mantener
    // la correspondencia 1:1 con los FID que devolvera OGR.
    if (shapeType == 0)
    {
      ++stats.nullShapes;
    }
    else if (hasEnvelope(shapeType) && contentBytes >= kEnvelopeSize
             && (shapeType == stats.shapeType || isPointType(shapeType)))
    {
      Entry entry;
      entry.minX = readLittleEndianDouble(shapeHeader + 4);
      entry.minY = readLittleEndianDouble(shapeHeader + 12);

      if (isPointType(shapeType))
      {
        entry.maxX = entry.minX;
        entry.maxY = entry.minY;
      }
      else
      {
        entry.maxX = readLittleEndianDouble(shapeHeader + 20);
        entry.maxY = readLittleEndianDouble(shapeHeader + 28);
      }

      entries.push_back(entry);
      ++stats.records;
    }
    else
    {
      ++stats.unsupported;
    }

    offset = contentOffset + contentBytes;

    if ((stats.records & 0xFFFF) == 0)
    {
      const int percent = static_cast<int>(100 * offset / declaredBytes);
      if (percent != lastPercent)
      {
        lastPercent = percent;
        if (progress && !progress(percent, QStringLiteral("Indexando envolventes: %1 elementos...")
                                             .arg(entries.size())))
        {
          if (data)
            file.unmap(const_cast<uchar *>(data));
          return fail(QStringLiteral("Indexado cancelado por el usuario."));
        }
      }
    }
  }

  if (data)
    file.unmap(const_cast<uchar *>(data));
  file.close();

  stats.scanMs = timer.elapsed();

  if (!progress || progress(100, QStringLiteral("Indice de envolventes: %1 elementos.").arg(stats.records)))
    return true;

  return fail(QStringLiteral("Indexado cancelado por el usuario."));
}

} // namespace minigis
