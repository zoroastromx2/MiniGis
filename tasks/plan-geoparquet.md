# Implementation Plan: apertura y visualización de GeoParquet

> **Estado: implementado y verificado (2026-11-20).** Las tareas T0–T7 están
> hechas; el detalle y la evidencia están en `tasks/todo-geoparquet.md`. La
> instalación real fue `vcpkg install "gdal[parquet]"` en modo clásico y el
> build quedó con 51 pruebas CTest (48 en verde, 3 PostGIS omitidas).

## Overview

Habilitar en MiniGIS la lectura de archivos **GeoParquet** con el driver
`Parquet` de GDAL y fijar el contrato de lo que se abre, lo que se rechaza y lo
que se verifica. El motor vectorial, el almacén de geometrías y el render no se
reescriben: el trabajo es **compilar el driver**, **traducir sus rarezas a
errores accionables**, **medir con los archivos reales del usuario** y
**demostrar sin ventana que no se pierde ningún elemento**.

Spec de referencia: [`SPEC-geoparquet.md`](../SPEC-geoparquet.md).

Archivos de referencia del usuario (solo lectura, no se versionan):

* `D:\F\IRIS 4.0\2026\GeoParquet\09Mun.parquet` — 16 municipios, LCC mexicano.
* `D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet` — 67 375 manzanas, 1,4 M
  vértices.

## Architecture Decisions

* **D1 — El lector es el driver `Parquet` de GDAL.** No se escribe un lector de
  Parquet propio: GDAL ya soporta GeoParquet 1.0/1.1, WKB y *covering*. El
  proyecto usa GDAL como motor de datos y la API C de OGR como superficie.
* **D2 — La dependencia se instala con vcpkg en modo clásico.** El preset
  verificado (`windows-msvc-release`) usa `MINIGIS_USE_VCPKG_MANIFEST=OFF` y
  reutiliza `D:\VCPKG\installed`. Por eso **configurar no instala nada**: hay que
  ejecutar `vcpkg install "gdal[parquet]" --triplet x64-windows` una vez. El
  manifiesto `vcpkg/vcpkg.json` se actualiza igualmente para que el modo
  manifiesto (`-DMINIGIS_USE_VCPKG_MANIFEST=ON`) declare lo mismo.
* **D3 — Solo archivos locales, una capa por archivo.** La feature `parquet` de
  vcpkg no activa `arrow[dataset]`; las carpetas particionadas y las fuentes
  remotas se rechazan con un mensaje que pide el archivo.
* **D4 — Una tabla Parquet sin geometría no es un GeoParquet.** Se rechaza con
  código 5 y un mensaje que distingue «Parquet sin geometría» de «driver
  ausente»; no se abre una capa vacía que parezca un fallo de dibujo.
* **D5 — El SRC se toma de GDAL, incluido un CRS sin EPSG.** Los archivos del
  usuario son un LCC con nombre y sin autoridad de CRS proyectado; la
  reproyección usa el WKT. `crsEpsg` 0 no es un error.
* **D6 — `--check-geoparquet` compara OGR y almacén.** Mismo patrón que
  `--check-kml`: cuenta los elementos que entrega GDAL, construye el almacén y
  compara. Sin eso, una pérdida silenciosa no rompe ninguna prueba.
* **D7 — Los fixtures son propios, pequeños y versionados.** Se generan con
  `ogr2ogr` de OSGeo4W (driver Parquet de escritura) y, para los casos que GDAL
  no sabe escribir, con pyarrow/GeoPandas. `09Man` se usa para medir a mano.
* **D8 — Sin estilos ni controles nuevos.** GeoParquet no trae simbología; se
  aplica la de por defecto. La interfaz solo suma el filtro del diálogo.

## Dependency graph

```
T0 motor (vcpkg + build) ──┬── T2 apertura y errores ── T3 --check-geoparquet ── T4 CLI
                           │                                   │
T1 fixtures ───────────────┴───────────────────────────────────┼── T6 CTest
                                                               │
T5 interfaz (filtro, ventana) ────────────────────────────────┴── T7 documentación y cierre
```

T1 puede empezar antes de que termine T0 (OSGeo4W tiene driver de escritura).
T2 y T3 son secuenciales. T5 y T6 pueden solaparse una vez T4 está estable.

## Task List

### Phase 1: Motor y datos

- [x] **T0 — Compilar GDAL con Parquet y medir la línea base**
  - Instalar `gdal[parquet]` en `D:\VCPKG\installed`.
  - Reconfigurar y compilar MiniGIS con el GDAL nuevo.
  - Verificar `--list-drivers` y abrir `09Mun`/`09Man` con el código actual
    (sin cambios): anotar `--info`, `--snapshot` y tiempos. Esto separa «el
    driver funciona» de «nuestro contrato lo mejora».
  - Criterio: `Parquet` aparece en `--list-drivers`; `--info` de `09Mun` dice
    driver `Parquet`, 16 elementos y `MEXICO_ITRF_2008_LCC`.

- [x] **T1 — Fixtures GeoParquet y script de generación**
  - `scripts/make_geoparquet_samples.bat` (+ ayudante Python si hace falta)
    genera `data/sample/geoparquet/`:
    `parcels.parquet`, `parcels_utm30.parquet`, `mixto.parquet`,
    `sin_geometria.parquet`, `sin_crs.parquet`, `tabla.parquet`,
    `vacio.parquet`, `parcels_fid.parquet`.
  - Los archivos se versionan; el script no se ejecuta en `ctest`.
  - Criterio: cada fixture abre con OSGeo4W GDAL y tiene el contenido previsto
    (elementos, geometría, SRC, FID).

### Phase 2: Apertura y diagnóstico

- [x] **T2 — Errores accionables y rechazo de lo que no es GeoParquet**
  - `openVectorDataset()`: si la extensión es `.parquet`/`.geoparquet` y
    `GDALGetDriverByName("Parquet")` es nulo, error con la acción de recompilar.
  - Rechazar carpetas (particionado no soportado) y tablas Parquet sin
    geometría (`OGR_L_GetGeomType == wkbNone` y 0 campos de geometría).
  - Señal tipada (`VectorOpenIssue`) para que la CLI distinga «sin geometría»
    (código 5) de «GDAL no abre» (código 3).
  - Criterio: `--info` de `tabla.parquet` sale 5 con «no es GeoParquet»;
    `--info` de una carpeta sale con el mensaje que pide el archivo;
    `--info` de `parcels.parquet` sigue saliendo 0.

- [x] **T3 — `--check-geoparquet` en core**
  - Nuevo `src/core/minigisgeoparquetinspect.{h,cpp}`:
    abre cada capa, cuenta elementos/partes con `countGeometryKinds`, construye
    el almacén, compara, calcula extensión nativa y 4326, lee SRC, campos, FID y
    caché de atributos; `toJson()`/`toText()`.
  - Criterio: `parcels.parquet` → `ok: true`, `storeFeatures == features`;
    `09Man.parquet` → 67 375 y `ok: true`; `sin_crs` → `crsEpsg: 0` sin fallar;
    `sin_geometria` → `withoutGeometry: 1`; `mixto` → punto, línea y polígono.

- [x] **T4 — Integración en la CLI**
  - Opción `--check-geoparquet` (con `--json`), ayuda y ejemplos.
  - Mapear errores a códigos 1/2/3/4/5; `--info` pasa a distinguir «sin
    geometría».
  - Criterio: comandos del spec §6.2; `--check-geoparquet` sin capa sale 1;
    archivo inexistente sale 2; informe no `ok` sale 4.

### Phase 3: Interfaz, pruebas y cierre

- [x] **T5 — Diálogo y comprobación en ventana**
  - Añadir `*.parquet` y `*.geoparquet` a `kOpenFilter`.
  - Verificar arrastrar y soltar, panel, tabla de atributos, identificar y
    `--snapshot-window` con `09Man`; comprobar que no se congela la ventana.
  - Criterio: captura de ventana con la capa y su tabla; tiempos de T0 no
    empeoran.

- [x] **T6 — Pruebas CTest**
  - Modos nuevos en `tests/run_cli_test.cmake` (o script propio):
    info, snapshot, check, mixto, sin geometría, sin SRC, tabla, carpeta,
    vacío, FID.
  - Criterio: `ctest --preset windows-msvc-release --output-on-failure` en
    verde, con las pruebas nuevas y sin regresiones.

- [x] **T7 — Documentación y cierre**
  - README: formato, `--check-geoparquet`, comando de instalación de la feature,
    solución de problemas («driver no disponible»), ejemplos con los archivos
    del usuario.
  - RENDIMIENTO: nota de la medición con `09Man`.
  - Actualizar `SPEC-geoparquet.md` (estado, modo vcpkg real, tiempos, CRS de
    GeoPandas) y `tasks/todo-geoparquet.md`.
  - Criterio: los 11 criterios de éxito del spec, uno por uno.

## Checkpoints

### Después de T0

- [x] `vcpkg list | findstr /i "gdal parquet arrow"` muestra GDAL con la
      feature y Arrow instalado.
- [x] `bin\minigis.exe --list-drivers` incluye `Parquet`.
- [x] `--info --json 09Mun` y `09Man` abren con el código actual; tiempos
      anotados.
- [x] `--snapshot` de `09Mun` no sale vacío (línea base, aunque falten
      rechazos).

### Después de T2–T4

- [x] `--check-geoparquet --json parcels.parquet` → `ok: true` y
      `storeFeatures: 5`.
- [x] `--check-geoparquet --json 09Man` → 67 375, `ok: true`.
- [x] `--info tabla.parquet` → 5 y mensaje «no es GeoParquet».
- [x] `--info carpeta` → mensaje que pide el archivo.

### Después de T6–T7

- [x] `ctest` completo en verde.
- [x] Criterios 1–11 del spec comprobados.
- [x] Sin diferencias en `--info --json`/`--snapshot` de shapefile, GeoPackage,
      GeoJSON y KML.

## Verification commands

```bat
:: Una sola vez, antes de configurar:
D:\VCPKG\vcpkg.exe install "gdal[parquet]" --triplet x64-windows --recurse

:: Bucle normal:
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure

:: Contrato:
bin\minigis.exe --list-drivers
bin\minigis.exe --info --json "D:\F\IRIS 4.0\2026\GeoParquet\09Mun.parquet"
bin\minigis.exe --info --json "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet"
bin\minigis.exe --check-geoparquet --json "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet"
bin\minigis.exe --snapshot manzanas.png -l "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet" --size 1600x900
bin\minigis.exe --snapshot municipios.png -l "D:\F\IRIS 4.0\2026\GeoParquet\09Mun.parquet" --size 1600x900
```

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| `vcpkg install gdal[parquet]` falla o tarda horas (Arrow 26, Boost, Thrift) | Alto: bloquea todo | Se lanza en segundo plano y se sigue con T1; el log de vcpkg queda como evidencia. Si falla, se revierte el manifiesto y se replantea con el usuario |
| GDAL reconstruido cambia features/ABI y rompe otros formatos | Alto | Misma versión 3.12.4; reconstruir MiniGIS y ejecutar `ctest` completo antes de tocar código |
| Arrow/Parquet DLLs no llegan junto al `.exe` | Medio | El script `MiniGisCopyDlls.cmake` copia todas las DLL de vcpkg; verificar `arrow.dll`, `parquet.dll`, `snappy.dll`, `thrift.dll` en `bin` |
| El driver abre `.parquet` sin geometría como capa vacía | Medio | T2 detecta `wkbNone` + 0 campos de geometría y devuelve error accionable |
| La CLI no distingue «sin geometría» de «GDAL no abre» | Medio | Señal tipada en `VectorLayer::open`; pruebas de exit 5 y 3 |
| `09Man` tarda demasiado en indexar o dibujar | Medio | Medir en T0; el almacén tiene presupuesto de vértices y *fallback* a lectura directa; presupuesto de R8 |
| CRS de GeoPandas 1.0 sigue desconocido con 3.12.4 | Bajo/Medio | Los archivos del usuario son 1.1 y funcionan; se documenta el aviso y no se asume 4326 |
| Los fixtures binarios cambian entre generaciones | Bajo | Se versionan y no se regeneran en `ctest`; el script documenta las versiones |
| El modo manifiesto no se usa en el preset | Bajo | Se documenta que la instalación es clásica; el manifiesto se actualiza igualmente |

## Open Questions

* Las del spec §13 (Q2–Q8) se resuelven con las propuestas: rechazar tabla sin
  geometría, sin `--source-crs`, `09Man` como referencia, `*.parquet` y
  `*.geoparquet`, solo lectura.
* **Q1 (vcpkg)** ya aprobada: se instala `gdal[parquet]`.
* **Modo manifiesto:** ¿se deja el preset en clásico (rápido, verificado) o se
  añade un preset `windows-msvc-release-manifest` para reproducibilidad? Se
  propone dejarlo en clásico y documentar el comando; no cambiar el preset
  verificado.
