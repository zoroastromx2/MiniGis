# Implementation Plan: mosaico de imágenes (carga masiva, progreso y navegación fluida)

> Spec de origen: [`SPEC-mosaico.md`](../SPEC-mosaico.md) · Módulo: `mosaico`
> Lista de tareas: [`tasks/todo-mosaico.md`](todo-mosaico.md)

## Overview

Convertir la carga de N imágenes (508 JPEG de 1,54 GB en el caso real) en **un
mosaico**: un objeto de datos nuevo (`RasterMosaic`) que agrupa las hojas, una
ventana de progreso cancelable que las abre en paralelo, hojas que aparecen en el
mapa por lotes, y un camino de dibujado en dos modos —**réplica piramidal en el
SRC del lienzo** para vistas gruesas y **hojas descartadas por vista con warp por
nivel** para el detalle— que baja los cuadros de 39 s a menos de 250 ms.

Las medidas que justifican la arquitectura están en `SPEC-mosaico.md` §2:
réplica 88 m + overviews = 15–19 ms por cuadro; VRT único con warp = 59–61 s.

## Architecture Decisions

1. **`RasterMosaic` no hereda de `RasterLayer`.** `RasterLayer` está atada a un
   dataset (constructor privado, `open()` estático); el mosaico es una
   *colección* de hojas con su propia revisión, estilo y extensión. Se añade una
   lista paralela en el lienzo (`mMosaics`) con un **índice de orden** que
   conserva el apilado entre rásters sueltos y mosaicos.
2. **El mosaico no dibuja hojas para lo grueso: dibuja la réplica.** La réplica
   es un GeoTIFF en el SRC del lienzo (sin warp por cuadro). El detalle se lee de
   las hojas cuando el píxel de lienzo es más fino que el de la réplica.
3. **Warp por nivel de detalle (potencias de 2), no por escala exacta.** Es la
   corrección directa de la causa medida: hoy cada paso de zoom rehace un warp
   por capa (`RasterLayer::viewForEpsg` compara `pixelSize` con `1e-9`).
4. **Descarte antes de crear el warp.** `RasterRenderer::paint` pide la vista
   antes de comprobar la intersección; el camino del mosaico comprueba la
   extensión de la hoja (en el SRC del lienzo, cacheada) primero.
5. **Una lectura entrelazada de 3 bandas por hoja y cuadro** (`GDALDatasetRasterIO`)
   en lugar de tres `GDALRasterIO`: un JPEG se decodifica una vez, no tres.
6. **La réplica se construye con GDAL en un trabajador** (`GDALCreate` +
   `GDALCreateGenImgProjTransformer2` + `GDALChunkAndWarpImage` +
   `GDALBuildOverviews`), con `pfnProgress` para el avance y la cancelación, y
   `NUM_THREADS=ALL_CPUS` (medido: 17,5 s frente a 116 s sin hilos).
7. **Caché en disco con clave y sidecar**, fuera de la carpeta de datos
   (`QStandardPaths::CacheLocation/mosaicos`), validada por ruta+tamaño+fecha,
   con LRU y tope; si no se puede escribir, réplica en `/vsimem` y aviso.
8. **La apertura en paralelo no comparte datasets entre hilos.** Cada hilo abre
   y termina su `RasterLayer`; el trabajador solo emite `shared_ptr` ya listos al
   hilo de la GUI (el hilo que después los usa para dibujar).
9. **`--layer-list` es parte del contrato, no un extra.** Con 508 `-l` el proceso
   ni arranca en Windows (límite de 32767 caracteres, comprobado), así que sin
   esa opción la puerta de rendimiento no se puede ejecutar.
10. **Todo se verifica por CLI primero** (`--check-mosaic`, 14 comprobaciones) y
    la GUI se añade encima; así cada tarea deja una prueba reproducible.

## Grafo de dependencias

```
T1 (RasterMosaic + lienzo)
 ├── T2 (CLI: --layer-list, --info, --check-mosaic 1-3)
 │    ├── T3 (renderer modo hojas: descarte, LOD, lectura entrelazada)
 │    │    ├── T4 (caché de cuadro del mosaico)
 │    │    └── T5 (réplica: clave, sidecar, LRU)
 │    │         └── T6 (construcción de la réplica: warp + overviews + progreso)
 │    │              └── T10 (modo réplica en el renderer + transición)
 │    ├── T7 (MosaicLoader en paralelo)
 │    │    └── T8 (ventana de progreso + loadPaths)
 │    │         ├── T9 (panel: grupo, hijas, lotes)
 │    │         │    └── T11 (estilo del mosaico + estadísticas a demanda)
 │    │         └── T12 (--snapshot-progress + checks 13/14)
 │    └── T13 (fixture 6 hojas + modos de run_cli_test)
 └── T14 (CTest + puerta opt-in)   ← depende de T12 y T13
T15 (README, RENDIMIENTO, spec)   ← depende de todo
```

## Fases y tareas

### Fase A — Núcleo del mosaico (sin GUI)

| # | Tarea | Tamaño |
|---|---|---|
| A1 | T1 · `RasterMosaic` + integración en `MapCanvas` | M (4 archivos) |
| A2 | T2 · CLI `--layer-list`, `--info` de mosaico, `--check-mosaic` 1–3 | M (2 archivos) |
| A3 | T3 · Renderer modo hojas: descarte, LOD, lectura entrelazada | M (3 archivos) |
| A4 | T4 · Caché de cuadro y revisiones del mosaico | S (2 archivos) |

**Checkpoint A**: `ctest --preset windows-msvc-release` verde (54 pruebas) y
`--check-mosaic` con el fixture en verde en 1–4 y 7–11. `--bench` con el mosaico
del fixture dibuja y mide.

### Fase B — Réplica piramidal y caché

| # | Tarea | Tamaño |
|---|---|---|
| B1 | T5 · `MosaicReplica`: clave, sidecar, validación, LRU, ubicación | M (3 archivos) |
| B2 | T6 · Construcción (warp + overviews), progreso, cancelación, `/vsimem` | M (3 archivos) |
| B3 | T10 · Modo réplica en el renderer y transición con las hojas | S (2 archivos) |

**Checkpoint B**: réplica de las 6 hojas del fixture construida y reutilizada
(`--check-mosaic` 5 y 12); con el conjunto real, réplica ≤ 40 s, ≈ 50 MB, y
cuadro de país ≤ 150 ms.

### Fase C — Carga y ventana

| # | Tarea | Tamaño |
|---|---|---|
| C1 | T7 · `MosaicLoader`: apertura en paralelo, progreso, errores, cancelar | M (3 archivos) |
| C2 | T8 · `LoadProgressDialog` + `MainWindow::loadPaths` con mosaico | M (4 archivos) |
| C3 | T9 · Panel: fila de mosaico, hijas, reconstrucción por lotes | M (3 archivos) |
| C4 | T11 · Estilo del mosaico y estadísticas a demanda | M (3 archivos) |
| C5 | T12 · `--snapshot-progress`, checks 6/13/14 y presupuesto | S (2 archivos) |

**Checkpoint C**: elegir los 508 archivos en la GUI: progreso por archivo, hojas
apareciendo, cancelar conserva lo cargado, carga completa ≤ 40 s y `--bench`
dentro de presupuesto.

### Fase D — Pruebas, puerta y documentación

| # | Tarea | Tamaño |
|---|---|---|
| D1 | T13 · Fixture de 6 hojas + modos nuevos de `run_cli_test.cmake` | M (3 archivos) |
| D2 | T14 · Pruebas de CTest y puerta opt-in `minigis.rendimiento-mosaico` | M (2 archivos) |
| D3 | T15 · `README.md`, `RENDIMIENTO.md` y cierre del spec | S (3 archivos) |

**Checkpoint final**: SC1–SC14 del spec, `ctest` completo en verde (54 + 5 nuevas
+ puerta omitida sin variable), y las cifras reales anotadas.

## Orden de implementación y paralelización

* Secuencial obligatorio: T1 → T2 → (T3, T7 en paralelo) → T4/T5 → T6 → T10;
  T8 → T9 → T11; T12 → T14; T13 puede empezar en cuanto T2 esté cerrado.
* Paralelizable con sitio de trabajo distinto: T3 (render) y T7 (loader) tras
  T2; T13 (fixture y modos de prueba) tras T2.
* Todo lo de la fase B comparte la clave de caché con T5: no paralelizar T5 con
  T6 en dos sesiones.

## Riesgos y mitigaciones

| Riesgo | Impacto | Mitigación |
|---|---|---|
| La réplica tarda más de 40 s en discos lentos | Alto | Medir con el conjunto real antes de cerrar B2; si se pasa, bajar niveles a 2–8 o subir el factor a 16 (menos MB, menos tiempo) y anotarlo |
| El warp con rejilla a escala de lienzo sigue leyendo el origen completo (lo que se midió en §2.2) | Alto | La réplica es el camino por defecto; el modo hojas solo se usa donde hay ≤ 40 hojas visibles, y `--check-mosaic` 14 lo vigila |
| Costuras o salto de color en la transición réplica/hojas | Medio | Check 6 (≤ 2 % de píxeles distintos, diferencia media ≤ 8); si falla, cambiar el umbral de transición a 2× antes que relajar la prueba |
| Apertura concurrente de GDAL inestable | Medio | Pool acotado (máx. 8), un dataset por hilo, `-V` con el error por hoja; si aparece, `--loader-threads 1` como respaldo documentado y la carga sigue secuencial |
| Panel con 508 hijas lento | Medio | Grupo plegado por defecto, reconstrucción por lotes, y si aun así pesa, mostrar las primeras 200 + `…` (anotado en el spec) |
| Caché que crece sin control o se invalida mal | Medio | Sidecar con tamaño+fecha por hoja, prueba de reutilización (check 12) y de invalidación (tocar una hoja), LRU con tope |
| Los 508 datasets abiertos consumen RAM | Bajo | Medir el proceso; las hojas se abren con `GDALOpenEx` (sin caché de bloques); si hiciera falta, cerrar y reabrir las hojas fuera de la vista de detalle |
| Regresión en el camino de un ráster suelto | Medio | R8 es explícito: un solo archivo no crea mosaico; las pruebas de ráster actuales (4) y `minigis.rendimiento-raster` lo vigilan |
| `--bench` ya se comportó de forma patológica con un VRT grande (64 s/cuadro) | Bajo | La puerta principal son las 4 extensiones canónicas de `--check-mosaic`; `--bench` se informa como medida secundaria |

## Decisiones tomadas sobre las preguntas abiertas del spec

El spec se aprobó sin cambios, así que se adoptan las propuestas de
`SPEC-mosaico.md` §13 y se anotan en el propio spec al cerrar:

1. Grupo con hijas en el panel, plegado por defecto con más de 50 hojas.
2. Réplica a 1/8 del píxel de origen (≈ 50 MB, ≈ 22 s con este conjunto).
3. Tope de caché de 4 GB con LRU, sin preguntar.
4. La réplica se construye siempre que haya mosaico (el encuadre inicial ya es
   una vista de país).
5. Visibilidad por hoja **fuera** de esta entrega.
6. Un mosaico por selección; varios mosaicos se apilan en orden de carga.
7. Objetivos de SC6/SC7 tal cual (§6.3 y §11 del spec).
8. `--load-sample-data` incluirá el mosaico pequeño de 6 hojas.
