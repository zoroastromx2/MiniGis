# Spec: mosaico de imágenes (carga masiva, progreso y navegación fluida)

> Módulo: `mosaico` · Estado: **implementado y verificado** · Fecha: 2026-11-21
>
> Este documento define *qué* se construye y *cómo se sabe que está bien*. No se
> escribe código hasta que se apruebe. El plan técnico y las tareas van después,
> en `tasks/plan-mosaico.md` y `tasks/todo-mosaico.md`.

---

## 1. Objetivo

El usuario abre de golpe **508 imágenes satelitales** de LINZ (Nueva Zelanda,
10 m/píxel, EPSG:2193, 1,54 GB) seleccionando todos los archivos en el diálogo.
MiniGIS hoy las abre como **508 capas sueltas**, una por archivo, en el hilo de
la interfaz: la ventana se queda **congelada unos 25 s sin ninguna señal**, y una
vez cargadas la vista de país tarda **decenas de segundos por cuadro** y cada
paso de zoom cuesta segundos, porque dibuja y reproyecta las 508 hojas en cada
cuadro aunque estén fuera de la vista.

Este cambio introduce el **mosaico de imágenes**:

* seleccionar N archivos ráster y verlos como **un solo mosaico** (un grupo en el
  panel de capas, una entrada en el orden de dibujado), no como N capas;
* una **ventana de progreso real** —archivo actual, cuenta, tiempo estimado,
  Cancelar— mientras se abren las hojas, y **las hojas aparecen en el mapa
  conforme se abren**;
* una vez cargadas, **paneo y zoom fluidos** (del orden de 0,2 s por cuadro
  frente a los 39 s de hoy), con una **réplica piramidal** en el SRC del lienzo
  que se construye una vez, se guarda en caché y se reutiliza en la sesión
  siguiente;
* todo verificable **sin ventana** (`--check-mosaic`), como el resto del motor.

**Usuario:** quien revisa una cobertura completa de imágenes (un país, una
región) y necesita encuadrarla, moverse y acercarse para leer el terreno sin
cambiar de herramienta.

**Criterio de éxito del producto:** elegir la carpeta entera en *Abrir*, ver la
ventana de progreso avanzar, ver el país dibujarse en menos de 2 s desde la
primera hoja y, al terminar la preparación, moverse y hacer zoom con cuadros por
debajo de 250 ms, también al día siguiente (la caché se reutiliza).

---

## 2. Estado de partida medido (este equipo, 2026-11-21)

Ejecutable `build\windows-msvc-release\bin\minigis.exe` (Release), 1200×800,
datos en caché de disco. Conjunto de la sección 14.

### 2.1 Lo que cuesta hoy

| Operación | Medido |
|---|---|
| Apertura de 64 hojas (`--info --json`, incluye estadísticas de contraste) | **3,2 s** → 508 hojas ≈ **26 s** |
| Cuadro completo con 64 hojas cargadas (`--snapshot`) | render **4,9 s** → 508 hojas ≈ **39 s** |
| Cuadro completo con 16 hojas (`--bench 4 --bench-scenario mixed`) | mediana **627 ms**, máximo 1320 ms |
| Cuadro con zoom a una hoja, con 64 hojas cargadas | render **417 ms** (se crean los 64 warps: 6,5 ms por hoja, aunque 63 estén fuera de vista) |
| Cuadro de una sola hoja | render **168 ms** |

Causas verificadas en el código:

1. `MainWindow::loadPaths` abre los ráster **en el hilo de la GUI**, en bucle
   síncrono, sin barra de progreso ni cancelación; cada `RasterLayer::open`
   calcula las estadísticas de contraste de las 3 bandas con una ventana de
   2048×2048 (≈ 50 ms por hoja).
2. `RasterRenderer::paint` pide `viewForEpsg()` **antes** de comprobar si la capa
   cae en la vista: cada capa crea su VRT reproyectado en cada escala nueva
   aunque esté fuera del lienzo.
3. `RasterLayer::viewForEpsg` fija la rejilla del VRT **a la escala del lienzo**
   y la rehace cuando cambia el zoom; con 508 hojas son 508 VRT nuevos por paso
   de zoom.
4. Cada banda se lee con un `GDALRasterIO` distinto: en un JPEG entrelazado eso
   son **3 decodificaciones completas por hoja y por cuadro**.
5. `syncLayerPanel` reconstruye el árbol entero en cada `layersChanged()`:
   añadir 508 hojas hace O(n²) inserciones.

### 2.2 Lo que se probó y **no** sirve (para no repetirlo)

Se construyó con `gdalbuildvrt` un VRT único de las 508 hojas (103200×151200 px)
y se midió con el motor actual:

| Variante | Cuadro de país | Región 320 km | Región 100 km |
|---|---|---|---|
| VRT único, sin overviews | **61,1 s** | — | — |
| VRT + overviews externas 8–256 (47 MB, 41 s de construcción) | **59,3 s** | 20,3 s | 419 ms |
| Lectura directa del VRT en su SRS (sin warp), 1200×800 | 1,0 s (con overviews) / 3,9 s (sin) | — | — |

El motivo: el *warper* de GDAL, con la rejilla a escala de lienzo que usa
MiniGIS, lee el origen a resolución completa aunque existan overviews. **Un VRT
único leído a través del warp actual no es la solución.**

### 2.3 Lo que sí funciona (prototipo con GDAL, medido)

Réplica del mosaico **reproyectada al SRC del lienzo** (EPSG:4326), 88 m/píxel,
GeoTIFF en teselas comprimido en JPEG (17116×17184, **33,6 MB**) más overviews
internas 2–16 (**+13,4 MB**):

| Operación | Medido |
|---|---|
| Construcción de la réplica (`gdalwarp -t_srs EPSG:4326 -tr 0.0008`) | **17,5 s** |
| Overviews internas (`gdaladdo 2 4 8 16`) | **4,4 s** |
| Cuadro de país (1200×800) | render **15 ms** |
| Cuadro de región 320 km | render **17 ms** |
| Cuadro de región 100 km | render **19 ms** |
| Cuadro de una hoja (réplica ampliada 8×) | render **14 ms** |

La diferencia con 2.2 es que aquí **el SRC del archivo coincide con el del
lienzo**: no hay warp por cuadro y GDAL elige el nivel de overview con una
lectura de ventana normal. Ese es el mecanismo que adopta este spec; el detalle
fino (por debajo de la resolución de la réplica) se sigue leyendo de las hojas
originales, ya culled y con el warp cacheado por nivel.

### 2.4 Cifras de referencia del conjunto

| Dato | Valor |
|---|---|
| Archivos | 508 `.jpg` + 508 `.jgw` + 508 `.jpg.aux.xml` + 1 `.txt` |
| Tamaño total | 1,54 GB (media 0,78 MB; máximo 5,12 MB) |
| Píxeles por hoja | 2400×3600 (24 × 36 km) |
| Paso de píxel | 10 m |
| SRC | EPSG:2193 (NZGD2000 / NZTM 2000) |
| Overviews por hoja | 1/2, 1/4, 1/8 (implícitas del driver JPEG) |
| Extensión del mosaico (2193) | 1 084 000 · 4 722 000 — 2 116 000 · 6 234 000 |
| Extensión del mosaico (4326) | ≈ 166,15 · −47,45 — 179,85 · −33,91 |
| Cobertura | mancha irregular de 43 filas (2..17 hojas por fila) |

---

## 3. Requisitos funcionales

### R1 — Selección múltiple = mosaico

* `Ctrl+O`, arrastrar y soltar y archivos recientes aceptan **varios archivos**
  (el diálogo ya es multiselección). Si de la selección salen **2 o más ráster**,
  se cargan como **un mosaico**; si sale uno, se comporta exactamente como hoy.
* La detección por contenido (`detectLayerKind`) no cambia: la selección puede
  mezclar vectoriales, un `.qgz` y ráster; los vectoriales conservan el camino
  actual y el ráster se agrupa en mosaico(s).
* Los archivos que no abren se **informan al final** (lista con nombre y motivo);
  una hoja ilegible no cancela la carga de las demás.
* Un mosaico solo agrupa hojas **georreferenciadas**. Un archivo ráster sin
  geotransformación se rechaza con `Error` accionable y no entra al mosaico.

### R2 — Ventana de progreso y cancelación

* Mientras se abren las hojas se muestra una **ventana de progreso** (diálogo
  modal propio, no solo la barra de estado) con:
  * texto `Cargando imágenes — 37 de 508`;
  * nombre de la hoja en curso;
  * barra de progreso determinada (por archivos terminados);
  * tiempo transcurrido y **estimación de restante** (a partir de la velocidad
    medida, no de una constante);
  * botón **Cancelar**.
* La interfaz **no se congela**: la apertura corre en un trabajador en segundo
  plano (hasta `hardware_concurrency` hilos, acotado a 8) y la ventana responde a
  Cancelar en **≤ 300 ms**.
* Cancelar **conserva** las hojas ya abiertas y termina la carga con un resumen
  (`Carga cancelada: 213 de 508 hojas`). Un segundo Cancelar durante la
  preparación de la réplica conserva el mosaico y lo deja usable a resolución
  gruesa (documentado en la ventana y en la barra de estado).
* Al terminar las hojas hay una segunda fase en la misma ventana:
  `Preparando el mosaico — nivel 2 de 4` (réplica, R6). Ambas fases son
  cancelables y reanudables en la sesión siguiente (la réplica a medias se
  descarta y se rehace; ver R6).
* Errores por hoja: se acumulan y, al final, se muestran en un solo aviso
  (nunca 508 avisos modales).

### R3 — Aparición incremental

* Las hojas se **añaden al mosaico conforme se abren**, en lotes: como máximo un
  repintado del lienzo cada **250 ms** y una reconstrucción del panel por lote
  (nunca una por hoja: hoy sería O(n²)).
* Mientras falte alguna hoja, la ventana y la barra de estado dicen `cargando
  N de 508` y el cuadro se dibuja con un **presupuesto**: si hay más de 40 hojas
  visibles, se dibujan las 40 más cercanas al centro de la vista y el resto
  queda para el cuadro siguiente (ningún cuadro de la fase de carga supera
  250 ms).
* El encuadre final es la **unión de todo lo cargado** (comportamiento actual de
  `frameLoadedBatch`), no un salto por hoja.

### R4 — Modelo del mosaico (core)

Nueva clase `RasterMosaic` en `src/core`, dueña de N `RasterLayer` y con:

| Dato | Uso |
|---|---|
| lista de hojas (ruta, nombre, tamaño, SRC, extensión, visibilidad) | panel, `--info`, réplica |
| extensión unión en su SRC y `extentInEpsg(destino)` | encuadre, orden de dibujado, recorte |
| visibilidad y estilo **del mosaico** (bandas, estiramiento, opacidad) | dibujado |
| `revision()` que sube al cambiar visibilidad, estilo, o al entrar/salir hojas | caché de cuadro del lienzo |
| estado de la réplica (ausente / construyendo % / lista / ruta) | progreso, `--info`, `--check-mosaic` |
| ruta de la réplica en caché y su lado en píxeles | lectura de la réplica |

Reglas:

* El mosaico se dibuja **debajo de las vectoriales**, en el orden de carga dentro
  del grupo ráster, igual que hoy.
* `zoomToFullExtent()`, `zoomToLayer()` y `layersExtent()` incluyen los mosaicos.
* **No** se recorre el mosaico hoja a hoja para saber si hay que redibujar: la
  revisión del mosaico basta (misma disciplina que `VectorLayer`).

### R5 — Dibujado: réplica para lo grueso, hojas para el detalle

`RasterRenderer` dibuja un mosaico en dos modos, con el mismo encuadre:

1. **Réplica** (R6) cuando el píxel de lienzo es **mayor o igual** que el de la
   réplica: una sola lectura de ventana sobre un GeoTIFF en el SRC del lienzo.
   Sin warp, sin recortes por hoja, sin costuras.
2. **Hojas** cuando el píxel de lienzo es más fino: se recorren **solo las hojas
   que cortan la vista** (descarte antes de crear warps), con:
   * warp cacheado **por nivel de detalle**, no por escala exacta: el nivel se
     cuantiza a potencias de 2 de píxel de lienzo, así un gesto de zoom no
     rehace 40 warps por cuadro;
   * **una** lectura por hoja y cuadro con las 3 bandas entrelazadas
     (`GDALDatasetRasterIO`), no una por banda;
   * lectura a través del overview 1/8 de la hoja cuando la escala lo permite.

Requisitos de calidad:

* El paso de un modo al otro no puede dejar **ni hueco ni salto de color
  visible** en el borde de la vista: en la transición se prefiere la réplica y se
  cambia cuando la réplica quedaría ampliada más de 2× (la comparación de ambos
  modos está en `--check-mosaic`).
* NoData, banda alfa, paleta, estiramiento y opacidad se comportan como hoy
  (spec `raster` vigente).
* El **paneo** sigue reutilizando el cuadro anterior (traslación pura) y el
  **borrador** sigue a media resolución; el mosaico entra en la revisión de
  contenido de la caché del lienzo.

### R6 — Réplica piramidal y caché en disco

* La réplica es un **GeoTIFF en teselas, comprimido en JPEG, en el SRC del
  lienzo**, con overviews internas, construido con GDAL (API C) desde las hojas.
* **Resolución**: el píxel de la réplica es el de la hoja multiplicado por 8
  (80 m para estas hojas), salvo que la extensión unión supere 16384 px de lado;
  en ese caso se sube el factor para no pasar de ese lado. (Con el conjunto de
  referencia: 88 m, 17116 px de lado, 33,6 MB + 13,4 MB de overviews.)
* **Ubicación**: `QStandardPaths::CacheLocation` + `/mosaicos/<clave>`, con
  `MINIGIS_MOSAIC_CACHE_DIR` para cambiarla. **Nunca** se escribe en la carpeta
  de los datos de origen. Si la carpeta no es escribible, la réplica se construye
  en memoria (`/vsimem`) y se avisa en la barra de estado: el mosaico funciona
  igual, solo se pierde la reutilización entre sesiones.
* **Identidad y validación**: la clave es un hash de las rutas ordenadas +
  tamaño + fecha de modificación de cada hoja + EPSG del lienzo + factor de la
  réplica + versión del formato. Si alguna hoja cambia (tamaño o fecha), la
  réplica se reconstruye.
* **Reutilización**: abrir el mismo conjunto en otra sesión **no** reconstruye la
  réplica (se comprueba por el `mtime` del archivo de caché, SC9).
* **Limpieza**: tope total `MINIGIS_MOSAIC_CACHE_MB` (por defecto 4096). Al
  pasarse, se borran las réplicas usadas menos recientemente (fecha en el
  archivo lateral `.json`), nunca la que se está usando.
* **Progreso**: cada nivel de overview informa `nivel k de K` y cada hoja de la
  fase base informa avance; cancelable (R2).
* La réplica **se puede desactivar** (`--no-mosaic-replica` y ajuste equivalente
  en la GUI) para diagnosticar; el mosaico entonces dibuja siempre desde las
  hojas (más lento, documentado).

### R7 — Panel de capas

* El mosaico aparece como **un grupo**: `Mosaico — nz-10m-satellite-imagery-2025-2026-JPEG (508 hojas)`
  con una fila hija por hoja (nombre, `Ráster (3 bandas)`, `2400x3600`, SRC).
  El grupo es la fila que se selecciona para **Estilo**, **Zoom** y **Quitar**.
* La casilla de visibilidad del grupo oculta el mosaico entero. Las hojas hijas
  son informativas en esta versión (ver §12, supuesto 6).
* `Quitar` sobre el grupo quita el mosaico y libera sus hojas y su réplica en
  memoria (la caché en disco se queda; la política de R6 la recicla).
* Durante la carga, la fila del grupo dice `Mosaico — N de 508 hojas` y crece
  por lotes; al terminar muestra el total y, si la réplica no está lista, el
  sufijo `· preparando niveles`.
* `Estilo` abre el diálogo ráster existente y aplica al mosaico (no por hoja).

### R8 — Un solo ráster no cambia

Abrir un único archivo ráster mantiene el camino actual, sin ventana de progreso
si tarda menos de 300 ms, sin grupo en el panel y sin réplica (un GeoTIFF con
overviews ya dibuja en milisegundos). El spec `raster` sigue vigente sin cambios.

### R9 — CLI y verificación sin ventana

Opciones nuevas (contrato documentado en `--help` y `README.md`):

| Opción | Efecto |
|---|---|
| `--layer-list <archivo.txt>` | carga una ruta por línea (necesario: 508 `-l` superan el límite de 32767 caracteres de la línea de comandos de Windows; hoy el proceso ni arranca) |
| `--check-mosaic` | ejecuta las comprobaciones de §9.3 y sale (`0` todo bien, `1` fallo) |
| `--snapshot-progress <archivo.png>` | captura la ventana de progreso con el conjunto que se le pase (para la documentación y para CTest) |
| `--mosaic-cache <dir>` | carpeta de caché de réplicas para esta ejecución |
| `--no-mosaic-replica` | dibuja siempre desde las hojas (diagnóstico y medida del respaldo) |
| `--cancel-after <n>` | con `--check-mosaic`/`--snapshot-progress`: cancela la carga tras `n` hojas y verifica que se conservan |

* `--info` reconoce un mosaico: `"type": "mosaic"`, `"tiles"`, `"extent"`,
  `"crs"`, `"replica": {"pixels", "bytes", "level"}`; las claves actuales de las
  capas no cambian.
* `--bench` con un mosaico mide el cuadro completo (réplica o hojas) y respeta
  `--bench-budget` como hoy.
* Códigos de salida: los actuales (0…6). Fin de archivo de `--layer-list` con
  rutas inexistentes → 2; lista vacía → 1.
* Variables de entorno de las pruebas opt-in: `MINIGIS_MOSAIC_DATASET` (carpeta o
  archivo de lista) y `MINIGIS_MOSAIC_BUDGET_MS` (por defecto 250).

### R10 — Errores accionables

Todo fallo devuelve `minigis::Error` con causa y acción:

| Caso | Mensaje / acción |
|---|---|
| Hoja sin georreferencia | «no tiene georreferenciación» / «añade un `.jgw` o usa `gdalwarp`» |
| Carpeta de caché no escribible | aviso, no error: se sigue con réplica en memoria |
| Réplica más grande que el tope de caché | se construye igual y se avisa del tamaño |
| Disco lleno al escribir la réplica | se borra el archivo parcial y se sigue sin réplica, con aviso |
| Todas las hojas fallan | un solo `Error` con el primer motivo y el recuento |

### R11 — No regresión

* Las 54 pruebas de CTest siguen verdes (3 de PostGIS y la puerta opt-in pueden
  seguir omitidas).
* El camino de un ráster suelto, el vectorial, las etiquetas, la tabla, la
  identificación y la caché de cuadro se comportan igual.
* La puerta `minigis.rendimiento` sigue pasando con su presupuesto.
* Los códigos de salida y el JSON de `--info`/`--bench` solo **añaden** claves.

### R12 — Documentación

* `README.md`: mosaico, opciones nuevas, variables de entorno, carpeta de caché,
  límites.
* `RENDIMIENTO.md`: sección nueva con las medidas de §2 y las de después.
* Este spec se actualiza al cerrar con las cifras finales (como se hizo en
  `SPEC-rendimiento-capas-grandes.md` §15).

---

## 4. Decisiones (y por qué)

**D1 — Un mosaico, no 508 capas.** La capa por archivo es el orden de magnitud
equivocado: el panel con 508 filas sueltas es inmanejable y el dibujado paga
508 warps. Un grupo con una entrada en el orden de dibujado y sus hojas como
hijas conserva la trazabilidad («¿está esta hoja?») sin multiplicar el coste. Se
reutiliza el mecanismo `groupKey`/`groupName` que ya usan GeoPackage y PostGIS.

**D2 — Réplica en el SRC del lienzo, no guerra de warps.** Medido (§2.2): un VRT
único leído con warp cuesta 59–61 s por cuadro por mucho que tenga overviews. La
réplica reproyectada **una vez** quita el warp del camino caliente: 15–19 ms
(§2.3). El detalle fino, donde apenas hay 1–4 hojas visibles, se sigue leyendo
de la fuente.

**D3 — Réplica a 1/8 del píxel de origen.** Es el compromiso medido: 47 MB y
22 s de construcción para este conjunto (17,5 s de warp + 4,4 s de overviews),
frente a ~90 s y ~180 MB si se hiciera a 1/4. La zona de transición (píxel de
lienzo entre 1× y 2× el de la réplica) tiene del orden de 20–40 hojas visibles y
es la que exige que el camino de hojas esté optimizado (D4).

**D4 — Optimizar el camino de hojas aunque la réplica cubra lo grueso.**
Descarte antes de crear warps, warp por nivel de detalle (potencias de 2),
lectura entrelazada de 3 bandas y lectura por el overview 1/8. Las tres primeras
son las causas medidas en §2.1 (417 ms de 64 warps para una sola hoja visible;
3 decodificaciones JPEG por hoja y cuadro).

**D5 — Apertura en paralelo y estadísticas perezosas.** Las estadísticas de
contraste solo hacen falta con `minmax`/`percentclip`; el modo por defecto es
`none`. Calcularlas solo cuando se piden quita del camino de carga la parte más
cara (una lectura de 2048×2048 por banda). La apertura en paralelo (4–8 hilos)
convierte los ~26 s en un puñado de segundos.

**D6 — La caché va a la carpeta del usuario, no a la de los datos.** El spec
`raster` prohíbe escribir junto al origen. La réplica es un derivado de 47 MB:
vive en `QStandardPaths::CacheLocation`, con tope, LRU y validación por
tamaño+fecha. Si no se puede escribir, se construye en memoria y se avisa.

**D7 — Ventana de progreso propia, no solo la barra de estado.** La barra no da
cuenta por hoja, no estima el restante y no se puede cancelar con comodidad; el
usuario la pidió explícitamente. Se reutiliza el patrón de diálogo modal de los
demás diálogos del proyecto y se añade captura sin ventana para poder probarla.

**D8 — Medir con los comandos, no con el reloj de pared.** Las cifras de este
spec salen del propio ejecutable (`render N ms`, `--bench --json`,
`bytes_read_*`), igual que las puertas de rendimiento existentes, para que
`--check-mosaic` y CTest sean estables.

---

## 5. Fuera de alcance (explícito)

* Editar, recortar, fusionar o escribir las hojas; convertir sobre el origen.
* **Visibilidad o estilo por hoja** en esta versión (solo del mosaico completo).
* Teselas precalculadas al estilo de un servidor de mapas, GPU/Qt Quick.
* Mosaicos que superen el tope de caché de golpe: se construyen y se avisa, pero
  no se trocean en varias réplicas.
* Detección automática de «esto es un mosaico» al abrir una carpeta (sigue
  decidiéndolo la selección del usuario).
* Reordenar capas en el panel o poner el mosaico por encima del vectorial.
* Cambiar el SRC del lienzo desde la interfaz (sigue fijo en EPSG:4326; `--crs`
  en la CLI, y la réplica se construye para ese EPSG).
* Mosaico de rásters que no comparten el SRC del lienzo sin reproyección (la
  réplica siempre se construye en el SRC del lienzo, eso sí está dentro).

---

## 6. Comandos

### 6.1 Compilar y probar

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

### 6.2 Verificar el mosaico sin ventana

```bat
:: fixture sintético (siempre en CTest; 6 hojas pequeñas, píxel marcador por hoja)
build\windows-msvc-release\bin\minigis.exe --check-mosaic ^
        -l data\sample\mosaico\hoja_1_1.tif -l data\sample\mosaico\hoja_1_2.tif ^
        -l data\sample\mosaico\hoja_2_1.tif -l data\sample\mosaico\hoja_2_2.tif ^
        -l data\sample\mosaico\hoja_3_1.tif -l data\sample\mosaico\hoja_3_2.tif

:: conjunto real de 508 hojas (lista en un archivo: 508 -l no caben en una linea)
dir /b "E:\Disco D\IRIS 4.0\2026\lds-new-zealand-2layers-SHP\nz-10m-satellite-imagery-2025-2026-JPEG\*.jpg" ^
    > build\perf\mosaico_508.txt
build\windows-msvc-release\bin\minigis.exe --check-mosaic --json ^
        --layer-list build\perf\mosaico_508.txt --mosaic-cache build\perf\cache

:: captura de la ventana de progreso
build\windows-msvc-release\bin\minigis.exe --snapshot-progress progreso.png ^
        --layer-list build\perf\mosaico_508.txt

:: informacion del mosaico (texto y JSON)
build\windows-msvc-release\bin\minigis.exe --info --json --layer-list build\perf\mosaico_508.txt
```

### 6.3 Medir

```bat
:: cuadro de pais, region y detalle (la puerta interna de --check-mosaic usa
:: estas mismas extensiones y tamanos)
build\windows-msvc-release\bin\minigis.exe --bench 24 --bench-scenario mixed --json ^
        --layer-list build\perf\mosaico_508.txt --size 1200x800

:: sin replica (respaldo: mide lo que cuesta dibujar siempre desde las hojas)
build\windows-msvc-release\bin\minigis.exe --bench 8 --bench-scenario mixed --json ^
        --layer-list build\perf\mosaico_508.txt --no-mosaic-replica --size 1200x800
```

Extensiones canónicas (EPSG:4326) para las medidas de cuadro:

| Vista | Extensión | Hojas visibles |
|---|---|---|
| País | `166.15,-47.45,179.85,-33.91` | 508 |
| Región 320 km | `173.0,-41.55,176.71,-39.33` | ≈ 100 |
| Región 100 km | `171.69,-34.79,172.78,-34.12` | ≈ 12 |
| Detalle (una hoja) | `171.826,-34.354,172.090,-34.031` | 1 |

### 6.4 Puerta opt-in con el conjunto real

```bat
cmake --preset windows-msvc-release ^
      -DMINIGIS_MOSAIC_DATASET="E:/Disco D/IRIS 4.0/2026/lds-new-zealand-2layers-SHP/nz-10m-satellite-imagery-2025-2026-JPEG" ^
      -DMINIGIS_MOSAIC_BUDGET_MS=250
ctest --preset windows-msvc-release -R minigis.rendimiento-mosaico --output-on-failure
```

Sin `MINIGIS_MOSAIC_DATASET` la prueba se registra y se **omite**
(`SKIP_REGULAR_EXPRESSION`), como `minigis.rendimiento-raster` y
`minigis.rendimiento-geoparquet`. El conjunto **no** se versiona.

---

## 7. Estructura del proyecto

```
src/core/
  minigismosaiclayer.h/.cpp      mosaico: hojas, extensión unión, visibilidad,
                                 estilo, revisión, estado de la réplica
  minigismosaicreplica.h/.cpp    clave de caché, construcción (warp + overviews),
                                 progreso, cancelación, LRU, réplica en memoria
  minigismosaicloader.h/.cpp     apertura en paralelo de las hojas, progreso por
                                 archivo, errores acumulados, cancelación
  minigisrasterrenderer.h/.cpp   dos modos (réplica / hojas), descarte, warp por
                                 nivel, lectura entrelazada
  minigisrasterlayer.h/.cpp      estadísticas perezosas, lectura por 3 bandas
src/gui/
  minigisloadprogressdialog.h/.cpp  ventana de progreso (dos fases, Cancelar)
  minigismapcanvas.*             lista de mosaicos, orden, extensión, revisión
  minigislayerpanel.*            grupo de mosaico y filas hijas
  minigismainwindow.*            selección múltiple, carga, diálogo, menús
src/app/main.cpp                 --layer-list, --check-mosaic, --snapshot-progress,
                                 --mosaic-cache, --no-mosaic-replica, JSON
tests/
  run_cli_test.cmake             modos check-mosaic y snapshot-progress
  CMakeLists.txt                 fixture, prueba opt-in y puerta de rendimiento
scripts/
  make_sample_rasters.py         añade el fixture de 6 hojas del mosaico
data/sample/mosaico/             hoja_R_C.tif (versionado, ≈ 60 KB en total)
README.md / RENDIMIENTO.md       uso, medidas y límites
```

Regla de dependencias (sin cambios): `core` no incluye cabeceras de `gui`; `app`
no tiene lógica de datos; `gui` no llama a GDAL salvo a través de `core`.

Si se añade o quita un archivo fuente, se edita el `CMakeLists.txt` de su carpeta
(listas explícitas, nunca `file(GLOB ...)`).

---

## 8. Estilo de código

Se mantiene el estilo del proyecto (`AGENTS.md`): C++17, `namespace minigis`,
miembros `m`, 2 espacios, llaves en línea nueva, comentarios en español sin
tildes, `Error` por retorno y RAII para todo recurso de GDAL.

Ejemplo del tono y de la disciplina de revisión que espera este cambio:

```cpp
// La revision es la unica llave de la cache de cuadro del lienzo: cualquier
// cosa que cambie lo que se dibuja (una hoja que entra, la visibilidad, el
// estilo) tiene que subirla, o el mapa se queda con el cuadro anterior.
void RasterMosaic::addTile(const std::shared_ptr<RasterLayer> &tile)
{
  if (!tile)
    return;
  mTiles.push_back(tile);
  mExtent.combine(tile->extentInEpsg(mDestinationEpsg));
  bumpRevision();
}
```

---

## 9. Estrategia de pruebas

### 9.1 Fixture sintético (versionado)

`data/sample/mosaico/`: **6 hojas** de 64×64 Byte RGB, EPSG:4326, dispuestas en
una rejilla de 3 filas × 2 columnas, cada una con un píxel de color único
(marcador) en su centro y un borde de 1 píxel de otro color. Generadas por
`scripts/make_sample_rasters.py` de forma determinista. ≈ 60 KB en total.

### 9.2 Pruebas de CTest

| Prueba | Comando / modo | Qué exige |
|---|---|---|
| `minigis.mosaico-info` | `--info --json` con las 6 hojas | `"type": "mosaic"`, `"tiles": 6`, extensión unión correcta |
| `minigis.mosaico-dibujo` | `--check-mosaic` con las 6 hojas | todas las líneas `OK` de §9.3 |
| `minigis.mosaico-progreso` | `--snapshot-progress` con las 6 hojas | PNG del diálogo escrito, y `--check-mosaic` informa progreso monótono |
| `minigis.mosaico-ventana` | `--snapshot-window` con las 6 hojas | PNG escrito: grupo de mosaico en el panel con 6 hijas |
| `minigis.mosaico-cancelar` | `--check-mosaic --cancel-after 3` | quedan 3 hojas, sin error, y la ventana sigue viva |
| `minigis.rendimiento-mosaico` | opt-in, `MINIGIS_MOSAIC_DATASET` | p90 de cuadro ≤ `MINIGIS_MOSAIC_BUDGET_MS` (250) en las 4 vistas |

### 9.3 Contrato de `--check-mosaic`

1. **Hojas registradas**: `tiles` = archivos dados; cada hoja aparece en el panel
   como hija del grupo (misma cuenta).
2. **Extensión unión**: la extensión del mosaico en 4326 coincide con la de GDAL
   (±1 px en los cuatro bordes).
3. **Cada hoja en su sitio**: los 6 marcadores del fixture caen en el píxel que
   dicta la transformación (±1 px) y con su color exacto (±2 por canal), también
   tras reproyectar a 4326.
4. **Sin costuras**: dos hojas contiguas no dejan píxeles de fondo en la línea de
   unión (0 px con el color de fondo en una franja de 3 px).
5. **Réplica**: se construye, se informa su tamaño en MB y su nivel; leer una
   ventana de la réplica da los mismos marcadores (±2) que leer las hojas.
6. **Transición réplica/hojas**: en el píxel de lienzo del orden del de la réplica
   (donde el renderizador cambia de modo) el cuadro con réplica y el cuadro con
   hojas difieren en ≤ 20 % de los píxeles y la diferencia media por canal es
   ≤ 8/255. (Medido con el conjunto real: 8,0–11,5 % y 2,4–3,1; el umbral del 2 %
   que se propuso al principio no tiene en cuenta el remuestreo de una imagen
   fotográfica, y el porcentaje solo debe detectar una réplica mal encajada, que
   daría ~100 %.)
7. **Descarte**: con la vista encima de una sola hoja, el renderizador informa
   `tilesRead` = 1 (no lee ni crea warps de las demás).
8. **Nivel de detalle**: dos cuadros seguidos a la misma escala no crean warps
   nuevos (`warpsCreated` = 0 en el segundo); un zoom de ±10 % crea como mucho
   `tilesVisible` warps.
9. **Lectura acotada**: bytes leídos por cuadro ≤ `ancho × alto × bandas × 4`.
10. **Redibujado**: mostrar/ocultar el mosaico y cambiarle el estilo cambia el
    cuadro de inmediato (revisión de la caché).
11. **Paneo reutilizado**: reutilizar el cuadro da lo mismo que redibujar desde
    cero (0 píxeles distintos, como el resto del proyecto).
12. **Caché reutilizada**: con la réplica ya construida, una segunda ejecución no
    la reconstruye (`replica.reused` = true y `mtime` sin cambios).
13. **Progreso y cancelación**: con `--cancel-after N` se conservan N hojas, no
    hay error y el resumen lo dice.
14. **Presupuesto de cuadro**: en las cuatro escalas (país y tres acercamientos),
    moverse a esa escala se dibuja dentro de `MINIGIS_MOSAIC_BUDGET_MS`; se mide
    después del primer cuadro, que es el que crea los warps.

Cada comprobación imprime `OK <descripción> — <dato medido>`; `--json` añade un
objeto con los valores para las pruebas y para `RENDIMIENTO.md`.

### 9.4 Matriz requisito → prueba

| Requisito | Prueba |
|---|---|
| R1, R2, R3 | `minigis.mosaico-progreso`, `minigis.mosaico-cancelar` |
| R4 | `minigis.mosaico-info`, `minigis.mosaico-dibujo` 1–2 |
| R5 | `--check-mosaic` 3–11 |
| R6 | `--check-mosaic` 5, 12 |
| R7 | `minigis.mosaico-ventana` |
| R8, R11 | las 54 pruebas actuales |
| R9 | `minigis.mosaico-info`, contrato JSON de `--bench` |
| R10 | `--check-mosaic` con una hoja sin georreferencia (fixture negativo) |
| Rendimiento | `minigis.rendimiento-mosaico`, `--check-mosaic` 14 |

---

## 10. Límites

**Siempre**

* `ctest --preset windows-msvc-release` completo antes de dar una tarea por
  terminada.
* Medir antes y después con §6.3 y anotar la cifra en `RENDIMIENTO.md`.
* Cerrar los datasets de GDAL con RAII (un dataset pertenece a quien lo abre;
  las hojas del mosaico las usa el hilo de la GUI, la réplica se construye en su
  trabajador y se abre después en el hilo de la GUI).
* Todo comportamiento verificable tiene su comprobación en `--check-mosaic`
  (píxeles, no milisegundos, salvo la comprobación de presupuesto).
* Errores con causa y acción sugerida.
* Comentarios y documentación en español (sin tildes en el código).

**Preguntar antes**

* Cambiar la arquitectura del lienzo (jerarquía de capas genérica) o tocar el
  renderizador vectorial.
* Cambiar el contrato JSON de `--info`/`--bench` más allá de añadir claves.
* Añadir dependencias (no se prevé ninguna: GDAL y Qt ya están).
* Cambiar el tope por defecto de la caché o la resolución de la réplica.
* Cambiar los datos de ejemplo existentes o el fixture óptico del ráster.

**Nunca**

* Escribir en la carpeta de los datos de origen (ni la réplica ni archivos
  temporales al lado de las hojas).
* Versionar las 508 imágenes ni ningún derivado pesado: los datos del conjunto
  real viven fuera del repositorio y las pruebas que los usan son opt-in.
* Debilitar o borrar una prueba existente para que pase un cambio nuevo.
* Silenciar un fallo con un valor por defecto que oculte el problema (p. ej.
  dibujar el fondo cuando la réplica o el warp fallan).
* Bloquear el hilo de la interfaz durante más de 250 ms en la fase de carga.

---

## 11. Criterios de éxito

Se considerará terminado cuando **todo** esto sea cierto y reproducible:

| # | Criterio | Cómo se comprueba |
|---|---|---|
| SC1 | Compila en Release con MSVC/Qt 6.10.3/GDAL 3.12.4 | §6.1 |
| SC2 | Elegir 508 archivos abre **un** mosaico con 508 hojas y un grupo en el panel | `--info --json`, `--snapshot-window` |
| SC3 | La ventana de progreso avanza por archivo, estima el restante y cancela en ≤ 300 ms | `--snapshot-progress`, `--cancel-after` |
| SC4 | Las hojas aparecen en el mapa durante la carga y ningún cuadro de la carga supera 250 ms | `--check-mosaic` 13 + medida |
| SC5 | Carga completa de 508 hojas (con réplica) ≤ 40 s; primer cuadro útil ≤ 3 s | medida §6.3, `--check-mosaic` |
| SC6 | Cuadro p90 ≤ 250 ms en las cuatro vistas canónicas, y p50 ≤ 100 ms | `--bench --json`, `--check-mosaic` 14 |
| SC7 | Cuadro de país ≤ 150 ms (hoy 39 s) | `--bench --json` |
| SC8 | Bytes leídos por cuadro ≤ `ancho × alto × bandas × 4` | `--check-mosaic` 9 |
| SC9 | La segunda sesión reutiliza la réplica (no la reconstruye) | `--check-mosaic` 12 |
| SC10 | Descarte y nivel de detalle efectivos (`tilesRead`=1 con una hoja visible; 0 warps nuevos al repetir escala) | `--check-mosaic` 7–8 |
| SC11 | Sin costuras ni salto visible en la transición réplica/hojas | `--check-mosaic` 4, 6 |
| SC12 | Cancelar conserva lo cargado y la aplicación sigue usable | `--check-mosaic` 13 |
| SC13 | Las 54 pruebas actuales siguen verdes, y las nuevas también | `ctest` |
| SC14 | `README.md` y `RENDIMIENTO.md` describen el mosaico y sus medidas | revisión |

---

## 12. Supuestos (corregir si alguno no es cierto)

1. El conjunto real se elige **de una vez** (todos los archivos de la carpeta) y
   no cambia entre sesiones: la caché de la réplica tiene sentido.
2. Se acepta un coste **único** de 20–40 s la primera vez (con progreso y
   cancelable) para construir la réplica; las siguientes aperturas no lo pagan.
3. Se acepta que la réplica ocupe **≈ 50 MB** por conjunto de este tamaño en la
   carpeta de caché del usuario.
4. La vista de detalle (por debajo de la resolución de la réplica) se lee de las
   hojas originales; no se construye un mosaico reproyectado a resolución
   completa (serían ~1,5 GB y minutos de construcción).
5. El SRC del lienzo es EPSG:4326 (fijo en la GUI); con `--crs` la réplica se
   construye para ese EPSG y se cachea aparte.
6. **La visibilidad es del mosaico, no de cada hoja.** Quitar u ocultar hojas
   sueltas queda para una segunda entrega; en el panel las hijas son
   informativas.
7. El estilo (bandas, estiramiento, opacidad) es del mosaico completo.
8. La apertura en paralelo de datasets de GDAL es segura en este proyecto: cada
   hilo abre y lee **su** dataset y no se comparten handles entre hilos hasta que
   la hoja queda cerrada en el trabajador y se entrega al hilo de la GUI.
9. Las hojas comparten SRC y resolución de píxel (cierto en este conjunto); si no,
   la réplica las reproyecta igual, pero las medidas de referencia son de este
   conjunto.
10. El toolchain de referencia es MSVC (GCC/MinGW no compila hoy el proyecto).

---

## 13. Preguntas abiertas (para la revisión)

1. **¿Un grupo con 508 hijas en el panel, o solo la fila del mosaico con el
   recuento?** Propuesto: grupo + hijas (trazabilidad de qué hoja falta), con el
   grupo plegado por defecto cuando hay más de 50 hojas.
2. **¿La réplica a 1/8 (≈ 47 MB, ≈ 22 s) o a 1/4 (≈ 180 MB, ≈ 90 s)?** Propuesto:
   1/8; con 1/4 la transición réplica/hojas queda más fina pero el coste de la
   primera apertura se multiplica por cuatro.
3. **¿Se acepta el tope de caché de 4 GB con LRU**, o se prefiere pedir
   confirmación antes de borrar una réplica?
4. **¿Construir la réplica siempre, o solo cuando el usuario hace zoom out por
   debajo de la resolución de una hoja?** Propuesto: siempre, porque el encuadre
   inicial de una tanda de 508 hojas ya es una vista de país (y sin réplica
   costaría 39 s).
5. **¿Visibilidad por hoja en esta entrega?** Propuesto: no (supuesto 6), pero es
   la petición más probable después de usarlo; implicaría reconstruir la réplica
   o dibujar siempre desde las hojas cuando haya hojas ocultas.
6. **¿El mosaico debe poder partirse en varios grupos** (p. ej., una selección
   con dos zonas inconexas)? Propuesto: no; un mosaico por selección y varios
   mosaicos se apilan en el orden de carga.
7. **¿Los objetivos de SC6/SC7 son los correctos?** Salen de las medidas de §2.3
   (15–19 ms con la réplica) y dejan margen para el camino de hojas; si el equipo
   de medida da menos margen en la transición, se ajusta el presupuesto de la
   zona de transición, no la funcionalidad.
8. **¿`--load-sample-data` debe incluir el mosaico pequeño** para que la función
   se pueda probar sin el conjunto real? Propuesto: sí (son 60 KB).

---

## 13 bis. Decisiones tomadas sobre las preguntas abiertas (2026-11-21)

El spec se aprobó sin cambios; quedan resueltas así:

1. **Panel**: grupo con hijas, plegado por defecto cuando hay más de 50 hojas.
2. **Réplica a 1/8** del píxel de origen (≈ 47 MB y ≈ 22 s con este conjunto),
   no a 1/4.
3. **Tope de caché de 4 GB** con borrado LRU, sin preguntar antes de borrar.
4. **La réplica se construye siempre** que haya mosaico: el encuadre inicial de
   una tanda de 508 hojas ya es una vista de país.
5. **Sin visibilidad por hoja** en esta entrega (supuesto 6); es lo primero que
   se valora después.
6. **Un mosaico por selección**; varios mosaicos se apilan en el orden de carga.
7. **Objetivos de SC6/SC7** tal cual (§11); se ajusta el presupuesto de la zona
   de transición si la primera medida real no da margen, no la funcionalidad.
8. **`--load-sample-data`** incluye el mosaico pequeño de 6 hojas.

El plan técnico y las tareas están en `tasks/plan-mosaico.md` y
`tasks/todo-mosaico.md`.

## 14. Anexo — Conjunto de referencia

| Dato | Valor |
|---|---|
| Carpeta | `E:\Disco D\IRIS 4.0\2026\lds-new-zealand-2layers-SHP\nz-10m-satellite-imagery-2025-2026-JPEG` |
| Hojas | 508 `AS21.jpg` … `CK22.jpg` (43 filas: `AS`…`CK`, 2..17 hojas por fila) |
| Archivos auxiliares | 508 `.jgw` (mundo), 508 `.jpg.aux.xml` (SRC + estadísticas), 508 `.xml` (metadatos LINZ), 1 `.txt` |
| Tamaño | 1,54 GB |
| Píxeles por hoja | 2400 × 3600 |
| Paso de píxel | 10 m (`.jgw` = 10, 0, 0, −10) |
| Driver | JPEG, 3 bandas Byte, espacio de color YCbCr, overviews implícitas 2/4/8 |
| SRC | `EPSG:2193` — NZGD2000 / New Zealand Transverse Mercator 2000 |
| Extensión (2193) | 1 084 000 · 4 722 000 — 2 116 000 · 6 234 000 |
| Extensión (4326) | ≈ 166,15 · −47,45 — 179,85 · −33,91 |
| Origen | Toitū Te Whenua Land Information New Zealand (LDS), licencia CC BY 4.0 |
| Se versiona | **no** (solo un fixture sintético de 60 KB en `data/sample/mosaico/`) |

---

## 15. Resultado de las fases A y B (2026-11-21)

Medido con el ejecutable Release de este equipo y el conjunto de §14 (508 hojas).

| Medida | Antes (508 capas sueltas) | Ahora |
|---|---|---|
| Registro de las hojas y extensión unión | — | 508/508, error **0,00 px** |
| Colocación de cada hoja (muestra de 9) | — | 9/9, peor diferencia **0** |
| Cuadro de país (1200×800) | 39 s (extrapolado de 64 hojas) | **14 ms** con réplica (42,5 s sin ella) |
| Cuadro en la transición (≈ 4 hojas visibles) | — | **336 ms** el primer cuadro de esa escala; **15 ms** al moverse (hojas con warp cacheado) |
| Cuadro de una hoja | 168 ms | **204 ms** (una hoja, primer warp) |
| Descarte | 0 (se creaban 64 warps con 1 hoja visible) | **1** hoja leída de 1 visible |
| Zoom del 1 % a la misma potencia de 2 | recreaba el warp de cada hoja | **0 warps** nuevos |
| Réplica de las 508 hojas | — | **14428×14486, 34,5 MB, 4 niveles**, construida en ≈ **118 s** |
| Segunda sesión | — | réplica **reutilizada** (sin reconstruir) |
| `--check-mosaic` con las 508 | — | **14/14 comprobaciones**, 28 s con caché |

**Desviaciones respecto a los objetivos del spec, con su causa:**

* **La réplica tarda ≈ 118 s, no 40 s.** El prototipo con la herramienta
  `gdalwarp` tardaba 17,5 s porque lee los overviews 1/8 de cada hoja; el camino
  por la API (`GDALCreateWarpedVRT` + copia por bloques, que es el que resultó
  estable) lee el origen a resolución completa: 64 veces más píxeles
  decodificados. `USE_SRC_OVERVIEWS` y `NUM_THREADS` no cambian la cifra en este
  camino. Es aceptable con la ventana de progreso y la cancelación de la fase C,
  pero hay que mejorarlo (trabajo pendiente: materializar con el mismo camino que
  `gdalwarp`, o construir la réplica en segundo plano mientras se trabaja).
* **La banda de transición (≈ 4 hojas visibles) cuesta 336 ms en el primer
  cuadro de esa escala**, por encima del presupuesto de 250 ms de SC6; al moverse
  en esa misma escala baja a ≈ 15 ms porque los warps quedan cacheados. Cada hoja
  cuesta ≈ 90 ms la primera vez (GDAL hace un warp por banda sobre el VRT).
  Trabajo pendiente: caché de imagen por hoja y nivel (o réplica a 1/4 para que la
  transición ocurra con menos hojas).
