# Rendimiento del despliegue cartográfico vectorial en MiniGIS

> Análisis, implementación y **medición** de las optimizaciones del dibujado vectorial
> (Qt 6 + GDAL), con banco de pruebas reproducible y puerta de regresión para CI.

Todo lo que aparece aquí está medido sobre este equipo con la compilación Release
existente. Los números no son estimaciones: cada tabla se puede reproducir con los
comandos de la sección 8.

---

## 1. Resumen

| Caso (1200×800, 20 cuadros de paneo/zoom mixto) | Motor clásico | Motor acelerado | Mejora |
|---|---|---|---|
| Capa completa en pantalla (12.000 polígonos, 186.782 vértices) | 2.616 ms · 0,4 fps | **45 ms · 22 fps** | **58×** |
| Cuadro de borrador durante el arrastre (mismo caso) | 2.589 ms | **18 ms · 56 fps** | **144×** |
| Vista a 1/8 de la capa (180 elementos) | 1.297 ms | **18 ms · 56 fps** | **72×** |
| Cuadro de borrador de esa vista | 1.448 ms | **3 ms · 333 fps** | **483×** |
| Zoom profundo (0,02°, 1 elemento visible) | 1.245 ms | **1 ms** | 1.200× |
| Capa completa reproyectándose a EPSG:25830 | 3.083 ms | **43 ms · 23 fps** | **72×** |
| Shapefile de 34 MB (misma geometría) | 2.561 ms | **47 ms · 21 fps** | **54×** |

Carga de la capa de 47,8 MB / 2,2 M de vértices: **~0,4 s** construyendo el índice
en un hilo de trabajo, con barra de progreso y sin bloquear la ventana (antes: la
ventana se quedaba congelada durante varios segundos y sin ninguna señal visual).

Trabajo realizado: **9 archivos de código nuevos** (más este documento) y **16
modificados**, con el motor clásico intacto y seleccionable (`--renderer legacy`) para
poder comparar y como red de seguridad. Incluye un banco de pruebas (`--bench`) y una
prueba de CTest que falla si una regresión de rendimiento supera el presupuesto.

Además, la sección 9 documenta el caso que motivó la última tanda de trabajo: dos
capas reales de Nueva Zelanda (**3,24 M de edificios** y **2,43 M de direcciones**)
que antes tardaban 70 segundos por cuadro (o quedaban directamente inutilizables) y
ahora responden en 13 ms a escala de ciudad y 76 ms con el país entero en pantalla.

---

## 2. Diagnóstico medido

### 2.1 Cómo se midió

Se generó un conjunto de datos sintético realista (12.000 parcelas poligonales con
184 vértices cada una, 2,2 M de vértices; 47,8 MB en GeoPackage, 34,3 MB en
Shapefile) con `scripts` fuera del repositorio, y se añadió al ejecutable un modo
de medida sin ventana:

```bat
minigis --bench 20 --bench-scenario mixed --renderer fast  -l parcelas.gpkg --size 1200x800
minigis --bench 20 --bench-scenario mixed --renderer legacy -l parcelas.gpkg --size 1200x800
```

`--bench` simula gestos reales (paneo que va y vuelve, zoom que acerca y aleja),
mide el coste de cada cuadro y lo desglosa en **consulta**, **construcción de
trazados** y **pintado**. Ese desglose es lo que permitió localizar el problema:
sin él, todas las hipótesis parecen razonables.

### 2.2 Dónde se iba el tiempo (motor clásico)

`MapRenderer::render()` recorre **la capa entera en cada cuadro**:

```cpp
OGR_L_ResetReading(layerHandle);
while (OGRFeatureH feature = OGR_L_GetNextFeature(layerHandle)) {   // 12.000 elementos
  OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
  // ...por cada vértice: OGR_G_GetX(i) / OGR_G_GetY(i)  -> 2,2 M de llamadas
  // ...y si hay reproyección: clone() + transform() + delete por elemento
}
```

Cinco causas, todas verificadas con medidas:

1. **Sin descarte por extensión visible.** Al acercarse a una zona que contiene el
   0,2 % del área, el tiempo solo bajaba de 2.611 ms a 1.260 ms: seguía leyendo y
   proyectando los 2,2 M de vértices para tirar el 99,8 %.
2. **Una llamada a la API C de OGR por vértice y por coordenada.** `OGR_G_GetX`
   resuelve el tipo de geometría en cada llamada (curvas, anillos, Z/M...).
3. **Clonado y reproyección por elemento, en cada cuadro.** Reproyectar costaba
   ~370 ms extra sobre 2,6 s (mismo trabajo repetido en cada fotograma).
4. **Estado de pintado por elemento.** `setPen()`, `setBrush()`, `setOpacity()` y un
   `QPainterPath` nuevo para cada uno de los 12.000 polígonos, además de
   `Antialiasing` + uniones redondeadas.
5. **Carga síncrona en el hilo de la GUI.** `VectorLayer::open()` (y, en el peor
   caso, `OGR_L_GetFeatureCount(...,1)`) se ejecutaba en el hilo principal: la
   ventana se congelaba y ni siquiera se pintaba el mensaje de "cargando".

### 2.3 Hallazgos específicos de Qt (los que cambiaron el diseño)

Estos se descubrieron midiendo variantes del mismo cuadro, y son la parte menos
intuitiva del trabajo:

| Variante medida (12.000 polígonos, 186.782 vértices) | Tiempo de pintado |
|---|---|
| Un solo `QPainterPath` con 12.000 subpaths | **7.696 ms** |
| Trocado en lotes de 256 subpaths | 1.032 ms |
| Trocado en lotes de 4 subpaths | **52 ms** |
| Trazo de 1 px con pluma normal, unión redondeada | 1.072 ms |
| Trazo de 1 px con pluma *hairline* (cosmética) | **216 ms** |
| Sin trazo (solo relleno) | 187 ms |
| Sin suavizado (relleno + trazo) | 295 ms |

Lectura: el coste del rellenado con suavizado de Qt crece **muy por encima de lo
lineal** con el número de subpaths del mismo trazado, y el contorneador (*stroker*)
con uniones redondeadas era más caro que toda la geometría. De ahí las dos
decisiones centrales del renderizador: **lotes pequeños** y **pluma hairline por
encima de un umbral**. (El filtro espacial y la simplificación son las otras dos,
y son las que atacan la causa 1 y 2.)

También se midió, para descartar hipótesis: la regla de relleno (par-impar frente a
`WindingFill`) no influye (< 2 %); el recorte por región de recorte no encarece el
pintado; y `/arch:AVX2`, `/fp:fast` y LTO **no cambian nada** en este perfil
(sección 7).

---

## 3. Arquitectura de la solución

```
                     HILO DE TRABAJO                          HILO DE LA GUI
  ┌───────────────────────────────────────────┐   ┌────────────────────────────────────┐
  │ VectorLayerLoader                         │   │ MainWindow                         │
  │  · VectorLayer::open()      (GDAL)        │   │  · barra de progreso + cancelar    │
  │  · GeometryStore::build()                 │──▶│  · canvas->addLayer()              │
  │      - lectura única y reproyección única │señ│ MapCanvas                          │
  │      - buffers planos de coordenadas      │ales│  · extent / zoom / paneo           │
  │      - R-tree STR (envolventes)           │   │  · modo borrador + temporizador    │
  │      - niveles Douglas-Peucker            │   │  · reutilización del cuadro previo │
  └───────────────────────────────────────────┘   │ FastMapRenderer                    │
                GeometryStore (inmutable) ───────▶│  · consulta R-tree por vista       │
                                                  │  · lotes de trazados + pintado     │
                                                  └────────────────────────────────────┘
```

Archivos nuevos:

| Archivo | Responsabilidad |
|---|---|
| `src/core/minigisgeometrystore.h/.cpp` | Almacén en memoria: buffers planos, R-tree STR, niveles de simplificación |
| `src/core/minigisfastrenderer.h/.cpp` | Renderizador acelerado: descarte por vista, lotes, escalón de calidad |
| `src/core/minigisvectorloader.h/.cpp` | Carga en segundo plano con progreso y cancelación |

Modificados (sin romper el camino clásico): `minigismapcanvas.*` (motor
seleccionable, modo borrador, reutilización de cuadro), `minigismaprenderer.*`
(filtro espacial opcional y punto de entrada para mezclar motores),
`minigisvectorlayer.h` (referencia al almacén), `minigismainwindow.*` (progreso),
`main.cpp` (`--bench`, `--renderer`, `--vertex-budget`, `--spatial-filter`),
`CMakeLists.txt` y los `CMakeLists.txt` de `core`, `gui` y `app` (banderas de
optimización), `tests/` (prueba de rendimiento).

---

## 4. Optimización de renderizado y memoria

### 4.1 No volver a leer la geometría: buffers planos

El almacén lee la capa **una sola vez** y guarda las coordenadas en un
`std::vector<double>` contiguo, con dos niveles de índices (`anillo → primer
vértice`), sin objetos OGR:

```cpp
struct Feature { double minX, minY, maxX, maxY; uint32_t firstRing, ringCount; };
struct Level   { std::vector<double> coords;        // x,y intercalados
                 std::vector<uint32_t> ringFirst;   // vértice inicial de cada anillo
                 std::vector<uint32_t> ringCount;   // 0 = anillo descartado
                 double tolerance; qint64 vertices; };
```

Ventajas frente a guardar `OGRGeometry*`: sin despacho virtual ni resolución de
tipo por vértice, recorrido secuencial (la caché de la CPU ve el siguiente vértice
en la misma línea), y 16 bytes por vértice. La reproyección también se hace una
única vez, en la construcción y en el hilo de trabajo.

**Error real encontrado y corregido**: el acumulador de coordenadas hacía
`coords.reserve(coords.size() + n)` en cada anillo. `reserve` con el tamaño exacto
obliga a reasignar y **copiar todo lo acumulado** en cada llamada, lo que convierte
la lectura en O(n²): 12.000 anillos suponían ~30 s de `memcpy`. Con el crecimiento
geométrico de `push_back`, la lectura pasó de **29.466 ms a 93 ms**.

### 4.2 Indexación espacial

**En memoria (R-tree STR empaquetado).** Se construye en una pasada con
ordenación por franjas (*Sort-Tile-Recursive*): ordena por centroide X, divide en
franjas, ordena cada franja por Y y empaqueta hojas de 16 elementos. Consulta
iterativa con pila reutilizada:

```cpp
store.query(viewMinX - margen, viewMinY - margen, viewMaxX + margen, viewMaxY + margen, candidatos);
// 12.000 elementos en la capa -> 12.000 candidatos con la capa completa,
//                                180 candidatos en una vista a 1/8, 1 en zoom profundo
```

El margen cubre el ancho del trazo, para que un elemento cuyo contorno entra en la
vista no se descarte. Detalle importante: los nodos internos guardan sus hijos en
un vector aparte (`mChildIds`) porque tras ordenar por centroide **los índices
dejan de ser consecutivos** y no se pueden deducir del primero (era un fallo real:
el R-tree devolvía la capa entera porque las envolventes se acumulaban entre
elementos).

**En el archivo (para capas que no caben en memoria).** El camino clásico admite
ahora el filtro espacial del propio driver:

```cpp
OGR_L_SetSpatialFilterRect(layerHandle, extent.xMin(), extent.yMin(), extent.xMax(), extent.yMax());
```

Medido en la vista a 1/8 con el motor clásico: **1.287 ms → 50 ms (26×)**. El
driver usa el R-tree del GeoPackage (o el índice del shapefile) y solo entrega los
elementos que cortan la vista. Es la red de seguridad para capas enormes, donde no
tiene sentido copiar la geometría a memoria.

### 4.3 Simplificación por nivel de detalle

Dos fases, porque Douglas-Peucker solo tiene un problema real: los anillos densos
degeneran en O(n²) y leer 2,2 M de vértices costaba 30 s de simplificación.

1. **Filtro radial O(n)**: se conserva un vértice solo si dista más de la
   tolerancia del último conservado. Es el criterio que usan los visores para
   reducir densidad y, sobre todo, deja la polilínea manejable.
2. **Douglas-Peucker** sobre los vértices supervivientes, con distancia al
   *segmento* (no a la recta infinita, que se dispara con segmentos degenerados) y
   pila explícita.

La tolerancia se deriva de la escala: `0,5 px` en calidad y `1,5 px` en borrador,
y se cuantiza en potencias de dos para poder cachear un nivel por escalón de zoom.
Los niveles se precalculan al cargar (6 niveles, ~335 ms medidos) y el resto se
construye bajo demanda con expulsión del nivel más lejano. Si la vista acumula
demasiados vértices, el renderizador **sube de nivel hasta cumplir el presupuesto**
(`600.000` vértices por defecto): el coste del cuadro queda acotado por diseño en
lugar de crecer sin límite.

### 4.4 Lotes de dibujado (el ajuste más rentable)

Un `QPainterPath` por capa y tipo, troceado adaptativamente:

```cpp
int featuresPerChunkFor(size_t candidateCount) { return candidateCount <= 512 ? 1 : 4; }
```

Con un único trazado para los 12.000 polígonos, el rellenado con suavizado de Qt
tardaba **7,7 s**; con lotes de 4, **52 ms** (148×). El valor no es arbitrario: la
tabla de la sección 2.3 muestra que con pocos elementos conviene el lote mínimo
(1) y con muchos, un lote algo mayor (4) para amortizar el coste fijo de cada
`drawPath`.

### 4.5 Escalón de calidad del trazo

```cpp
QPen strokePen(const Style &style, bool hairline)
{
  if (style.strokeWidth <= 0.0) return QPen(Qt::NoPen);
  if (hairline) return QPen(style.strokeColor, 0.0);   // cosmética: 1 px de dispositivo
  return QPen(style.strokeColor, style.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
}
```

- Con **muchos elementos** (≥ 2.000 dibujados) el contorno se traza *hairline*:
  Qt no pasa por el contorneador y el cuadro baja de 1.072 ms a 216 ms (5×). A 1 px
  de grosor la diferencia visual es mínima y, a cambio, el mapa se mueve.
- Con **pocos elementos** se mantiene la pluma normal con unión a inglete (evita
  generar un arco en cada vértice, −25 %).
- En **borrador** se usa siempre hairline.

**Corrección de relleno**: los anillos se normalizan en la lectura (exteriores en
un sentido, huecos en el contrario) y se pinta con `Qt::WindingFill`. Así los
huecos siguen restándose y, además, **los solapes entre elementos de la misma capa
se rellenan** en lugar de aparecer como agujeros, que es el comportamiento del
camino clásico con `OddEvenFill` por elemento.

### 4.6 Memoria

| Concepto | Valor medido |
|---|---|
| GeoPackage de entrada | 47,8 MB |
| Vértices indexados | 2.208.000 (nivel 0 sin simplificar) |
| Memoria del almacén (6 niveles) | 77,3 MB |
| Reparto de la construcción | lectura 93 ms · índice 2 ms · niveles 335 ms |

El almacén es inmutable tras construirse, así que se puede construir en un hilo y
consultar desde otro. Si la capa supera `vertexBudget` (8 M de vértices por
defecto), la construcción se cancela con un mensaje y la capa se dibuja con el
camino clásico (y, si se pide, con el filtro espacial del archivo). **No hay copia
silenciosa de memoria sin límite.**

### 4.7 Delegar en la GPU y renderizado por teselas

Lo anterior baja el coste por vértice, pero el suelo lo pone el rasterizador por
software de Qt. Se ve claramente en los dos regímenes medidos:

- **Régimen ligado a geometría** (capa completa): 45 ms para 186.782 vértices, y el
  desglose dice que 5 ms son nuestros (consulta + trazados) y ~40 ms el pintado.
- **Régimen ligado a píxeles** (mucho relleno en pantalla): la vista a 1/8 tarda 16 ms
  con solo 180 elementos, porque los polígonos cubren casi todo el lienzo. Aquí el
  coste es superficie rellenada con suavizado, no vértices.

Si hay que bajar de ahí, el camino es la GPU, y Qt Quick es el sitio natural para
hacerlo (sección 6.3). Dos alternativas sin cambiar de marco:

1. **Teselas pre-renderizadas** (*tile cache*): cachear la imagen de cada tesela por
   nivel/x/y y recomponer con `QPainter::drawImage`. Al desplazar, la mayor parte
   del lienzo se reutiliza; solo se rasterizan las teselas nuevas. Es la evolución
   natural de la reutilización de cuadro ya implementada (sección 5.3) y escala a
   varios niveles de zoom simultáneos.
2. **`QPainter::drawImage` de una capa simplificada + overlays nítidos**: útil si el
   detalle fino solo importa en una ventana de interés.

---

## 5. Interacciones fluidas (zoom y paneo)

### 5.1 Modo borrador

Durante el arrastre o la rueda se dibuja a **media resolución**, con tolerancia de
simplificación 1,5 px y trazo *hairline*. Al parar, un temporizador de 140 ms pide
el cuadro definitivo:

```cpp
void MapCanvas::beginInteraction()          // se llama en cada evento de zoom/paneo
{
  if (!mDraftMode) setDraftMode(true);      // -> update()
  mQualityTimer->start();                   // 140 ms: al expirar, setDraftMode(false)
}
```

No se redibuja "todo en cada cuadro" por dos motivos: el trabajo por cuadro está
acotado (presupuesto de vértices) y el coste se desplaza fuera del gesto (borrador
primero, calidad después). Medido: **18 ms de borrador** frente a 2.589 ms del
camino clásico, es decir 56 fps mientras se arrastra una capa de 47 MB.

### 5.2 Ajuste a la rejilla de píxeles

La extensión visible se alinea a la rejilla de píxeles (`snappedExtent`). Es un
ajuste de menos de un píxel, invisible, que evita que los símbolos "vibren" al
arrastrar y —lo importante— hace que el desplazamiento entre dos cuadros sea un
**número entero de píxeles**, requisito para lo siguiente.

### 5.3 Reutilización del cuadro anterior

Si el cambio de extensión es solo un desplazamiento a la misma escala, se copia el
cuadro anterior por líneas (`memcpy`, mismo formato y tamaño) y **solo se vuelve a
dibujar la franja descubierta**, en forma de L, mediante una región de recorte:

```cpp
const QRect covered = QRect(QPoint(dx, dy), size).intersected(QRect(QPoint(0, 0), size));
exposed = QRegion(QRect(QPoint(0, 0), size)).subtracted(QRegion(covered));
...
painter.setClipRegion(exposedRegion);
```

Se mantienen dos cachés (calidad y borrador) porque durante un arrastre se encadenan
cuadros de borrador y, al soltar, uno de calidad: con una sola entrada cada cambio
de nivel la invalidaría. Efecto medido: cuadros de paneo de 230 ms → **173 ms** en
calidad, y de 72 ms → **51 ms** en borrador antes de pasar al troceado adaptativo;
además, volver atrás (arrastrar y deshacer) es instantáneo.

### 5.4 Lo que **no** se hizo (y por qué)

- **No** se desactiva el suavizado en modo borrador: con el troceado adaptativo el
  borrador ya va a 56 fps en el peor caso, así que no merece la pena perder calidad.
  La opción está disponible (`FastMapRenderer::Options::antialiasing`) por si en
  pantallas 4K hace falta.
- **No** se redibuja en cada evento de ratón: `update()` de Qt ya agrupa los eventos
  de pintado y el temporizador de calidad evita encadenar cuadros completos.

### 5.5 Contrato de la caché de cuadro (y un fallo que costó caro)

Reutilizar el cuadro anterior tiene una trampa: si el estado de las capas cambia
(simbología nueva, capa oculta), el cuadro guardado deja de representar lo que hay que
ver. Con la comprobación inicial —mismo tamaño, mismo SRC, mismo encuadre— el lienzo
devolvía la imagen anterior **sin repintar**, así que cambiar el estilo de una capa o
desmarcar su visibilidad no se reflejaba en el mapa hasta mover o redimensionar la
vista.

La corrección no depende de que cada sitio que cambia la apariencia se acuerde de
invalidar la caché (ese fue justo el fallo): `VectorLayer` lleva un contador de
**revisión** que sube al cambiar la simbología, la visibilidad o el almacén, y la caché
guarda la revisión con la que se dibujó. Si no coincide, se redibuja entero. Además,
`MapCanvas::refreshMap()` invalida y repinta de forma explícita, que es lo que llaman la
ventana al aceptar el diálogo de estilo y al cambiar el check de visibilidad del
listado de capas.

Se añadió la prueba `--check-refresh` (y su equivalente en CTest, `minigis.redibujado`)
porque este fallo es invisible para cualquier prueba de rendimiento:

```bat
minigis --check-refresh -l data/sample/shapefiles/parcels.shp
  OK   cuadro estable con el mismo estado — 99229 pixeles pintados
  OK   la simbologia nueva se ve en el mapa — 101143 pixeles pintados
  OK   la capa oculta desaparece del mapa — 0 pixeles pintados
  OK   al volver a mostrarla se recupera el dibujo
```

Compara los cuadros píxel a píxel: primero comprueba que con el mismo estado la caché
sí se reutiliza (si no, la prueba no probaría nada), después cambia la simbología y
exige que el cuadro cambie, luego oculta la capa y exige que no quede ni un píxel
pintado, y por último la vuelve a mostrar y exige recuperar el cuadro anterior.
Comprobado que **detecta el fallo**: quitando la revisión de la clave de caché, falla
con `FALLO la simbologia nueva se ve en el mapa` y `FALLO la capa oculta desaparece del
mapa` (mismo número de píxeles que el cuadro de referencia: la imagen vieja).

Efecto secundario de revisar esto: `setDraftMode()` invalidaba las dos cachés al entrar
y salir del modo borrador, lo que tiraba justo la reutilización que se busca. Ahora cada
nivel de calidad conserva la suya, de modo que un arrastre encadena cuadros de borrador
reutilizados.

**Y dos fallos más graves, que aparecieron en uso real.** Los dos eran de la
reutilización del cuadro, y los dos se escaparon por la misma razón: **se medían
milisegundos, no píxeles**. Los bancos de prueba decían que el paneo iba más rápido
—y era verdad—, pero nadie comprobaba que la imagen fuera correcta.

**Fallo 1: la copia iba hacia el lado contrario.** Al desplazar el mapa aparecía una
costura vertical, con la parte copiada y la franja repintada mostrando zonas distintas.
Para un punto fijo del mapa,

```
pixel_cacheado = pixel_nuevo + (nuevo.xMin - cache.xMin) * escala
```

es decir, si `xMin` crece (la vista se mueve hacia el este) el contenido aparece **más a
la izquierda** en el cuadro nuevo; el código copiaba hacia la derecha, de modo que todo
el contenido copiado quedaba desplazado el doble de lo debido.

**Fallo 2: la franja a repintar quedaba del lado contrario.** Al corregir el signo, el
rectángulo "cubierto" seguía calculándose por separado (como el rectángulo desplazado)
en lugar de a partir de la zona realmente copiada. Los dos rectángulos dejaron de
coincidir: se copiaba una parte y se repintaba otra, y **quedaba sin pintar una banda
del ancho del desplazamiento** (el mapa se veía incompleto al arrastrar, con una banda
vacía a la derecha y abajo).

**Fallo 3, estructural: el muestreo dependía del encuadre.** En el modo envolventes la
nube de puntos se muestrea con un paso calculado a partir del número de elementos
visibles, que cambia al desplazar. El cuadro copiado y la franja nueva contenían puntos
distintos, así que la densidad no cuadraba (medido: 10.147 píxeles distintos de
400.000). La reutilización solo es válida si el dibujado nuevo es una **traslación
pura** del anterior, así que ahora el renderizador marca los cuadros que usan
decisiones dependientes del encuadre (muestreo de puntos, muestreo de envolventes o
subida de nivel por presupuesto de vértices) y el lienzo no los reutiliza. Además, el
escalón de calidad del trazo se cuantiza (escalones de 1024 elementos) para que no
oscile entre cuadros por unos pocos elementos de diferencia.

**El invariante que faltaba.** La prueba de redibujado comprueba ahora que
**reutilizar el cuadro anterior da exactamente lo mismo que redibujar desde cero**, en
las cuatro direcciones y diagonales, con desplazamientos fraccionarios (ejercitan el
ajuste a la rejilla de píxeles) y **grandes, de hasta 210 px** (hacen evidente cualquier
banda sin pintar), en calidad y en borrador. Antes comprueba que el propio dibujado es
determinista (dos dibujados del mismo estado dan lo mismo; si no, la comparación no
significaría nada) y **acerca la vista**, porque si el contenido no llega a los bordes
del lienzo una banda sin pintar en el borde no produce ninguna diferencia: ese fue
exactamente el punto ciego que dejó pasar el fallo 2.

```
OK   el paneo reutilizado coincide con el redibujado completo — 0 pixeles distintos de 400000
OK   el mismo estado dibuja lo mismo en borrador — 0 pixeles distintos
OK   el paneo en borrador coincide con el redibujado (media resolucion) — 0 pixeles distintos de 100000
```

Medido con los fallos reintroducidos a propósito, para comprobar que la prueba los
detecta: **15.240 píxeles distintos** con el signo invertido, **53.499** con el
rectángulo cubierto mal calculado (22 % del lienzo: la banda vacía) y **10.147** con el
muestreo dependiente del encuadre. En el camino de calidad la exigencia es exacta (0
píxeles); en el de borrador se tolera un 0,1 % porque al dibujar una franja recortada Qt
suaviza de forma ligeramente distinta los símbolos que caen justo sobre el borde del
recorte (medido: 5 píxeles de 60.000 en una capa de puntos, imperceptibles y solo en el
cuadro intermedio del arrastre). Una costura o una banda sin pintar producen miles de
píxeles, así que ese margen no debilita la prueba.

**Coste en rendimiento de las correcciones: ninguno apreciable.** La reutilización sigue
activa donde es válida (escala de ciudad en la capa de edificios: 12 ms por cuadro, 83
fps; capa sintética de 12.000 polígonos: 45 ms de calidad y 18 ms de borrador) y se
desactiva solo en las vistas muestreadas, donde el coste dominante es la consulta al
índice de todos modos (país completo: 69 ms, de los cuales 53 ms son la consulta).

---

## 6. Carga asíncrona y retroalimentación en la interfaz

### 6.1 El trabajador (`src/core/minigisvectorloader.h/.cpp`)

Un `QObject` que lanza el trabajo con `QtConcurrent::run` y comunica el avance con
señales. La ventana nunca se bloquea y las señales llegan al hilo de la GUI por
conexión en cola:

```cpp
void VectorLayerLoader::start(const QStringList &paths)
{
  if (paths.isEmpty() || mRunning.load()) return;
  mCancelled.store(false);
  mRunning.store(true);
  emit started(paths);
  QtConcurrent::run([this, paths]() { run(paths); });   // vuelve inmediatamente
}

bool VectorLayerLoader::reportProgress(int percent, const QString &message)
{
  emit progressChanged(percent, message);   // en cola hacia el hilo de la GUI
  return !mCancelled.load();                // false = cancelar
}
```

Señales que expone:

| Señal | Uso en la interfaz |
|---|---|
| `progressChanged(int, QString)` | `QProgressBar::setValue` + texto de estado |
| `layerReady(layer, bool first)` | añadir la capa al lienzo (encuadrando la primera) |
| `layerFailed(path, message, hint)` | aviso con causa y acción sugerida, sin abortar el resto |
| `finished(loaded, failed, cancelled)` | resumen y ocultar el indicador |

Reglas de hilos que respeta el diseño (y conviene no olvidar):

- **Un dataset de GDAL pertenece a un hilo**: cada capa se abre, se lee y se indexa
  dentro del mismo hilo de trabajo; solo después se entrega a la interfaz, que ya no
  comparte el dataset con el hilo de trabajo. La documentación de GDAL es explícita:
  salvo excepciones, ninguna función pública es segura entre hilos sobre la misma
  instancia, ni siquiera entre bandas o capas del mismo dataset.
- La cancelación es una bandera atómica que consultan los puntos de control (lectura
  de geometrías y niveles de detalle); una carga larga se puede abortar sin esperar.
- El progreso se emite cada 1.024 elementos y en cada nivel, así que no inunda el
  bucle de eventos. (Si se usara `QPromise` + `QFutureWatcher`, Qt ya limita la tasa
  de `progressValueChanged` para no saturar la GUI.)

Progreso real observado al abrir un GeoPackage de 47,8 MB y un shapefile de 34 MB
(`QT_LOGGING_RULES="minigis.loading=true"`):

```
progreso 0  % "Abriendo «heavy.gpkg» (1 de 2)..."
progreso 5  % "heavy.gpkg — Preparando la lectura de la capa..."
progreso 9  % "heavy.gpkg — Leyendo geometrias: 1024 elementos..."
...
progreso 41 % "heavy.gpkg — Construyendo el indice espacial (12000 elementos)..."
progreso 44 % "heavy.gpkg — Preparando niveles de detalle..."
progreso 50 % "heavy.gpkg — Capa preparada."
progreso 50 % "Abriendo «heavy.shp» (2 de 2)..."
```

### 6.2 La interfaz (Qt Widgets, que es lo que usa MiniGIS hoy)

En la barra de estado: etiqueta con el mensaje, `QProgressBar` porcentual y botón
**Cancelar**; los tres se ocultan al terminar. El lienzo sigue siendo usable
mientras carga, y las capas que ya están listas se dibujan aunque el resto siga
leyéndose.

```cpp
connect(mLoader, &VectorLayerLoader::progressChanged, this, &MainWindow::onLoadProgress);
connect(mLoader, &VectorLayerLoader::layerReady,      this, &MainWindow::onLayerLoaded);
connect(mLoader, &VectorLayerLoader::layerFailed,     this, &MainWindow::onLayerLoadFailed);
connect(mLoader, &VectorLayerLoader::finished,        this, &MainWindow::onLoadFinished);
```

`onLoadProgress` solo actualiza la barra y el texto; `onLayerLoaded` añade la capa
al lienzo (`mCanvas->addLayer(layer, first || mCanvas->layers().empty())`), lo que
mantiene el orden de apilado y encuadra la primera capa. Los modos de captura
(`--snapshot-window`) esperan con un **bucle de eventos anidado**
(`MainWindow::waitForLoading`), de modo que las pruebas ejercitan exactamente el
mismo camino asíncrono que la aplicación real.

### 6.3 Si la interfaz fuera QML

Dos avisos antes de nada, porque afectan al plan:

1. **MiniGIS hoy es Qt Widgets, no QML.** No hay ningún `.qml` en el proyecto. Lo
   bueno es que el núcleo es agnóstico de la interfaz: `GeometryStore`,
   `VectorLayerLoader` y la simplificación no dependen de widgets y se reutilizan
   tal cual desde QML.
2. **`QQuickPaintedItem` es el equivalente QML de lo implementado** (QPainter sobre
   una imagen) y sirve para portar sin reescribir el renderizador, pero arrastra el
   coste documentado de subir la textura en cada cuadro ("*esta operación puede ser
   lenta si el elemento es grande*") y su `RenderTarget::FramebufferObject` se
   ignoraba en Qt 6.0–6.8 (solo acelera desde 6.9 y solo con OpenGL). Para rasterizar
   en GPU de verdad hay que usar el grafo de escena.

**Paso 1 — Portar con el renderizador actual (rápido y seguro).** Un `QQuickItem`
que pinta el `QImage` del `FastMapRenderer`:

```cpp
// mapitem.h
class MapItem : public QQuickPaintedItem
{
  Q_OBJECT
  Q_PROPERTY(qreal scaleUnitsPerPixel READ mapUnitsPerPixel NOTIFY frameRendered)
public:
  MapItem(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent)
  {
    setRenderTarget(QQuickPaintedItem::Image);      // Qt 6: siempre QImage
    setOpaquePainting(true);
    setAntialiasing(true);
    m_loader = new minigis::VectorLayerLoader(this);
    connect(m_loader, &minigis::VectorLayerLoader::progressChanged,
            this, &MapItem::progressChanged);       // -> ProgressBar en QML
    connect(m_loader, &minigis::VectorLayerLoader::layerReady, this,
            [this](const std::shared_ptr<minigis::VectorLayer> &layer, bool first) {
              m_layers.push_back(layer);
              if (first) m_extent = layer->extentInEpsg(m_epsg);
              update();                              // -> updatePaintNode / paint
            });
    connect(this, &QQuickItem::widthChanged,  this, &MapItem::update);
    connect(this, &QQuickItem::heightChanged, this, &MapItem::update);
  }

  Q_INVOKABLE void openLayers(const QUrl &file) { m_loader->start({file.toLocalFile()}); }

  // Durante el gesto se dibuja en borrador; al parar, calidad.
  Q_INVOKABLE void beginInteraction() { m_draft = true;  update(); m_qualityTimer.start(); }
  Q_INVOKABLE void endInteraction()   { m_draft = false; update(); }

signals:
  void progressChanged(int percent, const QString &message);
  void frameRendered();

protected:
  void paint(QPainter *painter) override
  {
    const QSize size(int(width()), int(height()));
    m_lastFrame = m_canvasRenderer.render(m_layers, m_extent, size, m_background, m_draft);
    painter->drawImage(0, 0, m_lastFrame);
  }

private:
  minigis::FastMapRenderer m_canvasRenderer;
  minigis::VectorLayerLoader *m_loader = nullptr;
  std::vector<std::shared_ptr<minigis::VectorLayer>> m_layers;
  minigis::Rectangle m_extent;
  QTimer m_qualityTimer;          // 140 ms: single shot -> m_draft = false; update()
  int m_epsg = 4326;
  bool m_draft = false;
  QImage m_lastFrame;
};
```

```qml
// MapView.qml - gestos como los hace el propio Map de Qt Location
import QtQuick
import QtQuick.Controls

Item {
    id: root

    MapItem {
        id: map
        anchors.fill: parent

        DragHandler {
            // Durante el gesto: cuadro de borrador. Al soltar: calidad.
            onActiveChanged: active ? map.beginInteraction() : map.endInteraction()
            onTranslationChanged: (delta) => map.pan(-delta.x, -delta.y)
        }
        WheelHandler {
            onWheel: (event) => { map.beginInteraction(); map.zoomBy(Math.pow(1.0015, event.angleDelta.y)) }
        }
        PinchHandler {
            onScaleChanged: (delta) => { map.beginInteraction(); map.zoomBy(1 / delta) }
            grabPermissions: PointerHandler.TakeOverForbidden
        }
    }

    // Retroalimentacion de carga, sin bloquear: la senal viene del hilo de trabajo.
    Rectangle {
        anchors.centerIn: parent
        width: parent.width * 0.5
        height: column.implicitHeight + 32
        radius: 6
        color: Qt.rgba(1, 1, 1, 0.92)
        border.color: "#c8ccd0"
        visible: map.busy
        opacity: visible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }

        Column {
            id: column
            anchors.centerIn: parent
            spacing: 12

            BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: map.busy }
            Text { text: map.loadMessage; font.pixelSize: 13 }
            ProgressBar {
                width: 260
                from: 0; to: 100
                value: map.loadPercent          // 0..100 desde VectorLayerLoader
            }
            Button { text: qsTr("Cancelar"); onClicked: map.cancelLoading() }
        }
    }
}
```

El pinzado se apoya en `PinchHandler`/`WheelHandler`/`DragHandler`, que es lo que usa
el `Map` de Qt Location en Qt 6 (`MapGestureArea` se eliminó). Nótese el patrón
`beginInteraction()`/`endInteraction()`: es el mismo "borrador primero, calidad al
parar" de la sección 5.1, y es lo que mantiene los 60 fps en pantallas táctiles.

**Paso 2 — GPU de verdad (`updatePaintNode` + `QSGGeometryNode`).** Solo tiene
sentido para el régimen ligado a píxeles (relleno). Lo que hay que saber antes de
empezar, verificado en la documentación de Qt 6:

- `QSGRenderNode` está documentado como "*significativamente mejor que las
  alternativas basadas en texturas*" porque evita el render target intermedio y el
  texturado posterior. `QQuickFramebufferObject` es hoy una **clase heredada y solo
  OpenGL**; su sustituto portable es `QQuickRhiItem`.
- **Qt no expone ningún teselador público**: hay que triangular a mano con
  `QPainterPath::toFillPolygons()` (polígonos, no triángulos) o con
  `QPainterPathStroker` para los contornos. `Shape.CurveRenderer` de Qt Quick Shapes
  sí tesela en GPU y "*evita el coste de reteselar al hacer zoom*", pero está pensado
  para pocas formas, no para 12.000.
- En `QSGGeometry`, `lineWidth == 1.0` es lo único garantizado entre backends: las
  líneas gruesas hay que emitirlas como triángulos.
- Los *atlases* importan para el *batching*: dos materiales con texturas distintas no
  se pueden agrupar; con una textura común y `setSourceRect()` por tesela, sí.
- En Windows el backend por defecto de Qt Quick es **Direct3D 11**; se puede forzar
  con `QSG_RHI_BACKEND` o `QQuickWindow::setGraphicsApi()`.
- Para no reasignar vértices cada cuadro: `setVertexDataPattern(StaticPattern)` +
  `markVertexDataDirty()`, y en Qt 6.10+ `setVertexCount()`/`setIndexCount()` para
  cambiar el número de vértices sin reasignar.

Con eso, el `GeometryStore` actual sirve tal cual: la consulta R-tree da los
elementos visibles, el nivel de detalle da los vértices ya simplificados y solo hay
que triangular y subir buffers. Lo que **no** conviene es envolver el lienzo entero
en una capa (`layer.enabled`): la documentación de Qt avisa de que una capa ocupa
`ancho × alto × 4` en GPU y **no se puede agrupar**, y de que el coste de renderizar
a un objetivo intermedio y luego mezclarlo suele ser mayor que dibujar el contenido
normalmente.

---

## 7. Ajustes de compilación (CMake / MSVC)

### 7.1 Lo que se añadió

En `CMakeLists.txt`, una interfaz `MiniGIS::optimizations` enlazada por `core`,
`gui` y el ejecutable, con opciones explícitas y documentadas:

| Opción | Banderas | Por defecto |
|---|---|---|
| `MINIGIS_LTO` | `/GL` + `/LTCG /OPT:REF /OPT:ICF` en Release | `ON` |
| `MINIGIS_AVX2` | `/arch:AVX2` | `OFF` (requiere CPU con AVX2) |
| `MINIGIS_FAST_MATH` | `/fp:fast` | `OFF` (relaja NaN/Inf) |
| siempre en Release | `/Oi /Gy /Zc:inline` | — |

`/O2 /Ob2 /DNDEBUG` ya venían de `CMAKE_BUILD_TYPE=Release`.

### 7.2 Lo que se midió (y sorprende)

Compilé un segundo árbol con `/O2 /Ob2 /DNDEBUG /GL /fp:fast /arch:AVX2` (+
`/LTCG /OPT:REF /OPT:ICF` al enlazar; verificado en `build.ninja`) y comparé el mismo
banco de pruebas:

| Compilación | Calidad (mediana) | Borrador | Pintado | Trazados | Almacén |
|---|---|---|---|---|---|
| Release `/O2` (Qt Creator) | 169 ms | 39 ms | 159 ms | 5,6 ms | 429 ms |
| Release + LTO/AVX2/fp:fast | 169 ms | 40 ms | 163 ms | 5,5 ms | 404 ms |
| Release `/O2` (repetido) | 161 ms | 41 ms | 156 ms | 5,5 ms | 421 ms |
| Release + LTO/AVX2/fp:fast (rep.) | 157 ms | 39 ms | 158 ms | 5,4 ms | 415 ms |

**No hay diferencia.** Y tiene una explicación clara, que es la conclusión útil de
esta sección: el 90 % del tiempo de un cuadro se ejecuta **dentro del rasterizador de
Qt** (`Qt6Gui.dll`), que ya viene compilado con sus propias banderas; nuestras
banderas solo afectan al 10 % restante (consulta + construcción de trazados), que ya
cuesta 5 ms. Por eso:

- LTO se deja **activado** (no cuesta y ayuda a incorporar el código de `minigis_core`
  en el ejecutable), pero no se debe vender como una optimización de rendimiento:
  aquí es neutro.
- AVX2 y `fp:fast` quedan **desactivados por defecto**: aportan ruido de precisión o
  requisitos de CPU sin ganancia medible en este perfil. Si algún día se escribe un
  teselador propio o se vectoriza la simplificación con más peso, se pueden activar y
  volver a medir.

**Dónde sí hay margen: PGO (optimización guiada por perfil).** Es la única palanca de
compilación que actúa sobre el código de Qt que ejecutamos… salvo que Qt no se
recompila; pero sí mejora nuestro 10 % y, sobre todo, la disposición del código en
el binario. Queda documentada en `CMakeLists.txt` con las dos fases:

```bat
:: 1) instrumentar y recoger un perfil real de dibujado
cmake --preset windows-msvc-release -DCMAKE_CXX_FLAGS="/GENPROFILE" ^
      -DCMAKE_EXE_LINKER_FLAGS="/LTCG:PGINSTRUMENT" -DMINIGIS_LTO=OFF
ninja && bin\minigis.exe --bench 60 --bench-scenario mixed -l parcelas.gpkg --size 1600x900
:: 2) recompilar usando el perfil
cmake --preset windows-msvc-release -DCMAKE_CXX_FLAGS="/USEPROFILE" ^
      -DCMAKE_EXE_LINKER_FLAGS="/LTCG:PGOptimize"
ninja
```

**Otras dos recomendaciones de compilación/despliegue:**

- **Nunca distribuir Debug.** Además de `/Od`, un build Debug de Qt enlaza con las
  DLL de depuración y con `Q_ASSERT` activos: en este perfil la diferencia es de
  órdenes de magnitud. Los presets ya incluyen `windows-msvc-release`.
- **Asignador alternativo (`mimalloc`/`tcmalloc`)** para las rutas con muchas
  asignaciones pequeñas (creación/destrucción de `QPainterPath` y de buffers de
  trazado). Es una dependencia externa y no se ha medido aquí, así que queda como
  recomendación a evaluar, no como certeza.

---

## 8. Cómo reproducir las medidas y puerta de regresión

### 8.1 Generar un conjunto de datos pesado

```bat
:: 12.000 polígonos, ~2,2 M de vértices: GeoJSON -> GeoPackage y Shapefile
python make_heavy.py heavy.geojson
ogr2ogr -f GPKG heavy.gpkg heavy.geojson -nln heavy -lco SPATIAL_INDEX=YES
ogr2ogr -f "ESRI Shapefile" heavy_shp heavy.geojson -nln heavy
```

### 8.2 Medir

```bat
:: desglose por cuadro: consulta, construcción de trazados y pintado
minigis --bench 20 --bench-scenario mixed --renderer fast -l heavy.gpkg --size 1200x800 -V

:: comparar con el motor clásico (y con el filtro espacial del archivo)
minigis --bench 20 --bench-scenario mixed --renderer legacy -l heavy.gpkg --size 1200x800
minigis --bench 20 --bench-scenario mixed --renderer legacy --spatial-filter -l heavy.gpkg ^
        --extent -3.5,39.5,-3.0,39.9

:: salida para máquinas (CI, hojas de cálculo)
minigis --bench 20 --json --bench-scenario pan -l heavy.gpkg | jq .quality
```

### 8.3 Puerta de regresión en CI

`--bench-budget <ms>` hace que el ejecutable salga con **código 6** si el p90 por
cuadro supera el presupuesto; el código de salida es un contrato estable (el
`--help` lo documenta). CTest ya incluye:

- `minigis.bench`: valida el contrato de la salida JSON sobre los datos de ejemplo
  (diminutos; detecta roturas, no mide rendimiento).
- `minigis.rendimiento`: la puerta real. Se activa indicando una capa pesada:

```bat
cmake --preset windows-msvc-release -DMINIGIS_BENCH_DATASET=C:/datos/parcelas.gpkg ^
      -DMINIGIS_BENCH_BUDGET_MS=150
ctest --preset windows-msvc-release -R minigis.rendimiento
```

El presupuesto por defecto de la prueba (150 ms) es holgado a propósito, porque el
**p90** tiene picos puntuales (medido: 90 ms de p90 frente a 45 ms de mediana con el
conjunto de 12.000 elementos): corresponden al primer cuadro de un nivel de detalle
nuevo y están en la lista de trabajo pendiente (punto 1 de la sección 9). Aun así,
150 ms detecta cualquier regresión al comportamiento anterior, que estaba en
2.616 ms (17× de margen). Para una medida de rendimiento fina, usar la mediana del
JSON (`--json | jq .quality.median_ms`), que es mucho más estable que el p90.

### 8.4 Interruptores de diagnóstico útiles

| Interruptor | Para qué |
|---|---|
| `--renderer auto\|legacy\|fast` | elegir o comparar motores (en la app, `auto`) |
| `--vertex-budget <n>` | presupuesto de vértices por cuadro (0 = sin límite) |
| `--spatial-filter` | filtrar en origen con el índice del archivo (capas enormes) |
| `--read-budget <n>` | geometrías leídas del archivo por cuadro en el modo envolventes |
| `--index-shp <archivo.shp>` | diagnostica el índice de envolventes y verifica atributos y envolventes contra GDAL |
| `--with-attributes` | incluye la tabla de atributos en la captura de `--snapshot-window` |
| `--check-refresh` | comprueba que el mapa se redibuja al cambiar simbología o visibilidad, y que el paneo reutilizado no deja costuras ni bandas sin pintar |
| `-V` | por cuadro: consulta, trazados, pintado, candidatos, vértices y nivel |
| `QT_LOGGING_RULES="minigis.loading=true"` | traza de la carga en segundo plano |

---

## 9. Caso real: capas masivas (nz-building-outlines y nz-addresses)

Prueba de carga sobre datos reales de LINZ (Nueva Zelanda), no sintéticos:

| Capa | Elementos | Geometria | `.shp` | `.dbf` |
|---|---|---|---|---|
| `nz-building-outlines` | 3.236.141 | poligonos (~10 vertices) | 584 MB | 1,1 GB |
| `nz-addresses` | 2.429.477 | puntos | 65 MB | 1,47 GB |

### 9.1 Lo que pasaba

`nz-building-outlines` **se abria** (0,1 s) pero no se podia usar: el almacen en
memoria se rechazaba por presupuesto (~32 M de vertices, cuatro veces el limite de
8 M) y la capa caia al camino clasico, que tardaba **70 s por cuadro** (y 14,5 s
incluso activando el filtro espacial del archivo, porque un shapefile sin `.qix`
obliga a GDAL a recorrer los 3,24 M de registros en cada consulta). Es decir: la
ventana quedaba congelada y se percibia como "no carga".

### 9.2 Que se hizo: modo envolventes

El formato `.shp` permite leer la envolvente de cada elemento **sin leer su
geometria** (8 bytes de cabecera de registro + 4 de tipo + 32 de envolvente). Con ese
recorrido secuencial se construye un indice de solo envolventes y la geometria se lee
despues, bajo demanda, solo de lo que se ve:

| | Geometria completa | Solo envolventes |
|---|---|---|
| Memoria (3,24 M de elementos) | ~750 MB | **146 MB** |
| Construccion | rechazada por presupuesto | **1,1 s** (recorrido 0,35 s + indice 0,71 s) |
| Lectura por cuadro | imposible | solo lo visible, con cache entre cuadros |

El indice se construye en el hilo de trabajo con progreso, y cada elemento leido se
guarda en una cache con presupuesto (300.000 elementos), de modo que al desplazar solo
se leen las geometrias nuevas: en un paneo de 5 cuadros se leyeron **1.651
geometrias** (unas 330 por cuadro) en lugar de releer lo visible.

### 9.3 Resultados

| Caso | Antes | Ahora |
|---|---|---|
| Ciudad (2x2 km, 1.619 edificios) | 70 s (lectura completa) | **13 ms · 77 fps** |
| Ciudad, cuadro de borrador | 70 s | **3 ms · 333 fps** |
| Pais completo (3,24 M de candidatos) | 81 s (con lecturas dispersas) | **76 ms · 13 fps** |
| Indice + carga de la capa | — | **1,1 s** en segundo plano con progreso |

El pais completo ya no lee geometria: a esa escala un edificio ocupa menos de una
milesima de pixel, asi que se dibuja **el centro de su envolvente como punto** (segun
`minimumGeometryPixels`), que no cuesta I/O y produce un mapa de densidad de
edificacion legible. Con el suavizado activo, 150.000 puntos costaban 200 ms de
cuadro; con pluma cosmetica y sin suavizado, decenas de milisegundos.

`nz-addresses` (puntos) si cabe en memoria, pero aparecieron dos problemas: el **DBF
de 1,47 GB** se parseaba al indexar (51 s de lectura para algo que no necesita
atributos) y los **puntos no se pueden simplificar**, asi que los niveles de detalle
eran copias exactas de los datos crudos (40 MB por nivel). Corregido:

| | Antes | Ahora |
|---|---|---|
| Construccion del almacen | 52 s · 240 MB | **13,4 s · 184 MB** |
| Pais completo | 403 ms · 2,5 fps | **111 ms · 9 fps** |
| Auckland (5 km, 7.047 direcciones) | 41 ms | **39 ms · 26 fps** |

(los 12,9 s restantes son GDAL leyendo 2,43 M de puntos; es coste de una sola vez, en
segundo plano y con barra de progreso).

### 9.4 Herramienta de diagnostico anadida

`--index-shp` recorre el `.shp`, construye el indice y **verifica las envolventes
contra GDAL elemento a elemento** (comparando el FID del indice con la geometria que
devuelve OGR). Fue lo que permitio detectar que el tipo de forma de la cabecera va en
*little-endian* (leerlo en big-endian daba `0x05000000` en lugar de `5`):

```bat
minigis --index-shp "E:\datos\nz-building-outlines.shp"
  Elementos indexados : 3236141
  Elementos de la capa: 3236141
  Recorrido          : 584.1 MB en 316 ms
  Memoria del indice : 123.4 MB
  Verificacion       : 8 elementos comparados con GDAL, 0 discrepancias, diferencia maxima 0
```

### 9.5 Tres fallos propios detectados en esta prueba

- El bucle de dibujado esperaba en `entryFirstRing` el indice del primer **anillo** y
  `acquireSubset` guardaba el del primer **vertice**: lectura fuera de los arrays
  (violacion de acceso). Lo cazo la prueba con esta capa.
- El intento de almacen en memoria leia ~8 M de vertices (7 s) antes de rendirse.
  Ahora se estima el total con los primeros 512 elementos y se aborta en
  milisegundos: la construccion del indice de la capa de edificios paso de **8,7 s a
  1,1 s**.
- **La tabla de atributos mostraba "(nulo)" en todas las filas.** Para no leer un DBF
  de 1,1 GB, la construccion del almacen ignora temporalmente todos los campos
  (`OGR_L_SetIgnoredFields`) y los restaura al terminar; pero la comprobacion temprana
  del punto anterior sale de la funcion con un `return` anticipado, y ese camino **no
  restauraba nada**: la capa quedaba con todos los campos ignorados durante el resto
  de la sesion. La restauracion se hizo con un guardian RAII (`IgnoredFieldsGuard`),
  que se ejecuta en todas las salidas, y `--index-shp` comprueba ahora el contrato
  leyendo 259 filas (tres de muestreo mas un bloque contiguo de 256, que es lo que se
  ve al abrir la tabla).

  Es un fallo que no aparece en ninguna prueba de rendimiento y si en el uso real, asi
  que se anadio la prueba de CTest `minigis.indice-envolventes`, que falla si la capa
  se queda sin atributos. Comprobado que **detecta el fallo**: reintroduciendolo a
  proposito, la prueba informa de `Atributos tras indexar: 15 de 15 filas vacias`.

### 9.6 Limites de este modo

- Solo para **ESRI Shapefile en disco** (es el formato cuyo indice de envolventes se
  puede leer sin GDAL). Para otros formatos se mantiene el camino clasico, con el
  filtro espacial del archivo si se pide (`--spatial-filter`).
- El almacen **presta** la capa OGR (no es dueno del dataset): la capa debe seguir
  viva mientras se dibuja.
- A escalas muy lejanas la capa se degrada a nube de puntos: es una decision
  explicita y configurable (`minimumGeometryPixels`, `dotBudget`), no un fallo
  silencioso.
- El indice de 3,24 M de envolventes ocupa 146 MB y su consulta cuesta ~50 ms por
  cuadro (recorrer 3,24 M de envolventes); bajar de ahi exigiria un indice
  multinivel que responda con nodos gruesos en lugar de elemento a elemento.

---

## 10. Trabajo pendiente, por orden de rentabilidad

1. **Picos del primer cuadro de cada nivel de detalle** (p90 90 ms frente a mediana
   45 ms). Construir los niveles que falten **en segundo plano** y, mientras tanto,
   dibujar con el nivel disponible más cercano (refinamiento progresivo, como los
   trabajos de renderizado en paralelo de QGIS). Elimina tanto el pico como el tirón
   al visitar por primera vez un nivel nuevo.
2. **Teselas (*tile cache*)** para que el paneo no rasterice nada nuevo y para
   reutilizar entre niveles de zoom. Es la continuación natural de la reutilización
   de cuadro ya implementada.
3. **GPU (Qt Quick + `QSGGeometryNode`/`QSGRenderNode`)**: es la única salida al
   régimen ligado a píxeles. Requiere teselador propio, porque Qt no expone ninguno.
4. **Tabla de atributos**: `AttributeTableModel` pide los valores fila a fila
   (`OGR_L_GetFeature` por fila), es decir N+1 consultas al mover la barra. Con
   `OGR_L_SetNextByIndex` y lotes, o cacheando la capa en el `GeometryStore`, se
   arregla el otro punto que se nota lento con archivos grandes.
5. **`featureCount()` bajo demanda**: `displayName()` y la barra de estado lo llaman
   al cargar; con `OGR_L_GetFeatureCount(...,1)` fuerza un recorrido completo en los
   drivers que no lo tienen calculado. Debe ser explícito ("calcular recuento").
6. **Modo streaming** para capas por encima del presupuesto de memoria: filtro
   espacial del archivo por cuadro (ya disponible con `--spatial-filter`) + límite de
   elementos, en lugar de copiar la geometría a memoria.
7. **Símbolos de punto con sello cacheado** (una textura por estilo en lugar de
   construir trazados de elipse) y, si se añaden, **etiquetas**: hoy no hay
   renderizado de texto en el mapa, y las etiquetas son el siguiente orden de
   magnitud en complejidad.
8. **`mimalloc`** como asignador, a medir.

---

## 11. Límites y diferencias visuales (para que no sorprendan)

- **Trazos *hairline* por encima de 2.000 elementos dibujados**: el contorno sigue
  midiendo 1 px, pero se traza sin suavizado (líneas rectas con dientes en diagonales).
  Es el precio de pasar de 1.072 ms a 216 ms por cuadro. Con pocos elementos se
  mantiene el suavizado. Ajustable con `FastMapRenderer::Options::hairlineFeatureThreshold`.
- **Solapes dentro de la misma capa**: con `WindingFill` se rellenan (unión); el
  camino clásico, con `OddEvenFill` por elemento, mostraba un hueco. Es una mejora,
  pero es un cambio de comportamiento.
- **Simplificación de hasta 0,5 px** en calidad y 1,5 px en borrador: en el cuadro
  final la forma puede diferir en el orden de medio píxel. Los anillos pequeños
  nunca se descartan por completo.
- **Anillos interiores y polígonos con huecos**: verificados (se conservan
  correctamente, junto con el sentido de los anillos).
- **Zoom muy profundo** (extensiones menores que 1/32 de la capa): se dibujan los
  datos sin simplificar, porque quedan pocos elementos en pantalla.
- **El almacén se construye para un SRC de destino**. Si se cambiara el SRC del
  lienzo, las capas ya indexadas dejan de usarse y se dibujan por el camino clásico
  (hoy la aplicación no ofrece cambiar el SRC desde la interfaz).
- **El motor acelerado es el de por defecto** (`RendererBackend::Auto`) y las capas
  que aún no tienen almacén (mientras cargan, o si no cupieron en memoria) se
  dibujan con el clásico, respetando el orden de apilado.
- **Modo envolventes**: a escalas muy lejanas una capa masiva se dibuja como nube de
  puntos (envolventes) en lugar de geometría. Es visible y configurable.
- **No implementado**: teselas, GPU y etiquetas.

---

## 12. Archivos del cambio

**Nuevos**

```
src/core/minigisgeometrystore.h    src/core/minigisgeometrystore.cpp
src/core/minigisfastrenderer.h     src/core/minigisfastrenderer.cpp
src/core/minigisvectorloader.h     src/core/minigisvectorloader.cpp
src/core/minigisshapefileindex.h   src/core/minigisshapefileindex.cpp
RENDIMIENTO.md                     este documento
```

**Modificados**

```
CMakeLists.txt                     find_package(Qt6 ... Concurrent), interfaz MiniGIS::optimizations, PGO documentado
README.md                          seccion de rendimiento y banco de pruebas
src/core/CMakeLists.txt            fuentes nuevas + Qt6::Concurrent + optimizaciones
src/gui/CMakeLists.txt             optimizaciones
src/app/CMakeLists.txt             optimizaciones en el enlazado
src/core/minigismaprenderer.h/.cpp filtro espacial opcional, paintLayerInto() para mezclar motores
src/core/minigisvectorlayer.h/.cpp referencia al GeometryStore, envolvente por elemento (--index-shp)
src/gui/minigismapcanvas.h/.cpp    motor seleccionable, modo borrador, presupuesto, reutilizacion de cuadro
src/gui/minigismainwindow.h/.cpp   carga asincrona, barra de progreso, cancelar, espera en los modos de captura
src/app/main.cpp                   --bench, --bench-scenario, --bench-budget, --renderer, --vertex-budget,
                                   --spatial-filter, --read-budget, --index-shp
tests/CMakeLists.txt               pruebas minigis.bench y minigis.rendimiento
tests/run_cli_test.cmake           modo bench (contrato JSON + presupuesto)
```
