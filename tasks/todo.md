# Task List: capa ráster

> Derivado de [plan.md](plan.md) y [SPEC-raster.md](../SPEC-raster.md).
> Cada tarea se completa con: compila, pruebas verdes y verificación indicada.
> Estado: **pendiente de aprobación** — no se empieza hasta que se revisen plan
> y lista.

## Comandos de referencia

Con el entorno alineado (T1):

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

Si T1 no se aprueba, se usa la configuración manual del spec §6.1 con
`build\raster` y `ctest --test-dir build\raster --output-on-failure`.

Ejecutable: `build\windows-msvc-release\bin\minigis.exe` (o `build\raster\bin\minigis.exe`).

---

## Fase A — Fundación

### Checkpoint 0 — antes de tocar código

- [ ] Plan y lista de tareas revisados por una persona
- [ ] Decidido si T1 entra o no (rutas del entorno)

### [x] T1 — Alinear presets, `env.bat` y README con el entorno real

**Descripción:** `CMakePresets.json` apunta a `D:/QtWin/6.10.3` y `E:/vcpkg`,
que no existen. Se actualizan las rutas a `D:/Qt/6.8.3/msvc2022_64` y
`D:/VCPKG`, se fija `rc.exe`/`mt.exe` de 10.0.26100.0 (el SDK 10.0.28000 rompe
el enlace) y se añade un preset de prueba para Release. `scripts\env.bat` deja
de exigir rutas inexistentes y comprueba MSVC, Qt y GDAL con mensajes útiles.

**Criterios de aceptación:**
- [ ] `scripts\configure.bat windows-msvc-release` configura sin tocar nada a mano
- [ ] `scripts\build.bat windows-msvc-release` compila y despliega DLLs/plugins
- [ ] `ctest --preset windows-msvc-release` ejecuta y pasa las 9 pruebas
- [ ] El README refleja las versiones/rutas reales (Qt 6.8.3, vcpkg `D:/VCPKG`)

**Verificación:**
- [ ] Los tres comandos de arriba, desde una consola limpia de VS 2022
- [ ] Configurar también desde Qt Creator con el kit MSVC y compilar

**Dependencias:** ninguna · **Archivos:** `CMakePresets.json`, `scripts/env.bat`,
`README.md` · **Tamaño:** S

---

### [x] T2 — Generador determinista de rásters de ejemplo

**Descripción:** `scripts/make_sample_rasters.py` (OSGeo4W Python + GDAL +
numpy) crea los tres GeoTIFF de prueba y, con `--big <dir>`, el de 4096² con
overviews. Patrones fijos, sin aleatoriedad. `scripts\make_sample_data.bat`
llama al generador si encuentra Python con GDAL (avisa y sigue si no).

**Criterios de aceptación:**
- [ ] `ortho_rgb.tif`: 256×256, 3 bandas Byte, EPSG:4326, extensión
      `[-4.6, 39.8, -3.0, 41.2]`, 4 cuadrantes planos (rojo, verde, azul, blanco)
- [ ] `dem_utm30.tif`: 256×256, 1 banda Int16, EPSG:25830, extensión
      `[430000, 4470000, 431000, 4471000]`, valores `100+(fila+columna)%11`,
      píxel `(10,10)=1000`, NoData `-9999` en un bloque 16×16
- [ ] `landcover.tif`: 128×128 Byte con paleta (3 clases + NoData), EPSG:4326
- [ ] Dos ejecuciones producen archivos idénticos (mismo SHA-256)
- [ ] `--big build\perf` genera `terrain_big.tif` 4096² con overviews 2–16

**Verificación:**
- [ ] `"D:\OSGeo4w\apps\Python312\python.exe" scripts\make_sample_rasters.py`
- [ ] `"D:\OSGeo4w\apps\Python312\python.exe" scripts\make_sample_rasters.py --big build\perf`
- [ ] `gdalinfo` de cada archivo (tamaño, bandas, SRC, NoData, paleta)
- [ ] Comparar hashes de dos ejecuciones

**Dependencias:** ninguna · **Archivos:** `scripts/make_sample_rasters.py`,
`scripts/make_sample_data.bat`, `data/sample/rasters/*.tif` · **Tamaño:** S

---

## Fase B — Camino mínimo (abrir, informar, dibujar)

### [x] T3 — `RasterStyle`, `RasterLayer`, detección y `--info`

**Descripción:** modelo de capa ráster sobre GDAL: apertura con
`GDALOpenEx(RASTER|READONLY)`, metadatos, extensión nativa y
`extentInEpsg()`, revisión (estilo/visibilidad) y RAII del dataset. Se añade la
detección por contenido (vector si hay capas, si no ráster) y `--info`
texto/JSON para rásters.

**Criterios de aceptación:**
- [ ] `--info --json -l ortho_rgb.tif` devuelve `"type": "raster"`, 256×256,
      3 bandas Byte, `EPSG:4326`, extensión y nombres de banda
- [ ] `--info -l dem_utm30.tif` muestra `Int16`, NoData `-9999`, `EPSG:25830`
- [ ] `--info -l landcover.tif` informa de la tabla de color
- [ ] `extentInEpsg(4326)` del DEM coincide con `gdaltransform` (±1e-6°)
- [ ] Un archivo inexistente sale con 2 y un archivo ilegible con 3 y `Error` útil
- [ ] `--info` de `cities.shp` sigue devolviendo lo de hoy (sin cambios)

**Verificación:**
- [ ] Los comandos `--info` de arriba, texto y JSON
- [ ] `git diff` de `main.cpp` limitado a `--info` y detección

**Dependencias:** T2 · **Archivos:** `src/core/minigisrasterstyle.h`,
`src/core/minigisrasterlayer.h/.cpp`, `src/core/CMakeLists.txt`, `src/app/main.cpp`
· **Tamaño:** M

---

### [x] T4 — `RasterRenderer` mínimo

**Descripción:** dibujado de un ráster en el mismo SRC: rectángulo de pantalla a
partir de la extensión, lectura por ventana con `GDALRasterIO` al tamaño de
salida, composición ARGB (Byte RGB), NoData y banda alfa, sin estiramiento ni
warp todavía. Cuenta bytes leídos y píxeles dibujados.

**Criterios de aceptación:**
- [ ] Compone una imagen ARGB correcta para RGB (canales en su sitio)
- [ ] NoData y alfa salen transparentes; no se lee fuera de la ventana visible
- [ ] El renderizador no depende de widgets (solo `QPainter`/`QImage`)
- [ ] Estadísticas del cuadro: `bytesRead`, `pixelsDrawn`, `layers`

**Verificación:**
- [ ] Compila (`scripts\build.bat windows-msvc-release`)
- [x] Verificado con la captura de los cuadrantes en T5

**Dependencias:** T3 · **Archivos:** `src/core/minigisrasterrenderer.h/.cpp`,
`src/core/CMakeLists.txt` · **Tamaño:** M

---

### [x] T5 — Lienzo y `--snapshot` con rásters

**Descripción:** `MapCanvas` gana la lista de rásters, los dibuja antes del
vectorial, los incluye en `layersExtent()`, `zoomToFullExtent()`,
`zoomToLayer()` y en la revisión de contenido de la caché. `--snapshot` carga
rásters y vectoriales juntos.

**Criterios de aceptación:**
- [ ] `--snapshot` con `ortho_rgb.tif` dibuja los 4 cuadrantes en su sitio
      (color ±2 y posición ±1 px)
- [ ] Ráster debajo de vectorial: `parcels.shp` encima de la ortofoto
- [ ] Sin capas → código 5; el resto de códigos de salida no cambia
- [ ] Las 9 pruebas existentes siguen pasando

**Verificación:**
- [ ] `minigis.exe --snapshot out.png -l data\sample\rasters\ortho_rgb.tif --size 900x600`
- [ ] Comprobar píxeles del PNG con un script corto (Python/PowerShell)
- [ ] `ctest --preset windows-msvc-release`

**Dependencias:** T4 · **Archivos:** `src/gui/minigismapcanvas.h/.cpp`,
`src/app/main.cpp` · **Tamaño:** S

---

### Checkpoint A — camino mínimo

- [x] Compila y 9/9 pruebas verdes
- [x] `--info` describe los tres rásters de ejemplo
- [x] `--snapshot` enseña la ortofoto con los cuadrantes correctos, debajo de
      una capa vectorial
- [ ] Revisión humana antes de la reproyección

---

## Fase C — Reproyección, NoData y contraste

### [x] T6 — Warp, extensión densificada, una banda y NoData

**Descripción:** si el SRC no coincide, se crea y cachea un
`GDALCreateWarpedVRT` por capa y SRC de destino; la lectura por ventana se hace
sobre el VRT. `extentInEpsg()` transforma puntos intermedios de cada borde. Un
ráster de una banda se dibuja en grises; NoData y alfa se aplican también tras
el warp.

**Criterios de aceptación:**
- [ ] `--snapshot --crs EPSG:4326 -l dem_utm30.tif` coloca el DEM en su sitio
      (esquina con error ≤ 1,5 px respecto a `gdaltransform`)
- [ ] `--snapshot --crs EPSG:25830 -l ortho_rgb.tif` funciona en sentido inverso
- [ ] El bloque NoData del DEM es exactamente del color de fondo del lienzo
- [ ] El VRT se reutiliza entre cuadros (no se recrea por cuadro; contar
      creaciones en las trazas `-V`)
- [ ] Sin overviews el dibujo sigue siendo correcto (solo más lento)

**Verificación:**
- [ ] Los dos `--snapshot` de arriba y comprobación de píxeles
- [ ] `-V` muestra una sola creación del warp por capa y SRC
- [ ] `ctest --preset windows-msvc-release`

**Dependencias:** T5 · **Archivos:** `src/core/minigisrasterlayer.h/.cpp`,
`src/core/minigisrasterrenderer.cpp` · **Tamaño:** M

---

### [x] T7 — Estiramiento y paleta

**Descripción:** estadísticas por banda de una lectura decimada (≤512×512) al
abrir; modos `percentclip` (2–98 por defecto), `minmax` y `none`; LUT de 256
entradas por banda; rásters con tabla de color usan su paleta. El estilo
incluye el modo y los parámetros.

**Criterios de aceptación:**
- [ ] `percentclip` y `minmax` con el píxel 1000: rango ≥ 200 frente a ≤ 10
- [ ] La paleta de `landcover` sale exacta (3/3 clases)
- [ ] Cambiar de modo de estiramiento cambia el dibujado y sube la revisión
- [ ] Las estadísticas se calculan una vez al abrir (no por cuadro; `-V`)
- [ ] NoData no participa en los percentiles

**Verificación:**
- [ ] Dos `--snapshot` del DEM con `--raster-stretch percentclip|minmax` y
      comparación de rangos de gris
- [ ] `--snapshot` de `landcover.tif` y comprobación de los 3 colores
- [ ] `ctest --preset windows-msvc-release`

**Dependencias:** T6 · **Archivos:** `src/core/minigisrasterstyle.h`,
`src/core/minigisrasterlayer.cpp`, `src/core/minigisrasterrenderer.h/.cpp`
· **Tamaño:** M

---

### Checkpoint B — reproyección y contraste

- [x] DEM UTM encuadrado sobre lienzo 4326 y ortofoto sobre 25830
- [x] NoData transparente y paleta respetada
- [x] Percentiles vs min-máx claramente distintos
- [ ] Revisión humana antes de la interfaz

---

## Fase D — Interfaz

### [x] T8 — Panel de capas, apertura y acciones

**Descripción:** `LayerPanel` pasa a recibir `LayerEntry` (tipo + puntero
vector/ráster + textos) y muestra las dos clases en una lista. `MainWindow`
clasifica las rutas (vector → cargador asíncrono; ráster → apertura síncrona),
añade el ráster al lienzo, actualiza panel/título, habilita Quitar/Zoom/Estilo y
deshabilita Etiquetas/Tabla para rásters. Diálogo de apertura, recientes y
arrastrar y soltar aceptan formatos ráster.

**Criterios de aceptación:**
- [ ] `--snapshot-window` con ráster + vector muestra la fila del ráster
      (`Ráster (3 bandas)`, `256×256`, SRC) y el ráster en el lienzo
- [ ] Visibilidad, quitar y zoom funcionan sobre el ráster
- [ ] Etiquetas y Tabla quedan deshabilitadas con un ráster seleccionado
- [ ] Los flujos vectoriales no cambian (9 pruebas verdes)

**Verificación:**
- [ ] `minigis.exe --snapshot-window v.png -l ortho_rgb.tif -l parcels.shp`
- [ ] Abrir la aplicación y probar visibilidad/quitar/zoom/recientes/arrastrar
- [ ] `ctest --preset windows-msvc-release`

**Dependencias:** T5 · **Archivos:** `src/gui/minigislayerpanel.h/.cpp`,
`src/gui/minigismainwindow.h/.cpp`, `src/gui/minigismapcanvas.cpp` · **Tamaño:** M

---

### [x] T9 — `RasterStyleDialog` y acciones de estilo

**Descripción:** diálogo modal con bandas R/G/B y alfa, modo de estiramiento,
mín/máx y percentiles según el modo, resampling y opacidad. Se abre desde
**Estilo** (botón del panel y menú) cuando la capa actual es ráster; al aceptar,
sube la revisión y redibuja. Sigue el patrón de `StyleDialog` (sin vista previa,
con *Restablecer*).

**Criterios de aceptación:**
- [ ] El diálogo se abre con los valores actuales y los aplica al aceptar
- [ ] Los controles de mín/máx y percentiles se habilitan según el modo
- [ ] Cambiar opacidad o estiramiento se ve en el mapa sin mover la vista
- [ ] Cancelar no modifica la capa

**Verificación:**
- [ ] Abrir la aplicación, cargar `dem_utm30.tif`, probar los tres modos
- [ ] `ctest --preset windows-msvc-release`

**Dependencias:** T8 · **Archivos:** `src/gui/minigisrasterstyledialog.h/.cpp`,
`src/gui/CMakeLists.txt`, `src/gui/minigismainwindow.h/.cpp` · **Tamaño:** M

---

### Checkpoint C — interfaz

- [x] Panel, zoom y diálogo de estilo funcionan con ráster (visibilidad y redibujado se verifican en T10)
- [x] Vectorial sin regresiones (9/9 pruebas y captura con raster + parcelas)
- [ ] Revisión humana antes del contrato de pruebas

---

## Fase E — Contrato y rendimiento

### [x] T10 — `--check-raster` y captura del diálogo

**Descripción:** modo de diagnóstico sin ventana con las 9 comprobaciones del
spec §9.3 (cuadrantes, extensión, warp, NoData, estiramiento, paleta,
redibujado, paneo reutilizado, lectura acotada) y salida `--json` con los
valores medidos y `--snapshot-raster-dialog`. Las opciones `--raster-stretch`, `--raster-bands` y `--raster-opacity` ya se implementaron en T7.

**Criterios de aceptación:**
- [ ] `--check-raster` con las tres capas imprime una línea `OK` por
      comprobación y ninguna `FALLO`
- [ ] Con un fallo inducido (p. ej. extensión mal puesta) imprime `FALLO` y
      sale con código 1
- [ ] `--json` devuelve los valores (errores en px, niveles, bytes, píxeles)
- [ ] `--snapshot-raster-dialog` escribe el PNG del diálogo
- [ ] Las opciones `--raster-*` inválidas salen con 1

**Verificación:**
- [ ] `minigis.exe --check-raster -l ortho_rgb.tif -l dem_utm30.tif -l landcover.tif`
- [ ] `minigis.exe --check-raster --json ... | jq` (o inspección)
- [ ] `minigis.exe --snapshot-raster-dialog d.png -l dem_utm30.tif`

**Dependencias:** T7, T8, T9 · **Archivos:** `src/app/main.cpp`,
`src/core/minigisrasterrenderer.h/.cpp` · **Tamaño:** M

---

### [x] T11 — Pruebas de CTest y puerta de rendimiento

**Descripción:** modos nuevos en `tests/run_cli_test.cmake`
(`raster-info`, `check-raster`, `raster-dialog`, `snapshot-window`) y sus
pruebas en `tests/CMakeLists.txt`. Puerta opcional con
`MINIGIS_RASTER_BENCH_DATASET` (patrón de `MINIGIS_BENCH_DATASET`).

**Criterios de aceptación:**
- [ ] `ctest` pasa 13 pruebas (9 actuales + 4 nuevas)
- [ ] `minigis.raster-info` falla si falta `"type": "raster"` o el EPSG del DEM
- [ ] `minigis.raster-dibujo` falla si `--check-raster` imprime `FALLO` o no
      ejecuta cada comprobación esperada
- [ ] Con `MINIGIS_RASTER_BENCH_DATASET` aparece y pasa
      `minigis.rendimiento-raster` (o falla si se supera el presupuesto)

**Verificación:**
- [ ] `ctest --preset windows-msvc-release --output-on-failure`
- [ ] Reintroducir un fallo a propósito (p. ej. extensión) y comprobar que la
      prueba lo detecta

**Dependencias:** T10 · **Archivos:** `tests/run_cli_test.cmake`,
`tests/CMakeLists.txt` · **Tamaño:** S

---

### [x] T12 — `--bench` con rásters, borrador y medida

**Descripción:** `--bench` carga rásters (y los combina con vectorial), mide
cuadros de calidad y borrador, y expone los contadores del ráster. Se dibuja a
media resolución durante el gesto y se mide el GeoTIFF de 4096² con overviews.

**Criterios de aceptación:**
- [ ] `--bench --json` conserva las claves actuales y añade las del ráster
- [ ] El borrador no rompe la reutilización (paneo = redibujado, 0 px calidad)
- [ ] Medición registrada: p90 calidad y borrador, mediana, bytes/cuadro
- [ ] Si el p90 supera 100/45 ms, se documenta el valor real y se ajusta el
      presupuesto con el humano antes de cerrar

**Verificación:**
- [ ] `minigis.exe --bench 24 --bench-scenario mixed --json -l build\perf\terrain_big.tif --size 1200x800`
- [ ] Repetir 3 veces y anotar mediana/p90
- [ ] `ctest --preset windows-msvc-release`

**Dependencias:** T10 · **Archivos:** `src/app/main.cpp`,
`src/core/minigisrasterrenderer.cpp`, `src/gui/minigismapcanvas.cpp` · **Tamaño:** M

---

### Checkpoint D — contrato y rendimiento

- [x] 13/13 pruebas verdes (14 con la puerta de rendimiento activada)
- [x] `--check-raster` en verde y detecta fallos inducidos (color corrompido → FALLO, codigo 4)
- [x] Medidas de 4096² anotadas: mediana 4 ms, p90 5 ms, borrador <1 ms, 731 KB/cuadro
- [ ] Revisión humana antes de la documentación

---

## Fase F — Cierre

### [x] T13 — README y RENDIMIENTO.md

**Descripción:** README: ráster en "Hace", tabla de opciones nuevas, estructura
de archivos, pruebas y límites; RENDIMIENTO.md: sección del ráster con las
medidas de T12 (coste por cuadro, bytes leídos, memoria, comparación con el
vectorial).

**Criterios de aceptación:**
- [ ] Las opciones nuevas están en la tabla de uso y en `--help`
- [ ] Los comandos de ejemplo funcionan tal como están escritos
- [ ] La sección de rendimiento incluye números medidos (no estimaciones)
- [ ] "Qué no hace" se actualiza (ráster entra, rampas/hillshade siguen fuera)

**Verificación:**
- [ ] Ejecutar los ejemplos del README
- [ ] Revisar que los enlaces del spec siguen siendo válidos

**Dependencias:** T12 · **Archivos:** `README.md`, `RENDIMIENTO.md` · **Tamaño:** S

---

### [x] T14 — Verificación final contra el spec

**Descripción:** pasada completa de los criterios SC1–SC14 con compilación
limpia desde cero, sin artefactos de prueba en el árbol.

**Criterios de aceptación:**
- [ ] Configuración y compilación limpias (build nuevo) sin errores
- [x] 13/13 pruebas verdes (14 con la puerta de rendimiento activada)
- [ ] SC1–SC14 comprobados uno a uno y anotados
- [ ] `git status` solo muestra los archivos previstos (sin restos de `build\`)
- [ ] Spec y plan actualizados si alguna decisión cambió

**Verificación:**
- [ ] `rmdir /s build\windows-msvc-release` (o carpeta nueva) y repetir T1
- [ ] `ctest --preset windows-msvc-release --output-on-failure`
- [ ] Repasar la tabla de SC del spec marcando cada fila

**Dependencias:** todas · **Archivos:** ninguno previsto (correcciones si
aparecen) · **Tamaño:** S

---

### Checkpoint E — cierre

- [x] SC1–SC14 verificados y anotados (13/13 pruebas, check-raster 12/12, p90 4 ms)
- [x] Documentación al día: README y RENDIMIENTO.md §13
- [ ] Revisión humana final: el trabajo del spec está terminado

---

## Riesgos activos

Los del plan §6. Los dos que más pueden mover fechas: coste del warp por cuadro
(T6/T12) y exactitud de la reutilización de cuadro con ráster (T5/T10/T12).
