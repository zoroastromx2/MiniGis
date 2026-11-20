# Spec: apertura y visualización de GeoParquet

> Módulo: `geoparquet` · Estado: **aprobada e implementada (2026-11-20)** · Fecha: 2026-11-20
>
> Implementación: GDAL 3.12.4 recompilado con la feature `parquet` de vcpkg
> (Arrow 26.0.0), `--check-geoparquet` en `src/core/minigisgeoparquetinspect.*`,
> rechazos en `VectorLayer`, filtro `*.parquet` en el diálogo y 12 pruebas
> nuevas de CTest. Verificado con `09Mun`/`09Man` y con 51 pruebas CTest (48 en
> verde; las 3 de PostGIS se omiten sin servidor).
>
> Este documento define *qué* se construye y *cómo se sabe que está bien*. No se
> escribe código hasta que se apruebe. El plan técnico y las tareas van después,
> en `tasks/plan-geoparquet.md` y `tasks/todo-geoparquet.md`.
>
> Una sola capacidad (`geoparquet`): no aplica el mapa de capacidades de la fase 0.

---

## 1. Objetivo

MiniGIS debe abrir y dibujar archivos **GeoParquet** (`.parquet`) con la misma
calidad que ya tiene con shapefile, GeoPackage, GeoJSON y KML: capa en el panel,
geometría en el mapa, atributos en la tabla, identificar, etiquetas, `--info` y
`--snapshot`.

El caso que lo motiva son los archivos reales del usuario:

* `D:\F\IRIS 4.0\2026\GeoParquet\09Mun.parquet` (16 municipios de CDMX, 261 KB);
* `D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet` (67 375 manzanas, 22,3 MB,
  1 403 803 vértices).

Hoy **MiniGIS no puede abrirlos**: su GDAL de vcpkg (3.12.4) se compiló sin el
driver `Parquet`, aunque el puerto de vcpkg sí ofrece esa característica. El
archivo no está roto: con el GDAL de OSGeo4W (3.12.2), que sí trae el driver, se
abre, se reproyecta y se lee completo. El trabajo es, por tanto, **habilitar el
motor y fijar el contrato**, no escribir un lector de Parquet propio.

Este cambio:

* compila GDAL con el driver `Parquet` a través de la feature `parquet` de vcpkg;
* abre `.parquet` como cualquier vectorial, con detección por contenido;
* dibuja **todas** las geometrías que GDAL entregue, incluidas las de tipo mixto,
  sin perder elementos en silencio;
* conserva los atributos, el `fid` del archivo y el SRC (aquí un LCC mexicano
  **sin código EPSG de CRS proyectado**);
* rechaza con un mensaje accionable lo que no es un GeoParquet (una tabla
  Parquet sin geometría, una carpeta particionada, un archivo corrupto);
* lo verifica todo sin ventana con `--check-geoparquet`, al estilo de
  `--check-kml`.

**Usuario:** quien recibe datos en GeoParquet —cada vez más habitual en
intercambios y en ecosistemas Arrow/DuckDB— y hoy tiene que convertirlos a
GeoPackage con `ogr2ogr` antes de verlos.

**Criterio de éxito del producto:** soltar `09Man.parquet` en la ventana y ver
las manzanas de CDMX en su sitio (reproyectadas de LCC a EPSG:4326), con sus
10 campos en la tabla, sin bloqueos, y con un `--snapshot` y un
`--check-geoparquet` que demuestren que están las 67 375.

---

## 2. Estado de partida (verificado hoy)

Comprobaciones ejecutadas en este equipo el 2026-11-20 con
`build\windows-msvc-release\bin\minigis.exe` (MiniGIS 0.3.0, Qt 6.10.3, GDAL
3.12.4, PROJ 9.9.0, MSVC 19.44, Release), con la API C de GDAL 3.12.4 y con el
GDAL de OSGeo4W 3.12.2 como sonda de lectura.

### 2.1 El driver no está en el binario de MiniGIS

| Comprobación | Resultado verificado |
|---|---|
| `minigis --list-drivers` | **no aparece `Parquet`** (sí ADBC, Arrow no, GeoParquet no) |
| `GDALGetDriverByName("Parquet")` en el GDAL de vcpkg | **nulo**; el driver no está compilado |
| Puerto de vcpkg `gdal` | ofrece la feature **`parquet`**, que depende de `arrow[parquet]`; también existe `arrow` |
| Árbol de vcpkg instalado | **no hay** `arrow.dll`, `parquet.dll`, cabeceras de Arrow ni paquetes `thrift`/`snappy` |
| Consecuencia hoy | abrir un `.parquet` con MiniGIS devuelve «GDAL no pudo abrir» y el hint genérico de formatos; el archivo es válido |
| Modo de vcpkg del preset | `MINIGIS_USE_VCPKG_MANIFEST=OFF`: **configurar no instala nada**, se reutiliza `D:\VCPKG\installed`. La feature se instala una vez con `D:\VCPKG\vcpkg.exe install "gdal[parquet]" --triplet x64-windows` |
| GDAL de OSGeo4W 3.12.2 (`D:\OSGeo4w`) | **sí** tiene los drivers `Parquet` ((Geo)Parquet) y `Arrow`; se usa solo para sondear, **no** es una dependencia de MiniGIS |

> **Actualización tras T0 (2026-11-20):** la tabla anterior conserva el estado
> inicial, que es la evidencia del problema. Una vez instalada
> `gdal[parquet]`, `minigis --list-drivers` incluye **`Parquet`** y **`Arrow`**, y
> `--info` abre los archivos del usuario con el driver `Parquet`.

### 2.2 Los archivos del usuario (sonda con OSGeo4W 3.12.2)

Carpeta de referencia:
`D:\F\IRIS 4.0\2026\GeoParquet\` — contiene `09Mun.parquet` y `09Man.parquet`
con sus sidecars `09Mun.qmd` y `09Man.qmd` (metadatos de QGIS 3.40.3), más
`GeoParquet.zip` con los mismos cuatro archivos.

| Comprobación | `09Mun.parquet` | `09Man.parquet` |
|---|---|---|
| Tamaño | 261 714 bytes | 22 253 127 bytes |
| Driver / capas | `Parquet`, 1 capa | `Parquet`, 1 capa |
| Nombre de capa | `09Mun` | `09Man` |
| Elementos | **16** | **67 375** |
| Geometría | `Multi Polygon` | `Multi Polygon` |
| Partes / vértices | 16 / ~ | 67 688 / **1 403 803** |
| Extensión nativa (LCC) | `2776217.89, 786788.86 : 2820850.33, 846749.51` | `2777625.27, 794925.21 : 2820243.48, 845329.26` |
| SRC | `MEXICO_ITRF_2008_LCC` | idéntico |
| Autoridad del SRC | **sin código EPSG de CRS proyectado** (base `EPSG:8999`; QGIS lo guarda con `srid=0`) | idéntico |
| Extensión reproyectada a 4326 (4 esquinas + centro) | `-99.369, 19.046 : -98.932, 19.597` | `-99.354, 19.120 : -98.938, 19.584` |
| Campos | 6 (`fid`, `CVEGEO`, `CVE_ENT`, `CVE_MUN`, `NOMGEO`, `CVE_CE`) | 10 (`fid`, `CVEGEO`, `CVE_ENT`, `CVE_MUN`, `CVE_LOC`, `CVE_AGEB`, `CVE_MZA`, `TIPOMZA`, `AMBITO`, `CVE_CE`) |
| `fid` | campo `Integer64`; OGR asigna FID 0…n-1 | campo `Integer64` 1…67 375; OGR FID 0…67 374 |
| GeoParquet | versión **1.1.0**, `encoding: WKB`, `crs` PROJJSON, `covering.bbox`, `orientation: counterclockwise`, `geometry_types: ["MultiPolygon"]` | idéntico |
| Columnas de *covering* | `geometry_bbox.*`; GDAL **no** las expone como atributos | idéntico |
| Apertura | 7,4 ms; `GetFeatureCount` instantáneo | 7,4 ms; lectura de las 67 375 geometrías **371–453 ms** |
| Metadatos QGIS | `09Mun.qmd` (QGIS 3.40.3) junto al `.parquet` | `09Man.qmd` |

La extensión 4326 se obtuvo transformando las **cuatro esquinas y el centro** de
la extensión nativa, que es exactamente lo que ya hace
`VectorLayer::extentInEpsg()` (verificado: el resultado cae en CDMX). No hace
falta tocar el encuadre.

### 2.3 Comportamiento de GDAL que hay que tener en cuenta

| Caso | Resultado verificado con GDAL 3.12.2 |
|---|---|
| GeoParquet 1.1 escrito por GDAL/QGIS con `crs` PROJJSON | SRC **correcto** (los archivos del usuario) |
| GeoParquet 1.0/1.1 escrito por GeoPandas 1.1.4 + pyarrow 23 | `geo` trae PROJJSON válido, pero GDAL informa **SRC desconocido**. **Verificado también con 3.12.4**: sigue ocurriendo. Los archivos escritos por GDAL/QGIS (los del usuario) sí traen SRC |
| Tabla Parquet sin geometría | abre 1 capa con `Geometry: None` y sus atributos; hoy MiniGIS la mostraría vacía sin explicar por qué |
| GeoParquet con tipos mixtos | GDAL declara `Unknown (any)` y entrega cada elemento; el almacén ya clasifica por elemento (trabajo de KML) |
| GeoParquet vacío (0 filas) | abre 1 capa con 0 elementos y `Unknown (any)` |
| Carpeta con varios `.parquet` (particionado) | el puerto de vcpkg `parquet` **no** activa `arrow[dataset]`; la lectura de carpetas no está soportada por esta build |
| FID | sin columna FID declarada, GDAL numera 0…n-1; con `-lco FID=...` hay columna con nombre |
| *Covering bbox* | GDAL lo usa para filtros espaciales y lo oculta de los campos; MiniGIS no filtra espacialmente al leer, así que solo debe ignorarlo, no dibujarlo |

**Desviaciones de entorno relevantes:**

* vcpkg solo tiene la variante **Release** de GDAL; todo se compila y prueba con
  `windows-msvc-release`.
* La instalación de vcpkg vive en `D:\VCPKG`, **fuera del workspace**. Cambiar el
  manifiesto y recompilar GDAL requiere aprobación explícita, red para descargar
  Arrow y tiempo de compilación (estimado 30–90 minutos según caché).
* Los fixtures binarios se pueden generar con el GDAL de OSGeo4W (tiene driver de
  escritura Parquet) o con el Python de OSGeo4W (pyarrow 23.0.1 + GeoPandas
  1.1.4). No se añade ninguna herramienta nueva al proyecto.

**Conclusión del sondeo:** el motor vectorial, el almacén, el render y el
encuadre sirven tal cual. Lo que falta es **compilar el driver**, **decidir el
contrato de lo que no es GeoParquet**, **probar el SRC sin código EPSG**, **medir
con 67 375 elementos** y **fijar un diagnóstico verificable**.

---

## 3. Requisitos funcionales

### R1 — Apertura y detección de tipo

* Un archivo `.parquet` (o `.geoparquet`) se abre con `GDALOpenEx` en modo
  lectura y con `GDAL_OF_VECTOR`, como cualquier vectorial. La detección sigue
  siendo **por contenido**: la extensión solo decide el filtro del diálogo y el
  mensaje de error, nunca si el archivo se abre.
* El driver debe ser `Parquet`. Si GDAL no lo tiene compilado, el error lo dice
  y sugiere la acción concreta (recompilar GDAL con la feature `parquet`), en
  lugar del «no pudo abrir» genérico. Código de salida **3**.
* Un archivo inexistente devuelve **2**; un archivo que abre pero no tiene capas
  devuelve **5**.
* Una **tabla Parquet sin columna de geometría** se rechaza con código **5** y
  un mensaje que distingue «es Parquet pero no es GeoParquet»; no se abre una
  capa vacía que parezca un fallo de dibujo.
* Una **carpeta** que contiene `.parquet` se rechaza con un mensaje que pide
  abrir el archivo, no la carpeta, y explica que las colecciones particionadas
  no están soportadas por esta build (código **3** o **5**, el que corresponda al
  error real de GDAL).
* El diálogo **Archivo > Abrir capa** ofrece `*.parquet` y `*.geoparquet`;
  arrastrar y soltar un `.parquet` sobre el lienzo lo abre.
* Un GeoParquet con 0 elementos es una capa válida con 0 elementos; no es un
  error de apertura.

### R2 — Geometría: todo lo que GDAL entrega, sin pérdidas silenciosas

* Se dibujan las geometrías WKB del archivo, con sus partes y anillos:
  `Point`, `LineString`, `Polygon`, `MultiPoint`, `MultiLineString`,
  `MultiPolygon` y `GeometryCollection`.
* En capas con tipo declarado desconocido (habitual cuando hay tipos mixtos), el
  tipo se decide **elemento a elemento**, con la misma ruta que ya usa KML. Un
  elemento con `Point` y otro con `Polygon` en el mismo archivo se dibujan los
  dos; la capa se presenta como **Mixta**.
* Se aceptan Z y M; la altitud se ignora para el dibujo (igual que en el resto
  de formatos) y no rompe la lectura.
* Un elemento sin geometría se cuenta como `withoutGeometry`; no se descarta en
  silencio ni se confunde con un error.
* Si una parte de la geometría no se puede dibujar, se cuenta en
  `unsupportedParts` y `--check-geoparquet` lo informa. El elemento que sí tiene
  partes dibujables no se tira entero.
* Las columnas de *covering* (`geometry_bbox.*`) no se exponen como atributos ni
  se dibujan; son metadatos de filtrado.
* Si GeoParquet 1.1 usa codificación GeoArrow o WKT en lugar de WKB, se dibuja
  igual **si GDAL la entrega**. La prueba obligatoria es WKB, que es lo que
  escriben QGIS, GDAL y las herramientas del usuario; GeoArrow se comprueba como
  caso adicional si el driver lo permite.

### R3 — Atributos, `fid` y tipos

* Todos los campos del archivo aparecen en la tabla, en el orden de GDAL, con su
  nombre y su tipo legible (`Integer64`, `String`, `Real`, `Date`…).
* Un campo `fid` del archivo se conserva como **atributo**; el FID interno de OGR
  puede ser 0…n-1 y no tiene por qué coincidir con él (en `09Man`, el atributo
  `fid` va de 1 a 67 375 y el FID de OGR de 0 a 67 374).
* Los enteros de 64 bits se muestran con todos sus dígitos (no se convierten a
  `double`); los tipos `decimal`, fecha/hora, listas y estructuras se muestran
  como texto de OGR, sin truncar y sin hacer caer la tabla.
* `NULL` y cadena vacía se distinguen como ya hace `VectorLayer`.
* La caché de atributos por fila sigue evitando releer el archivo para cada
  celda; `--check-geoparquet` informa si la segunda lectura de una fila sale de
  la caché.

### R4 — SRC y reproyección

* El SRC se lee de GDAL y se usa para reproyectar al SRC del lienzo
  (`EPSG:4326` por defecto). No se exige que el SRC tenga código EPSG: los
  archivos del usuario son un LCC con nombre `MEXICO_ITRF_2008_LCC` y **sin**
  autoridad de CRS proyectado.
* `--info` y el panel muestran el nombre del SRC y, si existe, su EPSG. Para el
  LCC del usuario, `crs.epsg` vale **0** y la descripción dice «SRC proyectado
  sin codigo EPSG - MEXICO_ITRF_2008_LCC»; eso es correcto, no un fallo.
* La capa se dibuja en su sitio: el encuadre de `09Mun`/`09Man` cae en CDMX
  (≈ -99,2 / 19,4 en 4326).
* Si GDAL no entrega ningún SRC, la capa se abre con «SRC desconocido», se
  advierte en `--info` y en el panel, y **no se asume EPSG:4326 en silencio**.
  No se reproyecta. Es el mismo contrato que ya tienen los formatos sin `.prj`.
* `--crs EPSG:25830` (o cualquier destino válido) reproyecta también un
  GeoParquet, como el resto de capas.

### R5 — Interfaz

* **Abrir capa** incluye `*.parquet` y `*.geoparquet`; el arrastrar y soltar no
  necesita cambios de fondo (la detección es por contenido).
* El panel muestra la capa agrupada bajo el nombre del archivo
  (`09Man.parquet`), con el tipo de geometría legible (`Multi Polygon` o
  `Poligono`), el número de elementos y el SRC.
* La tabla de atributos muestra los 10 campos de `09Man` y responde sin
  bloquear la ventana al desplazarse; la carga de 67 375 elementos sigue
  haciéndose en segundo plano.
* Identificar, etiquetas y simbología funcionan sin controles nuevos: el
  GeoParquet no trae estilos, así que se aplica la simbología por defecto de la
  capa, igual que un shapefile.
* Cargar `09Man.parquet` no congela la ventana: el almacén se construye en el
  hilo de trabajo y la barra de progreso avanza; cancelar la carga la aborta sin
  dejar la capa a medias.

### R6 — Línea de comandos

* `--info`, `--info --json`, `--snapshot` y `--benchmark` aceptan `-l archivo.parquet`
  sin opciones nuevas.
* `--list-drivers` incluye `Parquet` (y `Arrow` si la build de vcpkg lo trae).
* Se añade `--check-geoparquet`, con `--json`, siguiendo el patrón de
  `--check-kml` y `--check-pg`.
* Códigos de salida: `0` éxito, `1` uso, `2` archivo no encontrado, `3` GDAL no
  abre o no hay driver, `4` el diagnóstico no está `ok`, `5` sin capas o sin
  geometría. No se cambian los códigos existentes.
* `stdout` sigue siendo datos y `stderr` diagnóstico.

### R7 — `--check-geoparquet`: contrato verificable

El comando abre el archivo, recorre cada capa con la API C de OGR, construye el
almacén de geometrías con las mismas opciones que la carga normal y compara las
dos orillas. Informa, por capa:

* `name`, `features`, `withoutGeometry`, `unsupportedParts`;
* `points`, `lines`, `polygons` (partes dibujables, no elementos);
* `storeFeatures` (−1 si no se construyó el almacén por presupuesto), y el
  veredicto `missingInStore`;
* `crsEpsg`, `crsName`, `crsDescription`, `extent` nativa y `extent4326`;
* `fields` (nombre y tipo) y `fidColumn` (vacío si no hay);
* `attributeCache` (si la segunda lectura de una fila no vuelve al origen).

`ok` es `true` solo si todas las capas conservan en el almacén tantos elementos
dibujables como entrega OGR, no hay partes no soportadas y no hubo error de
lectura. Si el almacén no se construyó por presupuesto, no se puede afirmar que
falten elementos: `storeFeatures` vale −1, `missingInStore` es `false` y el
diagnóstico lo dice explícitamente.

Ejemplo de forma del JSON (los valores son ilustrativos):

```json
{
  "path": "data/sample/geoparquet/parcels.parquet",
  "driver": "Parquet",
  "layers": [
    {
      "name": "parcels",
      "features": 5,
      "withoutGeometry": 0,
      "unsupportedParts": 0,
      "points": 0,
      "lines": 0,
      "polygons": 5,
      "storeFeatures": 5,
      "missingInStore": false,
      "crsEpsg": 4326,
      "crsName": "WGS 84",
      "crsDescription": "EPSG:4326 - WGS 84",
      "extent": [-3.76, 40.36, -3.68, 40.42],
      "extent4326": [-3.76, 40.36, -3.68, 40.42],
      "fields": [
        { "name": "parcel_id", "type": "String" },
        { "name": "area_ha", "type": "Real" }
      ],
      "fidColumn": "",
      "attributeCache": true
    }
  ],
  "ok": true
}
```

El texto sin `--json` es una tabla legible por capa, con la misma información y
una línea final `ok` / `revisar`, como `--check-kml`.

### R8 — Rendimiento y memoria

Medido sobre `09Man.parquet` en Release, con el GDAL de vcpkg ya con driver:

| Operación | Presupuesto | Medido (2026-11-20) |
|---|---|---|
| `--info` (apertura, recuento y extensión) | **≤ 1 s** | **109 ms** (`09Mun`: 113 ms) |
| Construcción del almacén (67 375 elementos, 1,4 M vértices; lectura + índice + niveles) | **≤ 4 s** | **1,13 s** (todo `--check-geoparquet`) |
| `--snapshot` 1600×900 del archivo completo | **≤ 2 s** | **1,16 s** (`09Mun`: 0,32 s) |
| Cuadro de calidad con la capa completa en pantalla | **≤ 500 ms** | **113 ms** de render |
| Primer cuadro tras soltar el archivo en la ventana (carga en segundo plano incluida) | **≤ 5 s** | **2,20 s** (`--snapshot-window` con tabla) |

* El archivo de 22,3 MB cabe en el presupuesto de vértices del almacén
  (`vertexBudget` = 8 000 000); no debe caer al camino de lectura directa.
* Un GeoParquet que supere el presupuesto de vértices se abre igual, con el
  aviso existente de «no se pudo indexar, se usará la lectura directa», y el
  render no debe bloquearse.
* La puerta `minigis.rendimiento` sigue en verde; el nuevo formato no puede
  degradar los existentes.

### R9 — No regresión

* `ctest --preset windows-msvc-release` completo en verde, incluidas
  `minigis.rendimiento` y las pruebas de KML, PostGIS, ráster y proyecto.
* Shapefile, GeoPackage, GeoJSON y KML conservan sus salidas de `--info --json`
  y `--snapshot` (sin diferencias de capas, campos, SRC ni píxeles).
* Si el driver `Parquet` no está compilado, **solo** fallan los `.parquet`; el
  resto de formatos sigue funcionando.
* No se toca el contrato de `VectorLayer`, del almacén ni del render más allá de
  los puntos de extensión necesarios (error específico y diagnóstico nuevo).

### R10 — Documentación

* `README.md`: GeoParquet en la lista de formatos, en la tabla de la CLI
  (`--check-geoparquet`), en los ejemplos y en la solución de problemas
  (incluida la feature `parquet` de vcpkg y el síntoma «driver no disponible»).
* `RENDIMIENTO.md`: nota de la medición con `09Man` si aporta una cifra nueva.
* Este spec se actualiza con lo aprendido (versión de GDAL, CRS de GeoPandas,
  tiempos finales) al terminar.

---

## 4. Decisiones (y por qué)

* **D1 — El lector es el driver `Parquet` de GDAL, no uno propio.** La regla del
  proyecto es GDAL como motor de datos y la API C de OGR como superficie. Un
  lector de Parquet propio exigiría Thrift, compresión y Arrow: miles de líneas
  para algo que GDAL ya hace, y una segunda semántica de SRC y FID que
  mantener. La contrapartida es compilar GDAL con Arrow; se asume.
* **D2 — Habilitar la feature en el manifiesto de vcpkg, no un plugin suelto.**
  El puerto ofrece `gdal[parquet]`; un plugin compilado contra el GDAL de vcpkg
  y las DLL de Arrow de OSGeo4W sería más rápido de probar, pero mezcla dos
  instalaciones y deja el despliegue fuera de vcpkg. Se descarta como solución
  (solo se usaría, como mucho, como sonda temporal).
* **D3 — Solo archivos locales y una capa por archivo.** El caso del usuario es
  un `.parquet` suelto. Las carpetas particionadas necesitan `arrow[dataset]`,
  que la feature `parquet` no activa; HTTP/S3 queda fuera. Rechazarlas con un
  mensaje claro es mejor que abrir media capa.
* **D4 — Una tabla Parquet sin geometría no es un GeoParquet y se rechaza.**
  Abrirla como capa vacía haría creer que el dibujo falló. El mensaje distingue
  «Parquet válido, sin geometría» de «GDAL no lo reconoce» y de «driver
  ausente», que son tres acciones distintas para el usuario.
* **D5 — El SRC se toma de GDAL tal cual, incluido un CRS sin EPSG.** Los
  archivos del usuario son un LCC mexicano sin autoridad de CRS proyectado; la
  reproyección funciona con el WKT y no debe depender de `AutoIdentifyEPSG()`.
  Si no hay SRC, se advierte; no se inventa 4326.
* **D6 — `--check-geoparquet` compara OGR y almacén.** Es la única forma de
  demostrar que no se pierden elementos sin mirar píxeles, y ya es el patrón de
  KML. No se inventa un contrato distinto para el mismo problema.
* **D7 — Los fixtures son propios y pequeños; los archivos del usuario no se
  versionan.** `09Man` (22 MB) se usa para medir a mano; las pruebas usan
  GeoParquet de pocos elementos generados con GDAL. Así `ctest` es rápido y
  reproducible.
* **D8 — No hay simbología ni estilos en GeoParquet.** Se aplica la simbología
  por defecto de MiniGIS; no se inventa un mapeo de metadatos a `Style`. Si en
  el futuro se quiere, será otro spec.
* **D9 — El `fid` del archivo es un atributo, no el FID de OGR.** La tabla y
  `Identificar` ya funcionan con el FID interno; renombrar o sustituir columnas
  rompería otros formatos y no hace falta.

---

## 5. Fuera de alcance (explícito)

* **Escritura, edición o actualización** de GeoParquet. Solo lectura.
* **Colecciones particionadas** (carpetas, Hive, `PARQUET:`), *datasets* de
  Arrow y lectura remota (`http://`, `s3://`, `/vsicurl/`).
* **Filtrado espacial o por atributos con pushdown** al *covering* o a los
  *row groups*. MiniGIS lee la capa completa para el almacén; el *covering* solo
  se ignora.
* **Tablas Parquet sin geometría** como capas de solo atributos.
* **Estilos**: no hay metadatos de simbología que interpretar.
* **Otros formatos Arrow** (`.arrow`, `.feather`, IPC): otro driver y otro spec.
* **Soporte de cifrado Parquet** o códecs que la build de Arrow no traiga.
* **Columnas de geometría múltiples** más allá de lo que GDAL exponga como
  capas: si las expone, se cargan como un archivo multicapa; no se diseña una
  interfaz para elegir columna.
* **Cambios en el motor de render** (salvo los ya previstos por KML/tipo mixto).

---

## 6. Comandos

### 6.1 Compilar y probar (el bucle del proyecto)

```bat
:: Una sola vez: instalar GDAL con la feature Parquet en el arbol compartido
D:\VCPKG\vcpkg.exe install "gdal[parquet]" --triplet x64-windows --recurse

scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

`configure` **no** instala dependencias en este proyecto: el preset usa
`MINIGIS_USE_VCPKG_MANIFEST=OFF` y reutiliza `D:\VCPKG\installed`. La
instalación de `gdal[parquet]` es el paso largo y se hace aparte; después,
reconfigurar basta para que CMake encuentre el GDAL nuevo. Para reproducir el
build solo con el manifiesto existe la opción
`-DMINIGIS_USE_VCPKG_MANIFEST=ON`, que instala en `build\<preset>\vcpkg_installed`
y compila todo desde cero (no es el camino verificado en este equipo).

### 6.2 Uso de lo nuevo

```bat
bin\minigis.exe --list-drivers
bin\minigis.exe --info "D:\F\IRIS 4.0\2026\GeoParquet\09Mun.parquet"
bin\minigis.exe --info --json "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet"
bin\minigis.exe --snapshot manzanas.png -l "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet" --size 1600x900
bin\minigis.exe --check-geoparquet "data\sample\geoparquet\parcels.parquet" --json
bin\minigis.exe --check-geoparquet "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet" --json
```

### 6.3 Fixtures

```bat
scripts\make_geoparquet_samples.bat
```

Genera los `.parquet` pequeños de `data\sample\geoparquet\` a partir de los
GeoJSON/shapefiles ya versionados, con `ogr2ogr` (OSGeo4W o cualquier GDAL con
driver Parquet). Los archivos resultantes se versionan; el script se documenta
para poder regenerarlos, no se ejecuta en `ctest`.

---

## 7. Estructura del proyecto

```
vcpkg/vcpkg.json                         # gdal con la feature "parquet"
scripts/make_geoparquet_samples.bat      # genera los fixtures (no en ctest)
data/sample/geoparquet/
  parcels.parquet                        # 5 polígonos, EPSG:4326, campos variados
  cities_utm30.parquet                   # 12 puntos, EPSG:25830 (reproyección)
  mixto.parquet                          # punto + línea + polígono (si el writer lo permite)
  sin_geometria.parquet                  # filas con geometría nula
  sin_crs.parquet                        # GeoParquet con crs nulo
  tabla.parquet                          # Parquet sin geometría (negativo)
  vacio.parquet                          # 0 elementos
src/core/minigisgeoparquetinspect.{h,cpp} # --check-geoparquet (o nombre equivalente)
src/core/minigisvectorlayer.cpp          # error específico de driver ausente / sin geometría
src/app/main.cpp                         # opción, códigos y JSON de --check-geoparquet
src/gui/minigismainwindow.cpp            # filtro *.parquet / *.geoparquet
tests/CMakeLists.txt                     # pruebas nuevas
README.md                                # formatos, CLI y solución de problemas
```

No se añaden fuentes con `file(GLOB ...)`: las listas de `CMakeLists.txt` son
explícitas.

---

## 8. Estilo de código

* C++17, Qt 6 y GDAL por la **API C de OGR** (`OGR_L_*`, `OGR_F_*`,
  `GDALOpenEx`), nunca las clases C++ de `OGRLayer`.
* Comentarios en español, sin tildes, en estilo Doxygen sobre lo público;
  explican el porqué (por qué se rechaza una tabla sin geometría, por qué el
  `fid` no se toca), no el qué.
* Errores con `minigis::Error { message, hint }`: qué pasó y qué puede hacer el
  usuario. Un error sin acción es un defecto.
* Nombres `PascalCase` / `camelCase` / miembros `m*` / `enum class` / namespace
  `minigis`; archivos `minigis<nombre>.cpp/.h`.

Ejemplo del tipo de mensaje que se busca (no es código final):

```cpp
return Error {
  QStringLiteral("«%1» es un archivo Parquet, pero no contiene ninguna columna de geometria.")
    .arg(QDir::toNativeSeparators(path)),
  QStringLiteral("MiniGIS abre GeoParquet, no tablas Parquet. Exporta la capa con "
                 "geometria desde QGIS/DuckDB u ogr2ogr y vuelve a intentarlo.")};
```

---

## 9. Estrategia de pruebas

### 9.1 Fixtures deterministas

Se generan una vez con GDAL y se versionan. Todos son pequeños (< 100 KB), salvo
que se decida incluir una muestra de 1 000 manzanas (≈ 300 KB) para medir el
almacén.

| Fixture | Contenido | Para qué |
|---|---|---|
| `parcels.parquet` | 5 polígonos, EPSG:4326, `parcel_id`, `uso`, `area_ha`, `manzana` | apertura, atributos, SRC, snapshot |
| `cities_utm30.parquet` | 12 puntos, EPSG:25830 | reproyección y encuadre |
| `mixto.parquet` | punto + línea + polígono | clasificación por elemento y tipo «Mixta» |
| `sin_geometria.parquet` | 2 filas, una con geometría nula | `withoutGeometry`, no perder filas |
| `sin_crs.parquet` | geometría en 4326, `crs: null` | «SRC desconocido», no asumir 4326 |
| `tabla.parquet` | tabla sin geometría | rechazo con código 5 y mensaje accionable |
| `vacio.parquet` | 0 elementos | capa válida, `ok` con 0 |
| `parcels_fid.parquet` | `-lco FID=parcel_id` | `fidColumn` y FID no contiguo, si el driver lo permite |

Los fixtures se generan desde `data/sample/parcels.geojson`,
`data/sample/shapefiles/parcels_utm30.shp` y `data/sample/kml/mixto.kml`; si el
writer de GDAL no puede escribir tipos mixtos, `mixto.parquet` se genera con
pyarrow/GeoPandas y se documenta que su SRC puede quedar desconocido.

### 9.2 Pruebas de CTest

| Prueba | Comprueba |
|---|---|
| `minigis.geoparquet-info` | `--info --json`: driver `Parquet`, 5 elementos, campos y `crs.epsg` 4326 |
| `minigis.geoparquet-snapshot` | PNG 1200×800 válido y con píxeles no vacíos |
| `minigis.geoparquet-check` | `--check-geoparquet --json`: `ok: true`, `storeFeatures == features`, `unsupportedParts == 0` |
| `minigis.geoparquet-mixto` | el fixture mixto conserva punto, línea y polígono; tipo «Mixta» |
| `minigis.geoparquet-sin-geometria` | `withoutGeometry` correcto, sin pérdida de filas |
| `minigis.geoparquet-sin-crs` | abre, `crs.epsg` 0, descripción de SRC desconocido, sin asumir 4326 |
| `minigis.geoparquet-tabla` | código 5 y mensaje que menciona «no es GeoParquet» |
| `minigis.geoparquet-carpeta` | código de error y mensaje que pide el archivo, no la carpeta |
| `minigis.geoparquet-vacio` | 0 elementos, `ok: true` |
| `minigis.geoparquet-reproyeccion` | `--snapshot --crs EPSG:4326` de un archivo UTM no queda fuera del lienzo |
| `minigis.rendimiento` | sigue en verde; no se degrada por el cambio |

Pruebas de humo con `run_cli_test.cmake`, en plataforma `offscreen`, con el
binario ya compilado en el preset.

### 9.3 Matriz requisito → prueba

| Requisito | Prueba |
|---|---|
| R1 apertura y rechazos | `geoparquet-info`, `geoparquet-tabla`, `geoparquet-carpeta`, `geoparquet-vacio` |
| R2 geometría sin pérdidas | `geoparquet-check`, `geoparquet-mixto` |
| R3 atributos y `fid` | `geoparquet-info`, `parcels_fid` (si aplica) |
| R4 SRC sin EPSG y reproyección | `geoparquet-sin-crs`, `geoparquet-reproyeccion`, medición manual con `09Man` |
| R5 interfaz | `geoparquet-snapshot` + captura manual |
| R6 CLI | todas las pruebas de CLI |
| R7 contrato del check | `geoparquet-check` |
| R8 rendimiento | medición manual + `minigis.rendimiento` |
| R9 no regresión | `ctest` completo |

### 9.4 Medición manual con los archivos del usuario

```bat
bin\minigis.exe --info --json "D:\F\IRIS 4.0\2026\GeoParquet\09Mun.parquet"
bin\minigis.exe --info --json "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet"
bin\minigis.exe --check-geoparquet --json "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet"
bin\minigis.exe --snapshot manzanas.png -l "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet" --size 1600x900
```

Se anotan los tiempos y se comparan con el presupuesto de R8. El `--snapshot`
debe mostrar CDMX, no un mapa vacío ni el archivo entero fuera de la vista.

---

## 10. Límites

* **Dependencia de compilación:** el driver no existe hasta que se recompile
  GDAL con Arrow/Parquet. Mientras tanto, `--check-geoparquet` falla con un
  mensaje accionable; no hay forma de abrir GeoParquet sin esa compilación.
* **Tiempo y red del vcpkg:** Arrow es una dependencia grande; la compilación
  puede tardar decenas de minutos y necesita red. Si falla, el feature queda
  bloqueado y hay que revertir el manifiesto; no hay plan B de leer Parquet a
  mano.
* **CRS de GeoPandas:** con GDAL 3.12.4 (verificado) un GeoParquet escrito por
  GeoPandas 1.1.4 declara el CRS en `geo` pero GDAL lo informa desconocido. El
  archivo se abre con «SRC desconocido» y un aviso; **no** se asume 4326. Los
  archivos del usuario (1.1.0 escritos por GDAL/QGIS) sí traen SRC y son el caso
  de referencia.
* **Sin índice espacial:** se lee la capa completa para construir el almacén;
  no se aprovechan *row groups* ni *covering* para filtros. Un archivo enorme
  puede tardar o caer al camino de lectura directa.
* **FID:** no hay columna FID obligatoria en GeoParquet; el FID de OGR puede ser
  sintético. La identidad estable es el campo que traiga el archivo (`fid`,
  `CVEGEO`…), no el FID interno.
* **Tipos anidados:** listas y estructuras se muestran como texto; no hay tabla
  expandida ni edición.
* **Códecs:** los que traiga Arrow en vcpkg (snappy, gzip, brotli, zstd…). Un
  archivo comprimido con un códec no disponible da error de GDAL, con el hint
  de la causa.
* **Nombres de columna:** el driver reconoce la columna primaria por el metadato
  `primary_column`; una columna llamada `geometry` sin metadato se detecta por
  heurística. Un GeoParquet mal formado puede abrir sin geometría y se trata
  como tabla (rechazo de R1).

---

## 11. Criterios de éxito

1. `bin\minigis.exe --list-drivers` incluye `Parquet` tras la compilación.
2. `--info --json` de `09Mun.parquet`: driver `Parquet`, 1 capa, 16 elementos,
   `Multi Polygon`, SRC `MEXICO_ITRF_2008_LCC` (EPSG 0), extensión nativa
   correcta, en **≤ 1 s**.
3. `--info --json` de `09Man.parquet`: 67 375 elementos, 10 campos, sin campos
   `geometry_bbox.*`, en **≤ 1 s**.
4. `--snapshot` 1600×900 de `09Mun.parquet` y `09Man.parquet`: PNG con las
   manzanas de CDMX en su sitio (no vacío, no fuera de la vista), en **≤ 2 s**.
5. `--check-geoparquet --json` de `09Man.parquet`: `ok: true`,
   `storeFeatures == 67375`, `unsupportedParts == 0`, `crsName` correcto.
6. El fixture mixto dibuja punto, línea y polígono; el tipo que muestra la capa
   es «Mixta» y no se pierde ninguna parte.
7. Una tabla Parquet sin geometría devuelve **5** y un mensaje que dice «no es
   GeoParquet»; una carpeta devuelve un mensaje que pide el archivo.
8. Un GeoParquet sin SRC se abre con «SRC desconocido», sin asumir 4326, y
   `--check-geoparquet` lo informa; un GeoParquet con SRC LCC se reproyecta bien.
9. `ctest --preset windows-msvc-release` completo en verde, incluida
   `minigis.rendimiento`; shapefile, GeoPackage, GeoJSON y KML no cambian.
10. La ventana sigue respondiendo al cargar `09Man.parquet` (carga en segundo
    plano, primer cuadro ≤ 5 s) y la tabla de atributos se desplaza sin
    bloquearse.
11. `README.md` explica el formato, `--check-geoparquet`, la feature `parquet` y
    el síntoma «driver no disponible».

---

## 12. Supuestos (corregir si alguno no es cierto)

1. **Se autoriza** cambiar `vcpkg/vcpkg.json` para pedir `gdal[parquet]` y
   ejecutar `vcpkg install "gdal[parquet]" --triplet x64-windows` en
   `D:\VCPKG` (operación global, con red y decenas de minutos). Sin esto no hay
   implementación posible. Aprobado el 2026-11-20.
2. Los archivos de referencia son los locales
   `D:\F\IRIS 4.0\2026\GeoParquet\09Mun.parquet` y `09Man.parquet`, en solo
   lectura; no se versionan en el repositorio.
3. El alcance es **un archivo local por capa**, GeoParquet 1.0/1.1 con geometría
   WKB; no hay carpetas particionadas ni fuentes remotas.
4. La corrección del SRC se apoya en GDAL 3.12.4; si el driver nuevo no lee el
   CRS de los archivos del usuario, el feature se replantea antes de seguir
   (no se implementa un lector de metadatos Parquet propio). Los archivos del
   usuario se leen con SRC correcto; GeoPandas queda como limitación conocida.
5. La simbología por defecto y las etiquetas desactivadas son aceptables; el
   GeoParquet no aporta estilos.
6. `--check-geoparquet` es el nombre del modo, por coherencia con
   `--check-kml` y `--check-pg`.
7. Los fixtures pequeños se generan con el GDAL de OSGeo4W y se versionan; no
   se añade una dependencia de Python al build.
8. Solo se compila y prueba `windows-msvc-release`; los presets Debug no
   enlazan en este equipo.
9. El código nuevo no cambia la API pública de `VectorLayer` más allá de un
   error específico y del diagnóstico; el resto de formatos se comportan igual.

---

## 13. Preguntas abiertas (para la revisión)

* **Q1 — Recompilar GDAL.** **Resuelta el 2026-11-20: aprobada.** Se instala
  `gdal[parquet]` con vcpkg en modo clásico; el manifiesto se actualiza para el
  modo manifiesto opcional. Si la compilación falla, el feature se replantea.
* **Q2 — Tabla Parquet sin geometría.** Se propone rechazarla (código 5). ¿Se
  prefiere abrirla como capa de solo atributos, aunque no se dibuje?
* **Q3 — `--source-crs`.** ¿Hace falta una opción para declarar el SRC de un
  GeoParquet que no lo trae (caso GeoPandas 1.0), o basta el aviso de «SRC
  desconocido»? Se propone no añadirla en este cambio.
* **Q4 — Fixtures con GeoPandas.** **Resuelta:** el CRS de GeoPandas sigue sin
  leerse con 3.12.4; se acepta como caso documentado (`sin_crs` ya cubre el
  aviso) y los archivos del usuario, que sí lo traen, son la referencia.
* **Q5 — Rendimiento.** ¿Se acepta `09Man.parquet` (67 375 manzanas) como
  referencia de rendimiento, o hay un archivo más grande que deba marcar el
  presupuesto?
* **Q6 — Extensión.** ¿Se ofrece también `*.geoparquet` en el diálogo, o solo
  `*.parquet` (la extensión que usan QGIS y las herramientas del usuario)?
* **Q7 — Alcance de escritura.** Se asume solo lectura. ¿Hay expectativa de
  exportar o editar GeoParquet en el futuro próximo?
* **Q8 — Dónde documentar la recompilación.** Se propone una sección nueva en
  `README.md` §4 (compilar) y una entrada en solución de problemas; ¿o se
  prefiere un `README` aparte para dependencias?

---

## 14. Resultado verificado (2026-11-20)

Los once criterios de §11, comprobados sobre la compilación Release:

1. `--list-drivers` incluye `Parquet` y `Arrow`.
2. `09Mun.parquet`: driver `Parquet`, 1 capa, **16** elementos, `Multi Polygon`,
   `MEXICO_ITRF_2008_LCC` con EPSG 0, extensión nativa correcta; `--info --json`
   en **113 ms**.
3. `09Man.parquet`: **67 375** elementos, 10 campos, sin columnas
   `geometry_bbox.*`; `--info --json` en **109 ms**.
4. `--snapshot` 1600×900: `09Mun` 323 ms y `09Man` 1,16 s; ambos PNG muestran
   CDMX en su sitio (capturas en `build\geoparquet-artifacts\`).
5. `--check-geoparquet --json 09Man`: `ok: true`, `storeFeatures: 67375`,
   `unsupportedParts: 0`, `crsName: MEXICO_ITRF_2008_LCC`, en **1,13 s**.
6. `mixto.parquet`: punto, línea y dos polígonos conservados; la capa es «Mixta».
7. `tabla.parquet`: exit **5** y «no contiene ninguna columna de geometria»;
   carpeta con `.parquet`: exit **3** y «abre el archivo, no la carpeta».
8. `sin_crs.parquet`: se abre con «SRC desconocido», EPSG 0, sin asumir 4326;
   `parcels_utm30.parquet` se reproyecta desde EPSG:25830.
9. `ctest --preset windows-msvc-release`: **51 pruebas, 0 fallos, 3 omitidas**
   (PostGIS sin servidor). Shapefile, GeoPackage, GeoJSON, KML y ráster siguen
   en verde.
10. `--snapshot-window --with-attributes` de `09Man`: **2,2 s**, 67 375
    elementos, 10 campos y la tabla desplazable; Identificar devuelve la manzana
    con su `CVEGEO`.
11. `README.md` documenta el formato, `--check-geoparquet`, la instalación de
    `gdal[parquet]` y los síntomas de driver ausente, tabla sin geometría,
    carpeta particionada y CRS de GeoPandas.

**Limitación conocida:** GeoPandas 1.1.4 escribe un `crs` que GDAL 3.12.4 no
aplica; los archivos del usuario (GDAL/QGIS 1.1.0) sí se leen con SRC. El aviso
y el no-asumir-4326 están implementados y probados (`minigis.geoparquet-sin-crs`).
