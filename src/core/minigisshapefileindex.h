#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace minigis
{

/**
 * Indice de envolventes de un ESRI Shapefile.
 *
 * Por que existe: una capa de millones de poligonos pequenos no cabe en memoria
 * con su geometria completa (la de edificios de Nueva Zelanda son 3,2 M de
 * elementos y ~32 M de vertices: unos 750 MB), pero si caben sus envolventes
 * (~130 MB). Y no hace falta el filtro espacial de GDAL: sin un indice `.qix`,
 * el driver del shapefile recorre los 3,2 M de registros en cada consulta
 * (medido: 14,5 s por cuadro).
 *
 * El formato .shp permite leer la envolvente de cada elemento sin leer su
 * geometria: cada registro empieza por una cabecera de 8 bytes (numero de
 * registro y longitud) seguida del tipo de forma (4 bytes) y de la envolvente
 * (4 dobles, 32 bytes). Es decir, con 36 bytes por registro se obtiene el indice
 * completo recorriendo el archivo una sola vez de forma secuencial.
 *
 * El indice es valido para leer despues la geometria con OGR: el FID que asigna
 * el driver del shapefile es la posicion del registro en el archivo, de modo que
 * `OGR_L_GetFeature(fid)` recupera el elemento cuya envolvente se guardo aqui.
 */
class ShapefileIndex
{
public:
  struct Entry
  {
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
  };

  struct Stats
  {
    qint64 records = 0;      // registros con envolvente util
    qint64 nullShapes = 0;   // registros vacios (tipo 0)
    qint64 unsupported = 0;  // tipos de forma sin envolvente conocida
    qint64 bytes = 0;        // tamano del archivo recorrido
    qint64 scanMs = 0;
    int shapeType = 0;
  };

  /**
   * Se puede indexar asi esta capa? Solo para el driver ESRI Shapefile, con
   * archivo .shp en disco y geometria (no tablas DBF sueltas).
   */
  static bool supports(const QString &driverName, const QString &sourcePath);

  /**
   * Recorre el .shp y rellena `entries` (una por registro, en el mismo orden que
   * los FID de OGR). `progress` recibe (porcentaje, mensaje) y puede devolver
   * false para cancelar. Devuelve false y explica el motivo en `error` si el
   * archivo no tiene el formato esperado.
   */
  static bool scan(const QString &shpPath, std::vector<Entry> &entries, Stats &stats,
                   const std::function<bool(int, const QString &)> &progress, QString *error);
};

} // namespace minigis
