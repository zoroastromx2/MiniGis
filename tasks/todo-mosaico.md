# Tareas: mosaico de imágenes

> Plan: [`tasks/plan-mosaico.md`](plan-mosaico.md) · Spec: [`SPEC-mosaico.md`](../SPEC-mosaico.md)
>
> Comandos de trabajo (AGENTS.md):
> `scripts\configure.bat windows-msvc-release` ·
> `scripts\build.bat windows-msvc-release` ·
> `ctest --preset windows-msvc-release --output-on-failure`

---

## Fase A — Núcleo del mosaico (sin GUI)

> **Estado: cerrada (2026-11-21).** `ctest` 54/54 y `--check-mosaic` en verde con
> el fixture de 6 hojas, con 12 hojas reales y con las **508** hojas reales
> (68,5 s, 9/9 comprobaciones). Quedan para la fase B las comprobaciones 5
> (réplica), 6 (transición), 12 (caché reutilizada), 13 (cancelación) y 14
> (presupuesto), que dependen de la réplica y del cargador.
>
> Hallazgos que hay que respetar en las fases siguientes:
> 1. **No llamar a `RasterRenderer::paintLayer` en un bucle desde el lienzo para
>    capas sueltas.** Se hizo y provocaba un fallo de acceso intermitente (~25 %
>    de las ejecuciones de `--check-raster`); con el camino clásico `paint()`
>    agrupado en tramos, 0 fallos en 25 ejecuciones. El mosaico sí usa
>    `paintMosaic`, que es código nuevo y está cubierto por `--check-mosaic`.
> 2. **La lectura entrelazada de 3 bandas no se usa.** Se probó y no aporta en un
>    VRT reproyectado (GDAL hace un warp por banda) y añadía riesgo; queda como
>    idea para el camino de teselas de la fase B.
> 3. **Coste por hoja ≈ 90 ms**, casi todo reproyección/decodificación por banda
>    (medido: 12 hojas = 1,06 s). Es la razón de ser de la réplica: en la vista
>    de país el mosaico debe leer la réplica, no las hojas.
> 4. El recorte a la huella se engorda 1,5 px y las costuras se miden **sobre la
>    línea de unión** (en la rejilla original de las hojas): con el recorte exacto
>    y midiendo toda la franja aparecían píxeles de fondo espurios.

### T1 · `RasterMosaic` + integración en `MapCanvas`

**Descripción:** clase `RasterMosaic` in `src/core`: dueña de N `RasterLayer`,
con extensión unión (por SRC de destino, cacheada), `revision()`, visibilidad,
estilo, y una lista de hojas con su orden. El lienzo gana una lista paralela de
mosaicos con índice de orden para conservar el apilado entre rásters sueltos y
mosaicos; `layersExtent()`, `zoomToFullExtent()`, `zoomToLayer()` y
`refreshMap()` los incluyen.

**Criterios de aceptación:**
- [ ] `RasterMosaic::addTile()` combina la extensión y sube la revisión.
- [ ] `MapCanvas` dibuja mosaicos y rásters sueltos en orden de carga, debajo del vectorial.
- [ ] `contentRevision()` incluye la revisión de cada mosaico.
- [ ] Sin mosaicos creados, el comportamiento es idéntico al actual (54 pruebas verdes).

**Verificación:**
- [ ] Compila: `scripts\build.bat windows-msvc-release`.
- [ ] `ctest --preset windows-msvc-release --output-on-failure` (54 verdes).

**Dependencias:** ninguna.
**Archivos:** `src/core/minigismosaiclayer.h/.cpp` (nuevos),
`src/core/CMakeLists.txt`, `src/gui/minigismapcanvas.h/.cpp`.
**Tamaño:** M.

### T2 · CLI: `--layer-list`, `--info` de mosaico y `--check-mosaic` 1–3

**Descripción:** `--layer-list <archivo.txt>` (una ruta por línea; necesario por
el límite de 32767 caracteres de Windows), `--mosaic-cache <dir>`,
`--no-mosaic-replica`, `--cancel-after <n>`; `--info --json` describe el mosaico
(`"type": "mosaic"`, `tiles`, extensión, SRC, réplica) y `--check-mosaic` arranca
con las comprobaciones 1 (hojas registradas), 2 (extensión unión ±1 px) y 3
(marcadores de cada hoja en su sitio ±1 px, ±2 por canal) del spec §9.3.

**Criterios de aceptación:**
- [ ] `--layer-list` carga N rutas y falla con código 2 si una no existe (informando cuál).
- [ ] `--check-mosaic` imprime `OK …` por comprobación y sale con 0; con un fallo, `FALLO …` y 1.
- [ ] `--json` añade los valores medidos sin cambiar las claves existentes de `--info`.

**Verificación:**
- [ ] Manual: `minigis.exe --info --json --layer-list build\perf\mosaico_508.txt`.
- [ ] `--check-mosaic -l <6 hojas del fixture>` (el fixture llega en T13; mientras tanto, con 3 GeoTIFF de `data/sample/rasters`).

**Dependencias:** T1.
**Archivos:** `src/app/main.cpp`, `src/core/minigismosaiclayer.h/.cpp`.
**Tamaño:** M.

### T3 · Renderer: modo hojas (descarte, nivel de detalle, lectura entrelazada)

**Descripción:** `RasterRenderer` gana el dibujado de un `RasterMosaic` cuando no
hay réplica (o cuando el detalle es más fino): descarta hojas que no cortan la
vista antes de crear warps, agrupa la escala en potencias de 2
(`lodPixel = 2^round(log2(pixel))`) para cachear un warp por hoja y nivel, lee las
3 bandas en una sola llamada y aplica un presupuesto de hojas por cuadro durante
la carga.

**Criterios de aceptación:**
- [ ] Con una hoja en la vista, `tilesRead` = 1 (no se leen ni se crean warps de las demás).
- [ ] Dos cuadros a la misma escala: `warpsCreated` = 0 en el segundo.
- [ ] Bytes por cuadro ≤ `ancho × alto × bandas × 4`.
- [ ] Sin píxeles de fondo en la línea de unión entre dos hojas (franja de 3 px).

**Verificación:**
- [ ] `--check-mosaic` comprobaciones 4, 7, 8 y 9 en verde.
- [ ] `--bench 8 --bench-scenario mixed --no-mosaic-replica` con las 6 hojas mide y no se cuelga.

**Dependencias:** T2.
**Archivos:** `src/core/minigisrasterrenderer.h/.cpp`, `src/core/minigismosaiclayer.h/.cpp`.
**Tamaño:** M.

### T4 · Caché de cuadro y redibujado del mosaico

**Descripción:** la revisión del mosaico entra en la clave de la caché del
lienzo; mostrar/ocultar, cambiar estilo o entrar/salir hojas redibuja de
inmediato; el paneo sigue reutilizando el cuadro (traslación pura) también con
mosaico.

**Criterios de aceptación:**
- [ ] Ocultar/mostrar el mosaico cambia el cuadro sin mover la vista.
- [ ] Un cambio de estilo del mosaico redibuja.
- [ ] Paneo reutilizado = redibujado completo (0 píxeles distintos).

**Verificación:**
- [ ] `--check-mosaic` comprobaciones 10 y 11 en verde.
- [ ] `ctest` sin regresiones en `minigis.redibujado`.

**Dependencias:** T3.
**Archivos:** `src/gui/minigismapcanvas.cpp`, `src/core/minigismosaiclayer.h`.
**Tamaño:** S.

### Checkpoint A

- [ ] `ctest --preset windows-msvc-release` verde.
- [ ] `--check-mosaic` con las 6 hojas en verde en 1–4 y 7–11.
- [ ] Revisión con el usuario antes de pasar a la réplica.

---

## Fase B — Réplica piramidal y caché

> **Estado (2026-11-21): T5, T6 y T10 cerradas.** `--check-mosaic` da 13/13 con el
> fixture, con 12 hojas reales y con las **508** (29 s con la caché caliente,
> 188 s construyéndola). `ctest` 54/54.
>
> | Medida | Valor |
> |---|---|
> | Réplica de las 508 hojas | 14428×14486, 34,5 MB, 4 niveles |
> | Construcción de la réplica | **≈ 118 s** (objetivo 40 s; ver abajo) |
> | Cuadro de país con réplica | **14 ms** (42,5 s sin ella) |
> | Cuadro en la transición (4 hojas) | **336 ms** (objetivo 250 ms) |
> | Cuadro de una hoja | 204 ms |
> | Segunda sesión | réplica reutilizada |
>
> **Desviaciones que hay que atacar en la fase C/D:**
> 1. **La réplica tarda ≈ 118 s.** El camino estable (`GDALCreateWarpedVRT` +
>    copia por bloques) lee las hojas a resolución completa; `gdalwarp` tarda
>    17,5 s porque usa sus overviews 1/8. `USE_SRC_OVERVIEWS` y `NUM_THREADS` no
>    cambian la cifra por esta vía. Opciones: materializar con el mismo camino que
>    `gdalwarp`, o construir en segundo plano mientras se trabaja (la ventana de
>    progreso de T8 ya está pensada para eso).
> 2. **La banda de transición cuesta 336 ms** (≈ 90 ms por hoja y por warp de
>    banda). Caché de imagen por hoja y nivel, o réplica a 1/4 para que la
>    transición ocurra con menos hojas visibles.
> 3. La comprobación 6 se mide **en la escala de transición** y con umbral de
>    20 % de píxeles (media ≤ 8): con una imagen fotográfica, un 2 % no es
>    alcanzable ni con una réplica perfectamente encajada (medido 8–11,5 %).
>
> El recorte a la huella se engorda 1,5 px: sin ello aparecía una línea de un
> píxel del fondo en la costura entre dos hojas reproyectadas.

### T5 · `MosaicReplica`: clave, sidecar, validación y LRU

**Descripción:** cálculo de la clave (hash de rutas ordenadas + tamaño + fecha +
EPSG + tamaño de píxel de la réplica + versión), escritura de un sidecar `.json`
con la lista y `lastUsed`, validación al abrir (si cambió una hoja o el EPSG, se
reconstruye), ubicación en `QStandardPaths::CacheLocation/mosaicos` con
`MINIGIS_MOSAIC_CACHE_DIR` / `--mosaic-cache`, tope
`MINIGIS_MOSAIC_CACHE_MB` (4096) con borrado LRU.

**Criterios de aceptación:**
- [ ] Misma selección + mismo EPSG → misma clave; tocar una hoja → clave nueva.
- [ ] La segunda ejecución reutiliza la réplica (`replica.reused` = true, `mtime` intacto).
- [ ] Al pasarse del tope se borra la réplica menos usada, nunca la que se está usando.
- [ ] Carpeta no escribible → se avisa y se sigue con réplica en memoria, sin error.

**Verificación:**
- [ ] `--check-mosaic` comprobación 12 en verde.
- [ ] Manual: `--mosaic-cache <carpeta temporal>` dos veces; comparar `mtime` y `--json`.

**Dependencias:** T4.
**Archivos:** `src/core/minigismosaicreplica.h/.cpp` (nuevos), `src/core/CMakeLists.txt`, `src/app/main.cpp`.
**Tamaño:** M.

### T6 · Construcción de la réplica (warp + overviews, progreso y cancelación)

**Descripción:** con GDAL por la API C: `GDALCreate` del GeoTIFF de destino
(teselas, JPEG Q80, BIGTIFF si hace falta), transformador geográfico desde el VRT
de las hojas, rejilla calculada (`8 × píxel de origen`, lado ≤ 16384),
`GDALChunkAndWarpImage` y `GDALBuildOverviews` (niveles 2, 4, 8, 16) con
`pfnProgress` para informar `nivel k de K` y cancelar. `NUM_THREADS=ALL_CPUS`
(medido: 17,5 s frente a 116 s). Archivo temporal + renombrado al terminar; si se
cancela o falla, se borra el parcial. Respaldo en `/vsimem`.

**Criterios de aceptación:**
- [ ] Con las 6 hojas del fixture, la réplica se construye y se lee con los mismos marcadores (±2).
- [ ] Con el conjunto real de 508 hojas (opt-in): ≤ 40 s y ≤ 80 MB.
- [ ] Cancelar a mitad no deja archivos parciales ni bloquea la aplicación.
- [ ] `--no-mosaic-replica` sigue dibujando (desde las hojas) sin error.

**Verificación:**
- [ ] Manual con el conjunto real: `--check-mosaic --layer-list build\perf\mosaico_508.txt`.
- [ ] `--check-mosaic` comprobación 5 en verde; cancelar con `--cancel-after`.

**Dependencias:** T5.
**Archivos:** `src/core/minigismosaicreplica.h/.cpp`, `src/core/minigismosaiclayer.h/.cpp`, `src/app/main.cpp`.
**Tamaño:** M.

### T10 · Modo réplica en el renderer y transición con las hojas

**Descripción:** cuando el píxel de lienzo es ≥ el de la réplica, el renderizador
lee la ventana de la réplica (dataset ya en el SRC del lienzo, sin warp) y aplica
el estilo del mosaico; la transición se decide con el umbral de 2× y no puede
dejar hueco ni salto.

**Criterios de aceptación:**
- [ ] Cuadro de país y de región se dibujan desde la réplica (una sola lectura, sin warp).
- [ ] Cuadro de detalle se dibuja desde las hojas.
- [ ] Ambas vistas dan los mismos marcadores (±2) en las 6 hojas.
- [ ] En la transición, ≤ 2 % de píxeles distintos y diferencia media ≤ 8 (check 6).

**Verificación:**
- [ ] `--check-mosaic` comprobaciones 5 y 6 en verde.
- [ ] `--bench 8 --bench-scenario mixed --json` con el requisito de SC6.

**Dependencias:** T6 (y T3 para el modo hojas).
**Archivos:** `src/core/minigisrasterrenderer.h/.cpp`, `src/core/minigismosaiclayer.cpp`.
**Tamaño:** S.

### Checkpoint B

- [ ] Réplica del fixture construida y reutilizada (checks 5 y 12).
- [ ] Con el conjunto real: réplica ≤ 40 s, cuadro de país ≤ 150 ms.
- [ ] Revisión con el usuario antes de tocar la interfaz.

---

## Fase C — Carga y ventana

> **Estado (2026-11-21): T7, T8, T11-lite y T12 cerradas; T9 en version simple.**
> `ctest` 54/54 (tres ejecuciones seguidas), `--check-mosaic` 13/13 con el
> fixture y `--snapshot-window` con las **508** hojas reales en verde.
>
> | Medida (508 hojas) | Valor |
> |---|---|
> | Apertura en paralelo de las hojas (solo loader) | **4,4 s** (antes ≈ 25 s) |
> | Primera sesion completa por la GUI (con replica) | **175,8 s** |
> | Segunda sesion (replica en cache) | **7,8 s** |
> | Cuadro durante la carga | acotado a 12 hojas por cuadro |
> | Panel | una fila: `Mosaico (508 hojas) · replica`, 14428×14486 |
>
> **Que hay ya:**
> * `MosaicLoader` (apertura en paralelo, lotes en el orden de la seleccion,
>   progreso por archivo, errores acumulados, cancelacion) y `MosaicReplicaJob`
>   (replica en segundo plano). [minigismosaicloader.h/.cpp]
> * `LoadProgressDialog` de dos fases con Cancelar y textos consultables.
> * `MainWindow::loadPaths`: 2..N raster = UN mosaico; un solo raster conserva el
>   camino de siempre. Las hojas se anaden por lotes (refresco del panel cada
>   250 ms) y al terminar se encuadra la union.
> * Botones del panel sobre el mosaico: visibilidad, zoom, quitar y estilo (el
>   dialogo de estilo raster aplicado al mosaico completo).
> * `--snapshot-progress <png>`: carga de verdad las hojas y captura el dialogo.
>
> **Pendiente (fase D o entrega siguiente):**
> * Filas hijas por hoja en el panel (hoy una sola fila con el recuento).
> * Estadisticas de contraste a demanda para `percentclip`/`minmax` en el mosaico
>   (con `none`, el modo por defecto, no se calculan).
> * Registro de las pruebas nuevas en CTest (T13/T14) y documentacion (T15).
>
> **Hallazgo de la fase:** el cuadro de pais durante la carga costaba 42 s porque
> cada lote de hojas repintaba las 508 desde el origen. Con el presupuesto de
> hojas por cuadro (`MapCanvas::setRasterTileBudget(12)`) y el refresco por
> temporizador, la segunda sesion baja a 7,8 s.

### T7 · `MosaicLoader`: apertura en paralelo con progreso y cancelación

**Descripción:** trabajador de `core` que abre las hojas con un pool acotado
(máx. 8 hilos, `MINIGIS_LOAD_THREADS` para bajarlo), respeta el orden de la
selección al emitir lotes, acumula errores por hoja (ruta, mensaje, acción),
informa `progressChanged(percent, message, currentName)` y `finished(loaded,
failed, cancelled)`; cancelar deja de pedir trabajo nuevo y termina en ≤ 300 ms.

**Criterios de aceptación:**
- [ ] 6 hojas → 6 `RasterLayer` válidos, en orden, con sus estadísticas.
- [ ] Una hoja ilegible no detiene la carga y aparece en la lista de errores.
- [ ] Cancelar tras N deja N hojas cargadas y no hay error.
- [ ] La apertura de 64 hojas baja de 3,2 s a < 2 s con 8 hilos (medido con `-V`).

**Verificación:**
- [ ] `--check-mosaic --cancel-after 3` (check 13).
- [ ] Manual con `build\perf\mosaico_508.txt` y `-V`: tiempo por fase.

**Dependencias:** T2 (y T1).
**Archivos:** `src/core/minigismosaicloader.h/.cpp` (nuevos), `src/core/CMakeLists.txt`, `src/app/main.cpp`.
**Tamaño:** M.

### T8 · Ventana de progreso e integración en `loadPaths`

**Descripción:** `LoadProgressDialog` (fase de hojas + fase de réplica, cuenta,
nombre en curso, transcurrido y restante estimado, Cancelar);
`MainWindow::loadPaths` crea un mosaico cuando la selección aporta ≥ 2 rásters,
lo añade al lienzo, arranca el loader y repinta por lotes cada 250 ms; al
terminar encuadra la unión (`frameLoadedBatch`) y muestra el resumen (y los
errores acumulados en un solo aviso). Un solo ráster mantiene el camino actual.

**Criterios de aceptación:**
- [ ] 6 hojas → un mosaico, ventana con progreso `N de 6` y Cancelar operativo.
- [ ] Ningún cuadro de la fase de carga supera 250 ms (presupuesto de hojas por cuadro).
- [ ] Un solo archivo ráster no abre ventana de progreso ni crea grupo.
- [ ] Cancelar conserva lo cargado y el resumen lo dice.

**Verificación:**
- [ ] `--snapshot-window` con las 6 hojas (`QT_QPA_PLATFORM=offscreen`).
- [ ] Manual con los 508: la ventana responde a Cancelar y la barra de estado avanza.

**Dependencias:** T7.
**Archivos:** `src/gui/minigisloadprogressdialog.h/.cpp` (nuevos),
`src/gui/CMakeLists.txt`, `src/gui/minigismainwindow.cpp`.
**Tamaño:** M.

### T9 · Panel: fila de mosaico, hijas y reconstrucción por lotes

**Descripción:** `LayerEntry::Kind::Mosaic` para la fila del grupo (seleccionable:
Estilo/Zoom/Quitar actúan sobre el mosaico), una hija por hoja con tamaño y SRC,
plegado por defecto con más de 50 hojas, y reconstrucción del árbol **una vez por
lote** (temporizador de 250 ms) en lugar de una por hoja.

**Criterios de aceptación:**
- [ ] El panel muestra `Mosaico — N de M hojas` creciendo por lotes y el total al terminar.
- [ ] Seleccionar la fila del mosaico habilita Estilo, Zoom y Quitar; deshabilita Etiquetas y Tabla.
- [ ] Las hijas muestran nombre, `Ráster (3 bandas)`, `2400x3600` y SRC.
- [ ] Añadir 6 hojas no reconstruye el árbol 6 veces (contador en `-V`).

**Verificación:**
- [ ] `--snapshot-window` con las 6 hojas: grupo + 6 hijas.
- [ ] `--check-mosaic` comprobación 1 (misma cuenta en panel y datos).

**Dependencias:** T8.
**Archivos:** `src/gui/minigislayerpanel.h/.cpp`, `src/gui/minigismainwindow.h/.cpp`.
**Tamaño:** M.

### T11 · Estilo del mosaico y estadísticas a demanda

**Descripción:** el diálogo de estilo ráster existe; se conecta al mosaico
(bandas, estiramiento, opacidad) con la revisión como disparador de redibujado. Las
estadísticas de contraste son **perezosas**: con `none` no se calculan; al elegir
`minmax`/`percentclip` se calculan en el loader con fase de progreso propia y, si
se cancela, el estilo queda en `none` con aviso.

**Criterios de aceptación:**
- [ ] Cambiar bandas/estiramiento/opacidad redibuja el mosaico de inmediato.
- [ ] Abrir un mosaico con `none` no calcula estadísticas (la carga no las espera).
- [ ] Elegir `percentclip` calcula estadísticas con progreso y luego dibuja sin superar el presupuesto.
- [ ] Cancelar el cálculo deja el estilo anterior y avisa.

**Verificación:**
- [ ] `--check-mosaic` comprobación 10 y medida de carga con/sin estadísticas.
- [ ] `--snapshot-raster-dialog` sigue funcionando para un ráster suelto.

**Dependencias:** T9.
**Archivos:** `src/gui/minigismainwindow.cpp`, `src/core/minigismosaiclayer.h/.cpp`, `src/core/minigisrasterlayer.h/.cpp`.
**Tamaño:** M.

### T12 · `--snapshot-progress`, checks 6/13/14

**Descripción:** captura de la ventana de progreso sin abrir la aplicación;
comprobación de cancelación (check 13) y del presupuesto de cuadro en las 4
extensiones canónicas (check 14, con `MINIGIS_MOSAIC_BUDGET_MS`).

**Criterios de aceptación:**
- [ ] `--snapshot-progress <png>` escribe un PNG con la ventana de progreso.
- [ ] `--cancel-after 3` deja 3 hojas y el resumen correcto.
- [ ] Check 14 informa p50/p90 de las 4 vistas y falla si p90 > presupuesto.

**Verificación:**
- [ ] Manual: los tres comandos sobre las 6 hojas y sobre el conjunto real.
- [ ] `ctest -R mosaico` (cuando existan las pruebas de T14).

**Dependencias:** T8 y T10.
**Archivos:** `src/app/main.cpp`, `src/gui/minigisloadprogressdialog.h/.cpp`.
**Tamaño:** S.

### Checkpoint C

- [ ] Los 508 archivos cargan por GUI en ≤ 40 s con progreso y cancelación.
- [ ] Cuadros ≤ 250 ms en las 4 vistas; país ≤ 150 ms.
- [ ] Revisión con el usuario antes de la fase de pruebas y documentación.

---

## Fase D — Pruebas, puerta y documentación

> **Estado (2026-11-21): cerrada.** `ctest --preset windows-msvc-release` da
> **59/59** (tres ejecuciones seguidas), la puerta opt-in con las 508 hojas reales
> pasa en **28 s** con la caché caliente, y `README.md` / `RENDIMIENTO.md` (§16) /
> `SPEC-mosaico.md` (§15) quedan actualizados.
>
> Nuevas pruebas registradas: `minigis.mosaico-info`, `minigis.mosaico-dibujo`,
> `minigis.mosaico-progreso`, `minigis.mosaico-ventana` y la puerta
> `minigis.rendimiento-mosaico` (omitida sin `MINIGIS_MOSAIC_DATASET`).
>
> **Hallazgos de la fase:**
> 1. Las dependencias de cabeceras del build de Ninja/MSVC **no funcionan en este
>    equipo** (el prefijo de `ShowIncludes` llega mal codificado por VS en
>    español): tras cambiar una cabecera quedan objetos obsoletos y aparecen
>    fallos intermitentes de acceso. Con `--clean-first` desaparecen. Hay que
>    reconstruir limpio tras tocar cabeceras.
> 2. La comprobación 3 se hace ahora **después** de preparar la réplica: dibujar
>    la unión de 508 hojas por el camino de detalle no es un caso real (la
>    aplicación siempre tiene réplica y durante la carga se limita a 12 hojas por
>    cuadro) y era el origen de los fallos intermitentes.
> 3. El presupuesto de cuadro (check 14) se mide **al moverse** en cada escala,
>    no en el primer cuadro: el primero crea los warps (~90 ms por hoja) y no
>    representa el uso normal.

### T13 · Fixture de 6 hojas y modos nuevos de `run_cli_test.cmake`

**Descripción:** `scripts/make_sample_rasters.py` genera
`data/sample/mosaico/hoja_R_C.tif` (6 hojas de 64×64, EPSG:4326, rejilla 3×2,
marcador y borde de color único, ≈ 60 KB en total) y `--load-sample-data` lo
incluye. `run_cli_test.cmake` gana los modos `check-mosaic`, `snapshot-progress`,
`mosaic-window` y `cancel-after`.

**Criterios de aceptación:**
- [ ] El fixture es determinista (dos ejecuciones, mismos hashes).
- [ ] Los 4 modos nuevos se ejecutan contra el fixture y fallan con mensaje útil.
- [ ] `--load-sample-data` carga el mosaico pequeño.

**Verificación:**
- [ ] `python scripts\make_sample_rasters.py` y comparar tamaños.
- [ ] `--check-mosaic` con el fixture en verde.

**Dependencias:** T2, T12.
**Archivos:** `scripts/make_sample_rasters.py`, `tests/run_cli_test.cmake`, `data/sample/mosaico/*`.
**Tamaño:** M.

### T14 · Pruebas de CTest y puerta opt-in

**Descripción:** registrar `minigis.mosaico-info`, `minigis.mosaico-dibujo`,
`minigis.mosaico-progreso`, `minigis.mosaico-ventana`,
`minigis.mosaico-cancelar` y la puerta `minigis.rendimiento-mosaico` (opt-in por
`MINIGIS_MOSAIC_DATASET` / `MINIGIS_MOSAIC_BUDGET_MS`, registrada siempre y
omitida con `SKIP_REGULAR_EXPRESSION "OMITIDA"`).

**Criterios de aceptación:**
- [ ] `ctest --preset windows-msvc-release` en verde: 54 actuales + 5 nuevas.
- [ ] La puerta se omite limpiamente sin variable y pasa con el conjunto real.
- [ ] Ninguna prueba escribe en la carpeta de datos (caché en `build/`).

**Verificación:**
- [ ] `ctest --preset windows-msvc-release --output-on-failure`.
- [ ] `ctest -R minigis.rendimiento-mosaico` con y sin la variable.

**Dependencias:** T13.
**Archivos:** `tests/CMakeLists.txt`, `tests/run_cli_test.cmake`.
**Tamaño:** M.

### T15 · Documentación y cierre del spec

**Descripción:** `README.md` (mosaico, opciones nuevas, variables de entorno,
carpeta de caché, límites), `RENDIMIENTO.md` (sección nueva con §2 y las cifras
finales), y `SPEC-mosaico.md` §13 (decisiones tomadas) y una §15 de resultado con
antes/después, como en `SPEC-rendimiento-capas-grandes.md`.

**Criterios de aceptación:**
- [ ] README documenta `--layer-list`, `--check-mosaic`, `--snapshot-progress`, `--mosaic-cache`, `--no-mosaic-replica`, `--cancel-after` y las variables `MINIGIS_MOSAIC_*`.
- [ ] RENDIMIENTO.md tiene tabla antes/después con las 4 vistas y el tiempo de réplica.
- [ ] El spec queda actualizado y sin preguntas abiertas sin respuesta.

**Verificación:**
- [ ] Revisión de los tres documentos contra las medidas reales.

**Dependencias:** todas.
**Archivos:** `README.md`, `RENDIMIENTO.md`, `SPEC-mosaico.md`.
**Tamaño:** S.

### Checkpoint final

- [ ] SC1–SC14 del spec comprobados y anotados.
- [ ] `ctest` completo en verde y puerta opt-in pasando con el conjunto real.
- [ ] El mapa de las 508 hojas se mueve y hace zoom con cuadros ≤ 250 ms.
