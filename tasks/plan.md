# Implementation Plan: capa ráster en MiniGIS

> Plan técnico derivado de [SPEC-raster.md](../SPEC-raster.md) (aprobado).
> Tareas concretas en [todo.md](todo.md). No se empieza a implementar hasta que
> este plan y la lista de tareas se aprueben.

## 1. Resumen

Añadir capas ráster de solo lectura a MiniGIS: abrir cualquier ráster de GDAL,
dibujarlo bajo el vectorial, reproyectarlo al SRC del lienzo, ajustar bandas,
contraste y opacidad, y verificarlo sin ventana con `--info`, `--snapshot`,
`--bench` y un nuevo `--check-raster`.

El trabajo se corta en **rebanadas verticales**: primero un camino completo que
abra, informe y dibuje un GeoTIFF en su SRC (sin warp), después la reproyección
y el contraste, después la integración en la interfaz y, al final, el contrato
de pruebas y las medidas. Cada corte deja el programa usable y las 9 pruebas
actuales verdes.

## 2. Arquitectura

```
                 ┌───────────────────────────────┐
                 │ src/app/main.cpp              │
                 │  · detección vector/ráster     │
                 │  · --info --snapshot --bench   │
                 │  · --check-raster              │
                 └───────────────┬───────────────┘
                                 │
        ┌────────────────────────┴────────────────────────┐
        │                                                 │
┌───────▼──────────────┐                       ┌──────────▼─────────────┐
│ src/gui              │                       │ src/core               │
│  MapCanvas           │  usa                  │  RasterLayer           │
│   · lista de rásters │ ────────────────────▶ │   · dataset GDAL (RAII)│
│   · orden de dibujo  │                       │   · metadatos, SRC     │
│   · zoom/extensión   │                       │   · estilo + revisión  │
│  LayerPanel          │                       │  RasterRenderer        │
│  MainWindow          │                       │   · warp cacheado      │
│  RasterStyleDialog   │                       │   · lectura por ventana│
└──────────────────────┘                       │   · LUT, NoData, alfa  │
                                               │   · estadísticas        │
                                               └────────────────────────┘
```

Flujo de dibujo de un cuadro:

1. `MapCanvas::renderToImage()` reutiliza el cuadro anterior si procede (igual
   que hoy) y deja una región expuesta.
2. `RasterRenderer::paint()` recorre los rásters visibles en orden de carga,
   antes de cualquier capa vectorial y con el mismo recorte.
3. Para cada capa calcula el rectángulo de pantalla de su extensión, la ventana
   de píxeles que hay que leer (con warp si el SRC no coincide), lee solo esa
   ventana al tamaño en píxeles del lienzo y la convierte a `QImage` ARGB con la
   LUT de contraste, NoData/alfa y paleta.
4. El motor vectorial dibuja encima como hasta ahora.
5. Al terminar, la caché de cuadro guarda la imagen y la **revisión de
   contenido**, que ahora incluye el estado de las capas ráster.

## 3. Decisiones de implementación

| Decisión | Motivo |
|---|---|
| `RasterLayer` y `RasterRenderer` en `core`, sin `QWidget` | Coherente con `VectorLayer`/`FastMapRenderer`; el renderizador ya usa `QPainter` en `core` |
| Lista de rásters paralela a la de vectores en `MapCanvas` | Evita refactorizar el motor vectorial y sus 9 pruebas (spec §4.1); el ráster va siempre debajo |
| El dataset de GDAL se abre, se lee y se dibuja en el hilo de la GUI | Un `GDALDatasetH` no es seguro entre hilos; no hay cargador en segundo plano para ráster (spec R2/A5) |
| Warp con `GDALCreateWarpedVRT` cacheado por SRC de destino, dentro de `RasterLayer` | Crear el VRT por cuadro cuesta más que dibujar; el SRC cambia poco |
| Lectura por ventana con `GDALRasterIO` al tamaño de salida y `GRIORA_Bilinear` | Acota memoria y tiempo por cuadro; usa overviews si existen |
| Composición a ARGB con LUT de 256 entradas por banda | Evita ramas por píxel; es el grueso del coste en un ráster Byte |
| Estadísticas de contraste de una lectura decimada ≤ 512×512 | Percentiles "suficientes" en milisegundos; nunca recorre el ráster completo |
| Modo borrador a media resolución, dibujado escalado | Mantiene 60 fps mientras se arrastra; el cuadro de calidad llega al parar |
| El ráster participa en la caché de cuadro, con revisión | Es una traslación pura; la revisión evita el fallo de "cambio el estilo y no se ve" |
| `LayerPanel` recibe una lista de entradas (`LayerEntry`) con tipo y punteros | Una sola tabla para las dos clases sin tocar `VectorLayer`; señales tipadas |
| Detección por contenido con `GDALOpenEx(RASTER|VECTOR)` y conteo de capas/bandas | No depende de extensiones; vectorial gana si el archivo tiene ambas cosas |

## 4. Orden de implementación

Dependencias (flechas = "necesita antes"):

```
T1 entorno* ─┐
T2 datos ────┼──▶ T3 RasterLayer ──▶ T4 RasterRenderer ──▶ T5 lienzo + snapshot
             │                                                  │
             │                                                  ▼
             │                                    T6 warp + NoData + gris
             │                                                  │
             │                                                  ▼
             │                                    T7 estiramiento + paleta
             │                                          │              │
             │                                          ▼              ▼
             │                              T8 panel/MainWindow   T10 --check-raster
             │                                          │              │
             │                                          ▼              ▼
             │                              T9 diálogo de estilo  T11 CTest
             │                                          └──────┬───────┘
             │                                                 ▼
             │                                   T12 bench + borrador + rendimiento
             │                                                 │
             └─────────────────────────────────────────────────▼
                                                        T13 documentación
                                                               │
                                                               ▼
                                                        T14 verificación final
```

\* T1 (actualizar presets/`env.bat`) es independiente y está pendiente de
confirmación; si se descarta, los comandos manuales del spec §6.1 siguen siendo
la referencia.

### Fase A — Fundación (T1–T2)

Datos deterministas y, si se confirma, un entorno que compile con un comando.

### Fase B — Camino mínimo (T3–T5)

Abrir un GeoTIFF, informarlo por `--info` y dibujarlo en `--snapshot` sin
reproyección. Es la rebanada vertical más pequeña que ya enseña algo.

### Fase C — Reproyección y composición (T6–T7)

DEM en UTM sobre lienzo EPSG:4326, NoData transparente, una banda en grises,
estiramiento y paleta.

### Fase D — Interfaz (T8–T9)

Panel, apertura, visibilidad, zoom, diálogo de estilo y captura del diálogo.

### Fase E — Contrato y rendimiento (T10–T12)

`--check-raster`, pruebas de CTest, `--bench` con ráster, modo borrador y las
medidas que fijan los presupuestos.

### Fase F — Cierre (T13–T14)

README, RENDIMIENTO.md y pasada completa de criterios de éxito.

## 5. Índice de tareas

Detalle completo (criterios, verificación, archivos, dependencias y tamaño) en
[todo.md](todo.md).

| # | Tarea | Fase | Dep. | Tamaño |
|---|---|---|---|---|
| T1 | Alinear presets, `env.bat` y README con el entorno real *(pendiente de confirmar)* | A | — | S |
| T2 | Generador determinista de rásters de ejemplo | A | — | S |
| T3 | `RasterStyle` + `RasterLayer`, detección y `--info` | B | T2 | M |
| T4 | `RasterRenderer` mínimo (mismo SRC, RGB, NoData/alfa) | B | T3 | M |
| T5 | Lienzo y `--snapshot` con rásters | B | T4 | S |
| T6 | Reproyección con warp, extensión densificada, una banda y NoData | C | T5 | M |
| T7 | Estiramiento (percentiles/min-máx/ninguno) y paleta | C | T6 | M |
| T8 | Panel de capas, apertura y acciones con rásters | D | T5 | M |
| T9 | `RasterStyleDialog` y acciones de estilo | D | T8 | M |
| T10 | `--check-raster`, opciones `--raster-*` y captura del diálogo | E | T7, T8, T9 | M |
| T11 | Pruebas de CTest del ráster y puerta de rendimiento | E | T10 | S |
| T12 | `--bench` con rásters, borrador y medida de rendimiento | E | T10 | M |
| T13 | README y RENDIMIENTO.md con el ráster y sus medidas | F | T12 | S |
| T14 | Verificación final contra el spec (SC1–SC14) | F | todas | S |

## 6. Riesgos y mitigaciones

| Riesgo | Impacto | Mitigación |
|---|---|---|
| El warp por cuadro cuesta más de lo previsto | Alto | VRT cacheado, lectura solo de la ventana visible, borrador a media resolución; se mide en T6/T12 y, si hace falta, se ajusta el presupuesto antes de seguir |
| La reutilización de cuadro con ráster deja costuras o desplazamientos | Alto | El ráster es traslación pura; se prueba reutilizar vs. redibujar en las 8 direcciones (T10) y se mide sin tolerancia en calidad |
| Colores o alfa distintos por premultiplicación | Medio | `QImage::Format_ARGB32_Premultiplied` y comparación exacta de cuadrantes/paleta; si hay desvío, se compara con tolerancia ±2 |
| El VRT o el dataset se comparte entre hilos por accidente | Alto | Apertura y dibujo en el hilo de la GUI; el cargador en segundo plano solo se usa para vectorial; regla en el código |
| `GDALOpenEx` con ambos modos falla en algún driver | Medio | Si la detección combinada falla, se reintenta con solo vector y solo ráster; GeoPackage mixto documentado como vectorial |
| Cambiar el panel rompe el flujo vectorial | Medio | `LayerEntry` envuelve los punteros existentes; las 9 pruebas y `--snapshot-window` se ejecutan tras T8 |
| El presupuesto p90 no se cumple en este equipo | Medio | Se mide en T12 con el GeoTIFF de 4096²; el presupuesto es un objetivo del spec y se documenta el valor real |
| El SDK 10.0.28000 vuelve a romper el enlace al reconfigurar | Bajo | T1 fija `rc.exe`/`mt.exe` en presets; si no se aprueba, el spec §6.1 ya lo indica |
| Los datos de ejemplo ocupan demasiado en git | Bajo | 3 GeoTIFF de 256²/128² (≈200 KB); el grande de rendimiento va a `build\` (ignorado) |

## 7. Paralelización

* **En paralelo:** T1 y T2 (entorno vs. datos) al principio; T11 (pruebas) y T12
  (medida) una vez cerrado T10.
* **Secuencial:** T3 → T4 → T5 → T6 → T7 (el renderizador crece por capas);
  T10 → T11 → T12 (el contrato antes que las pruebas y la medida).
* **Coordinación:** T8 y T10 comparten `MapCanvas` (extensión, orden, revisión).
  T10 se hace después de T8 para no tocar el lienzo dos veces.

## 8. Puntos de control

| Checkpoint | Después de | Exige |
|---|---|---|
| A — Camino mínimo | T5 | Compila, 9/9 pruebas, `--info` y `--snapshot` de `ortho_rgb` con los cuadrantes en su sitio |
| B — Reproyección y contraste | T7 | DEM UTM encuadrado sobre el lienzo 4326; NoData transparente; percentiles vs min-máx distinguibles |
| C — Interfaz | T9 | `--snapshot-window` escribe PNG; panel lista el ráster; el diálogo de estilo se abre y aplica cambios |
| D — Contrato y rendimiento | T12 | `--check-raster` en verde, pruebas nuevas en CTest, p90 medido y documentado |
| E — Cierre | T14 | SC1–SC14 del spec verificados; README/RENDIMIENTO actualizados |

Cada checkpoint implica **revisión humana** antes de seguir con la fase
siguiente.

## 9. Preguntas abiertas para esta revisión

1. **Presets y `env.bat` (T1):** ¿se actualizan a `D:/Qt/6.8.3`, `D:/VCPKG` y el
   `rc.exe` de 10.0.26100? Propuesto: sí; si no, se omite T1 y se trabaja con
   la línea de configuración del spec.
2. **Presupuesto de rendimiento:** se mantiene p90 ≤ 100 ms (calidad) y ≤ 45 ms
   (borrador) sobre 4096² con overviews; se confirma con la primera medida de
   T12 antes de fijarlo en la documentación.

Las decisiones de alcance (sin rampas de color, sin hillshade, ráster debajo del
vectorial) ya están cerradas en el spec y no se reabren aquí.
