# SPEC — Rendimiento de capas vectoriales grandes (GeoParquet topo NZ)

**Estado:** implementado y verificado (2026-11-21)
**Capa de referencia:** `E:\Disco D\IRIS 4.0\2026\lds-new-zealand-2layers-SHP\lds-nz-river-centrelines-topo-150k-Parquet\nz-river-centrelines-topo-150k.parquet`
**Stack:** C++17, Qt 6.10.3, GDAL 3.12.4 por la API C de OGR, CMake + vcpkg, MSVC 19.44,
preset `windows-msvc-release`.
**Alcance:** ruta compartida de datos/render (sirve a shapefile, GeoPackage y GeoParquet); esta
capa es la puerta de rendimiento y la prueba de aceptación.

---

## 1. Objetivo

Que una capa vectorial de cientos de miles de elementos y decenas de millones de vértices
**se abra en segundos y se manipule con fluidez** (paneo, zoom, identificar y tabla de
atributos), sin volver a leer el archivo entero en cada cuadro y sin congelar la interfaz.

El caso que lo motiva: la capa de ríos topográficos de LINZ (Nueva Zelanda), **711 574 líneas,
~22,1 M de vértices, 226,5 MB en GeoParquet, EPSG:2193**, un solo campo. Hoy:

| Operación (Release, este equipo) | Medido hoy |
|---|---|
| `--info --json` | 2,38 s |
| `--check-geoparquet --json` | 1,66 s, pero **almacén no construido** (`storeFeatures: -1`) |
| `--snapshot` 1600×900 (extensión completa) | **20,9 s** (render 20,2 s) |
| `--snapshot` 1200×800 (extensión de ~20 km) | **12,0 s** (render 11,7 s) |
| `--bench 4 --bench-scenario mixed` 1200×800 | **21,3 s/cuadro** (0,05 fps) |
| `--bench … --spatial-filter` | 19,9 s/cuadro (sin mejora) |
| `--identify-at` | 1,08 s (576 ms de consulta) |
| `--snapshot-window` 1400×900 sin tabla | **34,3 s** |
| `--snapshot-window --with-attributes` 1400×900 | **531,4 s** |

Causas verificadas:

1. `GeometryStore` rechaza la capa porque estima **22,1 M de vértices** sobre los primeros 512
   elementos y el tope por defecto es **8 M** (`GeometryStore::Options::vertexBudget`).
2. El respaldo de índice de envolventes (`buildEnvelopeIndex`) **solo existe para shapefile**;
   para Parquet falla con «El indice de envolventes solo esta disponible para archivos ESRI
   Shapefile en disco».
3. Sin almacén, cada cuadro dibuja la capa con el **camino clásico**
   (`minigismapcanvas.cpp`, capas sin almacén), que **lee y reproyecta los 711 574 elementos en
   cada cuadro**; el renderizador rápido solo agrupa capas con almacén.
4. El filtro espacial del render se desactiva cuando hay reproyección
   (`minigismaprenderer.cpp`, `mUseSpatialFilter && !transformation`), así que `--spatial-filter`
   no ayuda con una capa en EPSG:2193 sobre un lienzo en EPSG:4326.
5. La tabla de atributos pide los valores **fila a fila** (`OGR_L_GetFeature(fid)` con caché de
   una sola fila) para hasta 100 000 filas; en Parquet el acceso aleatorio cuesta ~5 ms por fila:
   ~500 s de los 531 s medidos.

**Criterio de éxito del producto:** abrir esta capa desde la interfaz, ver el mapa en segundos y
poder hacer paneo, zoom, identificar y abrir la tabla sin esperas de decenas de segundos, con
`--bench` dentro de los presupuestos de la sección 4.

---

## 2. Supuestos (corregir si alguno no es cierto)

1. Se acepta que el almacén en memoria use **hasta 1 GB duro** de RAM (configurable a la baja con
   `MINIGIS_STORE_BUDGET_MB`), con el tope actual de 8 M de vértices como mínimo.
2. La primera vez que se abre la capa se puede **esperar a que el almacén esté construido**, con
   barra de progreso y botón de cancelar; no se exige un cuadro grueso inmediato.
3. «Manipulación» incluye paneo/zoom, identificar y la tabla de atributos; no incluye edición,
   teselas, GPU ni simbología por categorías.
4. La capa de referencia **no se versiona** (226 MB): las pruebas que la usan son *opt-in* por
   variable de entorno, como ya ocurre con `MINIGIS_BENCH_DATASET` y
   `MINIGIS_RASTER_BENCH_DATASET`.
5. Se mantiene GDAL como único motor de datos, por la API C de OGR; **no** se añade una
   dependencia directa de Arrow/Parquet.
6. El equipo de medida es este (Ryzen 7 5700G, 16 hilos, 30 GB de RAM, Release, `windows-msvc-release`).

---

## 3. Requisitos funcionales

### R1 — Presupuesto de almacén de 1 GB duro

* El tope de vértices del almacén deja de ser el 8 M fijo: se calcula para que el **almacén
  completo** (nivel 0 + niveles precalculados) quepa en **1 GB**, con el 8 M actual como suelo.
  El nivel 0 son 16 B/vértice y se reserva el tope de 192 MB de niveles.
* El presupuesto se puede **bajar** con `MINIGIS_STORE_BUDGET_MB` (p. ej. 256 en equipos con poca
  RAM); no se sube por encima de 1 GB sin cambiar el código.
* El presupuesto y el consumo real se informan en `--bench --json` (`store.vertices`,
  `store.bytes`, `store.build_ms`) y en `-V`.
* Si la capa no cabe ni con el tope, se mantiene un respaldo **acotado**: se avisa y ningún
  cuadro puede tardar más de un tope configurable (no se vuelve a congelar la ventana durante
  minutos).

### R2 — Construcción del almacén en segundo plano

* La lectura, reproyección, índice y niveles de simplificación se construyen en el trabajador
  (`VectorLoader`) con progreso por fases (abrir, leer, reproyectar, indexar, simplificar) y
  cancelación.
* Con esta capa, `--check-geoparquet` debe informar `storeFeatures: 711574` y `missingInStore:
  false`.
* La capa no se añade al mapa hasta que su almacén está listo (decisión del usuario); mientras
  tanto el panel muestra el progreso.

### R3 — Dibujado por cuadro

* Con el almacén construido, el renderizador rápido debe dibujar esta capa **sin releer el
  archivo**: consulta espacial + nivel de simplificación + lotes.
* Presupuestos (sección 4): mediana de paneo/zoom ≤ 200 ms y p90 ≤ 400 ms a 1200×800; cuadro de
  la extensión completa ≤ 400 ms; cuadro de borrador ≤ 120 ms.
* Los niveles de simplificación se precalculan en la construcción (hoy 6 niveles, con tope de
  192 MB) y no bloquean el primer cuadro.

### R4 — Identificar

* El clic debe resolver contra el almacén (consulta espacial + prueba punto-geometría) cuando
  exista, sin recorrer la capa en OGR.
* `--identify-at` ≤ 300 ms con esta capa (hoy 576 ms de consulta).
* Sin almacén (capa por encima del presupuesto) el comportamiento actual se conserva, con el
  tope de candidatos declarado.

### R5 — Tabla de atributos

* La tabla deja de pedir filas sueltas: lee **páginas contiguas** en orden (cursor hacia delante
  y, si hace falta saltar, una lectura secuencial desde el inicio de la página) y cachea la
  página visible.
* Ajustar el ancho de columnas **no** puede recorrer las 100 000 filas: anchos iniciales fijos o
  medidos sobre una muestra.
* Abrir la tabla con esta capa ≤ 2 s; pasar de página ≤ 1 s; el resto de la interfaz no se
  bloquea.
* Se conserva el tope de 100 000 filas mostradas y el aviso de que hay más.

### R6 — Diagnóstico y contrato de la CLI

* `--bench --json` gana (o completa) el bloque `store` con `mode`, `features`, `vertices`,
  `bytes`, `build_ms`, `read_ms`, `index_ms`, `levels_ms` y `renderer_path`; los campos actuales
  no cambian de nombre ni de significado.
* `-V` traza por fase: apertura, lectura, reproyección, niveles, consulta y pintado.
* Nuevo interruptor opcional para el tope de memoria (variables de entorno
  `MINIGIS_STORE_BUDGET_MB`, o parámetro equivalente) documentado en `README.md`.

### R7 — Respaldo acotado para capas que no caben

* Si el almacén no cabe, la capa sigue abriéndose, pero:
  * se avisa en el panel y en la barra de estado;
  * el dibujo usa un presupuesto por cuadro (elementos/vértices) y no bloquea más de un tope
    fijo (p. ej. 2 s) por cuadro;
  * identificar y la tabla mantienen su propio límite.
* No se implementa en este spec un índice de envolventes para Parquet (queda como trabajo
  posterior si el respaldo acotado no basta).

### R8 — No regresión

* `ctest --preset windows-msvc-release` completo en verde (hoy 53 pruebas: 50 en verde y 3
  opt-in omitidas).
* Las capas pequeñas (shapefile, GeoPackage, KML, ráster) mantienen tiempos y salidas de
  `--info --json`, `--snapshot` y `--check-*` sin cambios de contenido.
* La puerta `minigis.rendimiento` sigue pasando con su presupuesto actual.

### R9 — Documentación

* `RENDIMIENTO.md`: nueva sección con la medición de esta capa, el antes/después y el desglose
  por fase.
* `README.md`: variables de entorno nuevas, notas de la tabla de atributos y límites.
* Este spec se actualiza al cerrar con las cifras finales.

---

## 4. Contrato de rendimiento y comandos

### 4.1 Comandos de trabajo

```bat
:: compilar y probar (preset verificado en este equipo)
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure

:: puerta opt-in de rendimiento con la capa de referencia
set MINIGIS_GEOPARQUET_BENCH_DATASET=E:\Disco D\IRIS 4.0\2026\lds-new-zealand-2layers-SHP\lds-nz-river-centrelines-topo-150k-Parquet\nz-river-centrelines-topo-150k.parquet
set MINIGIS_GEOPARQUET_BUDGET_MS=200
ctest --preset windows-msvc-release -R minigis.rendimiento-geoparquet --output-on-failure

:: medir a mano (M1–M10)
bin\minigis.exe --bench 24 --bench-scenario mixed --size 1200x800 --json -l "%MINIGIS_GEOPARQUET_BENCH_DATASET%"
```

El ejecutable de trabajo es `build\windows-msvc-release\bin\minigis.exe`; los comandos de la
tabla siguiente lo abrevian como `minigis`.

### 4.2 Mediciones

Todas las cifras con el ejecutable Release de este equipo, datos en caché de disco, ventana de
1200×800 o 1600×900 según se indique, y la capa de la sección 1.

| # | Comando | Métrica | Hoy | Objetivo |
|---|---|---|---|---|
| M1 | `minigis --info --json -l <parquet>` | tiempo total | 2,38 s | ≤ 2,5 s (sin regresión) |
| M2 | `minigis --check-geoparquet --json -l <parquet>` | `storeFeatures` / tiempo | −1 / 1,66 s | 711574 / ≤ 10 s |
| M3 | `minigis --snapshot out.png -l <parquet> --size 1600x900` | render del cuadro completo | 20,2 s | ≤ 0,4 s |
| M4 | `minigis --snapshot out.png -l <parquet> --size 1200x800 --extent 172.5,-41.4,172.7,-41.2` | render con zoom | 11,7 s | ≤ 0,3 s |
| M5 | `minigis --bench 24 --bench-scenario mixed --json -l <parquet>` | mediana / p90 de cuadro | 20,2 s / 22,3 s | ≤ 0,2 s / ≤ 0,4 s |
| M6 | `minigis --identify-at 172.6,-41.3 --json -l <parquet>` | `elapsed_ms` | 576 ms | ≤ 300 ms |
| M7 | `minigis --snapshot-window ui.png --with-attributes -l <parquet>` | tiempo total | 531,4 s | ≤ 12 s |
| M8 | M7 con la tabla abierta | primera página / cambio de página | ~500 s | ≤ 2 s / ≤ 1 s |
| M9 | `--bench --json` | `store.bytes` / RAM del proceso | 0 | ≤ 1 GB / ≤ 1,5 GB |
| M10 | `minigis --snapshot-window ui.png -l <parquet>` | carga sin tabla | 34,3 s | ≤ 10 s |

Notas de medición:

* «Render del cuadro» es el tiempo que reporta el propio ejecutable
  (`Imagen escrita: … (WxH, render N ms)`), no el tiempo total del proceso.
* Para M5 se usa la mediana del JSON (`quality.median_ms`), que es más estable que el p90; el
  p90 se vigila como puerta.
* M7 y M10 incluyen la construcción del almacén en segundo plano y el primer cuadro; se repiten
  dos veces y se toma la segunda (caché de disco caliente).

---

## 5. Decisiones (y por qué)

* **D1 — Subir el presupuesto del almacén antes que inventar un índice de archivo.** Leer y
  reproyectar 22 M de vértices una vez, en segundo plano, es más simple y más rápido que
  consultar el Parquet en cada cuadro; el almacén ya tiene niveles de simplificación y consulta
  espacial. El coste es RAM: nivel 0 a 16 B/vértice ≈ 354 MB, más niveles (tope 192 MB),
  envolventes y anillos ≈ **575 MB**, y el equipo tiene 30 GB (11 GB libres en la medición).
* **D2 — 1 GB duro, configurable a la baja.** Un tope fijo grande (p. ej. 64 M vértices) reventaría
  equipos con poca RAM; uno pequeño mantiene el problema. Se calcula un límite de vértices a
  partir de 1 GB y se puede bajar con `MINIGIS_STORE_BUDGET_MB` para equipos concretos y pruebas.
* **D3 — Esperar al almacén con progreso (decisión del usuario).** Evita una segunda ruta de
  dibujo con geometría falsa y mantiene una sola semántica de consulta/identificar. La
  contrapartida es que el primer cuadro llega tras la construcción (estimada en 4–10 s para
  esta capa), no en 2 s desde el clic.
* **D4 — La tabla lee páginas, no filas.** El acceso aleatorio por FID en Parquet es el orden de
  magnitud equivocado (5 ms/fila × 100 000). Leer en orden y cachear la página reduce el coste a
  decenas de milisegundos y no cambia el contrato visible (mismas filas, mismo orden).
* **D5 — GDAL por la API C, sin dependencia de Arrow.** El driver Parquet ya entrega las
  geometrías; añadir Arrow obligaría a mantener dos superficies de datos y a enlazar una
  biblioteca más en `minigis_core`.
* **D6 — El respaldo no puede congelar.** Aunque no se resuelva el caso de capas que no caben,
  un cuadro debe acotarse; hoy el camino clásico tarda 20 s y la ventana parece colgada.
* **D7 — Medir con el ejecutable, no con el reloj de la pared.** Las métricas de render salen
  del propio ejecutable (y de `--bench`), para que las puertas de CTest sean estables.

---

## 6. Fuera de alcance (explícito)

* Teselas (*tile cache*), GPU/Qt Quick, render multinivel en segundo plano (puntos 1–3 de
  `RENDIMIENTO.md` §10).
* Índice de envolventes para Parquet (se valora después de M2/M5; hoy solo shapefile).
* Filtrado por atributos, edición, simbología por categorías, etiquetas.
* Cambiar el formato de datos o escribir GeoParquet.
* Reducir el tope de 100 000 filas de la tabla o paginar en servidor.
* Cualquier cambio en los códigos de salida de la CLI.

---

## 7. Estructura y archivos previstos

```
cmake/                          (sin cambios salvo que se añada una opción de presupuesto)
src/core/
  minigisgeometrystore.h/.cpp   presupuesto adaptativo, estadísticas por fase
  minigisvectorloader.h/.cpp    opciones del almacén, progreso por fases, cancelación
  minigisvectorlayer.h/.cpp     lectura de atributos por páginas (cursor secuencial)
  minigisidentify.cpp           consulta contra el almacén cuando exista
src/gui/
  minigisattributetablemodel.h/.cpp  páginas, caché y anchos de columna sin recorrer todo
src/app/main.cpp                JSON de --bench, trazas -V, opción de presupuesto
tests/                          prueba opt-in de rendimiento y unitarias de presupuesto/páginas
RENDIMIENTO.md / README.md      medición y uso
```

---

## 8. Estilo de código

Las reglas del proyecto están en `AGENTS.md`; el spec no las repite. Para este cambio:

```cpp
// Presupuesto adaptativo: nunca por debajo del tope historico, nunca por
// encima del limite duro. La RAM libre se consulta una vez por construccion.
qint64 adaptiveVertexBudget(qint64 freeBytes)
{
  constexpr qint64 kFloorVertices = 8'000'000;
  constexpr qint64 kCeilingBytes = 1024LL * 1024 * 1024;
  const qint64 bytes = std::min(kCeilingBytes, freeBytes / 3);
  return std::max(kFloorVertices, bytes / kBytesPerVertex);
}
```

* Comentarios en español sin tildes, Doxygen en lo público, `minigis::` y nombres
  `minigis<Nombre>.cpp/.h`.
* Nada de `file(GLOB)`; listas de fuentes explícitas.

---

## 9. Estrategia de pruebas

* **Unitarias (core):** cálculo del presupuesto adaptativo (suelo, techo, poca RAM), lectura de
  páginas de atributos (orden, saltos, filas fuera de rango) y stats de construcción.
* **CTest existentes:** las 53 pruebas actuales deben seguir en verde, incluidas las 12 de
  GeoParquet y la puerta `minigis.rendimiento`.
* **Puerta opt-in nueva:** `minigis.rendimiento-geoparquet`, registrada siempre y **omitida** si
  no existe `MINIGIS_GEOPARQUET_BENCH_DATASET` (patrón `SKIP_REGULAR_EXPRESSION`). Corre
  `--bench 24 --bench-scenario mixed --json` y falla si `quality.median_ms >`
  `MINIGIS_GEOPARQUET_BUDGET_MS` (por defecto 200) o si `store.mode != "memory"` o
  `store.features` no coincide con la capa.
* **Prueba de tabla:** con la capa real, `--snapshot-window --with-attributes` dentro de un
  presupuesto (omitida sin la variable) y una prueba unitaria con una capa sintética de 1 000
  filas que verifique que solo se leen las páginas pedidas.
* **Medición manual:** comandos M1–M10 de la sección 4, anotados en `RENDIMIENTO.md`.

---

## 10. Límites (Always / Ask first / Never)

**Always**
* `ctest --preset windows-msvc-release` antes de dar por cerrado un cambio.
* Medir antes y después con los comandos de la sección 4 y anotar la cifra.
* Mantener los códigos de salida y el contrato de `--bench --json` (solo se añaden campos).
* Comentarios y documentación en español, sin tildes en el código.

**Ask first**
* Cambiar el significado del tope de 8 M o el valor por defecto del presupuesto.
* Añadir una dependencia (Arrow, mimalloc, …).
* Tocar el JSON de `--bench` más allá de añadir campos, o cualquier opción de la CLI.
* Cambiar el tope de 100 000 filas de la tabla de atributos.

**Never**
* Versionar el `.parquet` de 226 MB o cualquier derivado pesado.
* Enlazar Arrow/Parquet directamente en `minigis_core` (la regla es GDAL por API C).
* Desactivar o relajar pruebas para que pasen.
* Dejar una ruta con la ruta absoluta del dataset en el código o en las pruebas (usar la
  variable de entorno).

---

## 11. Riesgos

| Riesgo | Mitigación |
|---|---|
| El almacén de 22 M de vértices tarda más de lo estimado o usa más de 1 GB | Medir M2/M9; si no cabe, bajar el presupuesto adaptativo y activar el respaldo acotado (R7) |
| El primer cuadro completo sigue siendo lento por dibujar 711 574 elementos | Medir M3/M5; si hace falta, usar el modo de puntos para elementos sub-píxel y bajar el nivel inicial |
| La tabla secuencial no soporta saltos grandes con el driver Parquet | Medir el salto de página; si es lento, leer un bloque desde la fila pedida con `SetNextByIndex` y presupuesto |
| Las pruebas opt-in no se ejecutan nunca en CI | Se documenta el comando y se añade la variable a la lista de puertas opcionales del README |
| Regresión en capas pequeñas por el presupuesto adaptativo | El suelo es el tope actual y la puerta `minigis.rendimiento` lo vigila |

---

## 12. Criterios de éxito (verificables)

- [x] M1 0,10 s (caliente) y sin cambios en el JSON de `--info`.
- [x] M2: `storeFeatures = 711574`, `missingInStore: false`, 5,34 s.
- [x] M3 render 57-64 ms; M4 render 29 ms.
- [x] M5: mediana 47-52 ms, p90 50-54 ms en 24 cuadros `mixed`.
- [x] M6 160-250 ms.
- [x] M7 4,92 s; M8 primera página incluida en M7 (la tabla lee páginas, no filas).
- [x] M9 `store.bytes` 388 MB y RAM máxima del proceso 732 MB.
- [x] M10 4,86 s.
- [x] El primer cuadro una vez construido el almacén ≤ 2 s (el render es de decenas de ms).
- [x] `ctest` en verde (54 pruebas: 50 en verde, 3 de PostGIS y la puerta opt-in omitidas) y
  `minigis.rendimiento` sin regresión.
- [x] `RENDIMIENTO.md` (§15) y `README.md` actualizados con las cifras y las variables nuevas.

---

## 13. Decisiones tomadas sobre las preguntas abiertas (2026-11-21)

1. **Primer cuadro.** La métrica es el **primer cuadro**: ≤ 2 s desde que el almacén está listo
   (y ≤ 10 s de carga total, M10). No se añade el cuadro grueso previo.
2. **Memoria.** Tope **1 GB duro** (no adaptativo), configurable a la baja con
   `MINIGIS_STORE_BUDGET_MB`.
3. **Variables de entorno.** Sí: `MINIGIS_STORE_BUDGET_MB` y `MINIGIS_GEOPARQUET_BENCH_DATASET`
   forman parte del contrato documentado; `MINIGIS_GEOPARQUET_BUDGET_MS` ajusta la puerta.
4. **Tabla.** Se mantiene el tope de **100 000 filas** visibles con el aviso de «hay más»; no se
   pagina sobre los 711 574.
5. **Capa de referencia.** Puerta **opt-in** por variable de entorno; el `.parquet` no se
   versiona.

---

## 14. Anexo — Datos del archivo

| Dato | Valor |
|---|---|
| Archivo | `nz-river-centrelines-topo-150k.parquet` (226,5 MB) |
| Driver / capas | `Parquet`, 1 capa `nz-river-centrelines-topo-150k` |
| Elementos | 711 574 |
| Geometría | `Line String` (711 574 líneas, 0 puntos, 0 polígonos, 0 sin geometría) |
| Vértices | **13 491 596 reales** (22,1 M estimados sobre los primeros 512 elementos) |
| SRC | `EPSG:2193 — NZGD2000 / New Zealand Transverse Mercator 2000` |
| Extensión (2193) | 1 090 206,6 · 4 748 880,6 — 2 088 904,4 · 6 221 333,0 |
| Extensión (4326) | 166,266 · −47,231 — 179,460 · −34,025 |
| Campos | `t50_fid` (Integer) |
| FID de OGR | sin columna FID declarada (0…n−1) |
| Metadatos | `nz-river-centrelines-topo-150k.txt` y `.xml` (LDS), no usados |


---

## 15. Resultado (2026-11-21)

| Métrica | Antes | Después |
|---|---|---|
| Cuadro de paneo/zoom (mediana) | 21,3 s | **47-52 ms** |
| `--snapshot` 1600×900 (render) | 20,2 s | **57-64 ms** |
| `--snapshot` con zoom (render) | 11,7 s | **29 ms** |
| `--snapshot-window` sin tabla | 34,3 s | **4,86 s** |
| `--snapshot-window --with-attributes` | 531,4 s | **4,92 s** |
| `--identify-at` (consulta) | 576 ms | **160-250 ms** |
| `--check-geoparquet` | 1,66 s sin almacén | **5,34 s con 711 574 elementos** |
| Almacén (construcción / memoria) | — | **4,45 s / 388 MB** |
| RAM máxima del proceso | — | **732 MB** |

El detalle de la medición, las causas y los límites quedan en `RENDIMIENTO.md` §15. Las
decisiones tomadas (1 GB duro, 100 000 filas, puerta opt-in) están en la sección 13.
