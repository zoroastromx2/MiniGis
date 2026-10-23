# Spec: Identificar elementos geográficos (consulta por clic)

> Módulo: `identificar` · Estado: **implementado y verificado** (2026-10-22) · Fecha: 2026-10-22
>
> Aprobado con estas decisiones: **dock** no modal, la capa seleccionada
> **limita** el ámbito y los valores de píxel del ráster quedan **fuera**. El
> plan técnico y las tareas están en `tasks/plan.md` y `tasks/todo.md`.
>
> Verificado el 2026-10-22 con compilación limpia del preset `windows-msvc-release`:
> **18/18** pruebas, `--check-identify` con **23/23** comprobaciones,
> `--identify-at --json` (punto, capa limitada, UTM y hueco) y la captura de la
> interfaz con el panel y el marcador.

---

## 1. Objetivo

Hoy un clic izquierdo sobre el lienzo solo prepara un paneo: **no hay forma de
preguntar «¿qué es esto?»** sobre lo que se ve. Para saber a qué parcela
pertenece un polígono hay que abrir la tabla de atributos y buscar la fila a
mano; para saber qué ciudad es un punto, calcular coordenadas en la barra de
estado y compararlas. Eso hace que revisar datos sea lento y propenso a error.

Este cambio añade **Identificar**: un clic sin arrastre sobre el mapa muestra una
ventana con la información del elemento —o de los elementos— que hay bajo el
clic. Dos reglas gobiernan la consulta:

1. **La capa seleccionada en el árbol manda.** Si hay una capa seleccionada, la
   respuesta es la de esa capa; si no hay ninguna, se consultan todas las capas
   visibles.
2. **«Cerca» es una distancia en pantalla.** El radio de búsqueda se mide en
   píxeles y se convierte con la misma escala que usa el dibujado, de modo que
   un clic a 4 px del borde identifica el elemento a cualquier zoom.

**Usuario:** quien revisa datos en MiniGIS (una parcela junto a una ortofoto, una
ciudad sobre el mapa político, una línea de drenaje) y necesita identificar lo
que está viendo sin abrir QGIS ni recorrer una tabla de miles de filas.

**Criterio de éxito del producto:** un clic sobre un elemento muestra su capa,
su identificador y todos sus campos; con una capa seleccionada la respuesta es
solo de esa capa y sin selección aparecen todas las capas cercanas ordenadas por
apilado y distancia; y todo ello se puede comprobar sin ventana, con el mismo
estilo de contrato que ya usan `--check-layers` y `--check-raster`.

---

## 2. Estado de partida (verificado leyendo el código)

Estas comprobaciones se hicieron sobre el repositorio antes de escribir el spec.

| Comprobación | Resultado |
|---|---|
| ¿Existe ya «identificar»? | **No.** No hay `minigisidentify*`, ni opciones `--identify*`, ni señales de clic en el lienzo |
| Clic izquierdo actual | Solo inicia paneo (`minigismapcanvas.cpp:814-827`) y lo termina al soltar (`856-867`); no emite nada |
| Doble clic | Hace zoom (`minigismapcanvas.cpp:869-879`) |
| Selección del árbol | `LayerPanel::onSelectionChanged` emite `currentLayerChanged` (`minigislayerpanel.cpp:288-298`); `MainWindow::onLayerPanelCurrentChanged` fija `canvas->setCurrentLayer` o `mCurrentRaster` (`minigismainwindow.cpp:907-942`) |
| Conversión píxel ↔ mapa | `MapToPixel` (`minigismaprenderer.h:25-60`), la misma que usa el dibujado |
| Almacén de geometrías | El cargador lo construye en segundo plano **antes** de entregar la capa al lienzo (`minigisvectorloader.cpp:145-176`); hay R-tree (`GeometryStore::query`) y nivel 0 sin simplificar |
| Atributos por fila | `VectorLayer::fieldValues(row)` lee todos los campos de una vez por FID (`minigisvectorlayer.cpp:480-504`); la tabla de atributos usa ese mismo convenio |
| Pruebas definidas | 15 (`tests/CMakeLists.txt`): `help`, `version`, `info`, `snapshot`, `salida-por-error-de-archivo`, `geopackage`, `panel-capas`, `bench`, `indice-envolventes`, `redibujado`, `etiquetas`, `raster-info`, `raster-dibujo`, `raster-dialogo`, `raster-ventana` |
| Datos de ejemplo | `parcels.shp` (5 polígonos, P-005 con hueco), `cities.shp` (12 puntos, Madrid fid 0), `parcels_utm30.shp` (los mismos 5 en EPSG:25830), `ortho_rgb.tif` |

> **Verificación de partida (2026-10-22).** Con `pwsh` disponible se ejecutaron
> `scripts\build.bat windows-msvc-release` (compila y despliega sin errores) y
> `ctest --preset windows-msvc-release` (**15/15**, 5,3 s). `--version` informa
> Qt 6.10.3, GDAL 3.12.4, PROJ 9.8.1, MSVC 19.44 y Release. Los FID de los datos
> de ejemplo se comprobaron con `ogrinfo`: Madrid es el FID 0 de `cities.shp` y
> P-001 el FID 0 de `parcels.shp`. El bucle se repitió tras cada tarea; al cerrar
> la entrega, con compilación limpia, dio **18/18** (§11).

---

## 3. Requisitos funcionales

### R1 — Disparo: un clic sin arrastre

* Un **clic izquierdo sin arrastre** sobre el lienzo ejecuta Identificar en el
  punto del mapa bajo el cursor. Se considera clic si el desplazamiento máximo
  durante el gesto no supera **3 px** (distancia euclídea); si en algún momento
  lo supera, el gesto es paneo y **no** identifica, aunque el botón se suelte
  sobre el punto de partida.
* El **botón central** nunca identifica.
* El **arrastre** sigue siendo paneo, con el mismo modo borrador y la misma
  caché de cuadro de hoy. El **doble clic** sigue haciendo zoom; el primer clic
  del par puede identificar y se acepta como comportamiento documentado.
* No hay «modo identificar» que activar: el clic normal identifica.
* Sin extensión válida (lienzo sin capas ni encuadre) no se hace nada.
* El lienzo emite una señal con el punto del mapa y el píxel:

  ```cpp
  void identifyRequested(const QPointF &mapPoint, const QPoint &pixel);
  ```

  El punto se calcula con el mismo `MapToPixel` que se usa para pintar, para que
  «lo que está bajo el cursor» sea exactamente lo que se identifica.

### R2 — Alcance: manda la capa seleccionada

El alcance se decide con el estado del árbol **en el momento del clic**:

| Estado del árbol | Capas que se consultan | Aviso en el panel |
|---|---|---|
| Capa vectorial seleccionada y visible | **solo esa** | «Solo la capa *X*» |
| Capa vectorial seleccionada pero oculta | ninguna | «La capa *X* está oculta» |
| Capa ráster seleccionada | todas las vectoriales visibles | «La capa seleccionada es un ráster: se busca en todas las vectoriales» |
| Nada seleccionado | todas las vectoriales visibles | «Todas las capas visibles (N)» |
| Casilla **Buscar en todas las capas** marcada | todas las visibles, con los resultados de la seleccionada (si es vectorial) **primero** | «Capa *X* primero; además todas las visibles (N)» |

* Una capa **oculta nunca se consulta** (no está en el mapa), ni siquiera si es
  la seleccionada; en ese caso se avisa y no se busca en las demás: lo pedido es
  la capa seleccionada, no otra.
* Un **grupo** del árbol (el ítem del archivo) no es una capa y no cuenta como
  selección.
* Si no hay ninguna capa vectorial visible, el panel lo dice; no es un error.
* La consulta no cambia la selección ni la visibilidad de nada.

### R3 — Candidatos y prueba exacta

* El radio se expresa en **píxeles** (por defecto **6 px**) y se convierte a
  unidades del SRC de destino con `mapUnitsPerPixel()` del lienzo.
* **Candidatos:** elementos cuya envolvente corta el cuadrado de lado
  `2 × tolerancia` centrado en el clic, obtenidos del **R-tree** del
  `GeometryStore` (`query`). No se recorre la capa completa.
* **Prueba exacta** (nivel 0, sin simplificar):
  * **polígonos**: dentro por **paridad (even-odd)** sobre todos sus anillos, de
    modo que los **huecos quedan fuera** sin necesitar el rol de cada anillo;
    fuera del polígono, distancia al segmento más cercano;
  * **líneas**: distancia mínima a los segmentos;
  * **puntos**: distancia al punto.
* Un elemento se devuelve si está **dentro** o a una distancia **≤ tolerancia**.
  Cada resultado indica `inside` (dentro, no solo cerca) y `distance`
  (0 si está dentro, en unidades del SRC de destino).
* **Recorte:** como máximo **10 elementos por capa** y **50 en total**; si se
  recorta, `truncated = true` y el panel lo avisa (no se oculta el corte).
* **Presupuesto:** como máximo **256 candidatos por capa** (los más cercanos por
  envolvente) y **250 ms** por consulta. Si se agota, se corta y se avisa.
* **Capas sin almacén** (enormes, o almacén en otro SRC): camino alternativo con
  OGR —filtro espacial con el rectángulo del clic (transformando la caja al SRC
  de la capa si hace falta), geometría transformada al SRC de destino y la misma
  prueba exacta—, con los mismos topes. En modo `Envelopes` se usa el mismo
  `hitTest` del almacén, que lee la geometría **solo de los candidatos**.
* **FID = fila**, el mismo convenio que la tabla de atributos y el índice de
  envolventes; los valores se leen con `fieldValues(fid)` para los resultados que
  se van a mostrar (no para todos los candidatos).
* Un campo nulo se muestra como **`(nulo)`**, igual que en la tabla.

### R4 — Orden y prioridad

1. Si hay capa prioritaria (la seleccionada), **sus resultados van primero**.
2. Después, las capas en **orden de apilado de arriba abajo** (la última capa
   dibujada es la primera que se consulta).
3. Dentro de una capa: primero los elementos **dentro**, luego los cercanos por
   **distancia ascendente** y, a igual distancia, por **FID ascendente**.

El orden es determinista y es parte del contrato de las pruebas.

### R5 — La ventana de resultados

* Un **dock** no modal titulado **Identificar**, en el área derecha, que se
  muestra y se trae al frente en cada consulta. Se puede cerrar y volver a abrir
  con **Ver → Identificar** o **Ctrl+Shift+I**; cerrarlo no borra los resultados.
* **Cabecera:** coordenadas del clic, radio en px y en unidades de mapa, y el
  alcance aplicado (R2).
* **Árbol de dos columnas** (`Campo`, `Valor`):
  * **Capa** (nivel 0): nombre y, si la capa pertenece a un archivo con varias,
    el nombre del archivo; número de elementos encontrados; etiqueta
    «(seleccionada)» si es la capa prioritaria.
  * **Elemento** (nivel 1): `#<fid> · <tipo> · dentro` o
    `#<fid> · <tipo> · a <n> px`, con el tipo traducido (Punto, Línea, Polígono).
  * **Campos** (nivel 2): una fila por campo, en el orden de la capa, con el
    valor tal como lo devuelve `fieldValues` (vacío → `(nulo)`).
* **Estados vacíos explicados** (una fila de aviso, no un panel en blanco):
  sin capas vectoriales visibles, capa seleccionada oculta, capa seleccionada
  ráster, y «ningún elemento a menos de N px» con la pista de marcar
  **Buscar en todas las capas**.
* **Aviso de recorte**: si `truncated`, una fila dice cuántos resultados se
  muestran y que hay más.
* Botón **Copiar**: copia al portapapeles el contenido del panel como texto.
* Los campos se leen al identificar, no al pintar el panel; el panel no llama a
  GDAL.

### R6 — Marcador del clic

* El punto del último clic se marca con una **cruz** de 1 px (brazos de 4 px,
  centro de 3×3), sin suavizado, en **magenta puro** (`#FF00FF`), dibujada sobre
  el cuadro y **después de guardarlo en la caché**, igual que las etiquetas
  (`minigismapcanvas.cpp:709-726`).
* El marcador se guarda en **coordenadas de mapa**: se desplaza y se escala con
  el mapa al hacer paneo o zoom, y se sustituye en cada clic nuevo.
* Aparece en `--snapshot`, `--snapshot-window` y en el lienzo normal.
* **No invalida la caché de cuadro** (se dibuja encima, como las etiquetas).

### R7 — Identificar no cuesta un cuadro

* Identificar **no modifica** capas, simbología, visibilidad ni revisiones
  (`revision()`), y **no invalida** la caché de cuadro.
* El siguiente repintado **reutiliza** el cuadro anterior (solo dibuja el
  marcador encima). Se comprueba con `lastFrameReused()` (un booleano: reutilizar
  puede tardar 0 ms, así que los milisegundos no prueban nada).

### R8 — Contrato de la CLI

| Opción | Efecto |
|---|---|
| `--identify-at <x,y>` | Consulta puntual en coordenadas del SRC de destino; imprime los resultados (texto) o JSON con `--json` |
| `--identify-radius <px>` | Radio en píxeles (entero ≥ 1; por defecto 6) |
| `--identify-layer <nombre>` | Limita la consulta a esa capa (comparación sin distinguir mayúsculas); si no existe o es ambigua → código 1 con la lista de capas |
| `--check-identify` | Ejecuta el contrato de §9.3 y sale (0 todo bien, 1 algún FALLO) |

* `--identify-at` usa `--size` y la extensión (`--extent`, o la extensión completa
  de lo cargado) para convertir el radio a unidades de mapa; **no dibuja**.
* `--snapshot` y `--snapshot-window` aceptan `--identify-at` y dejan el
  **marcador** en la imagen (en `--snapshot-window` además se rellena el panel).
* `--identify-at --json` emite (claves estables, todas las de campo como texto):

  ```json
  {
    "query": {
      "x": -3.70379, "y": 40.41678, "epsg": 4326,
      "radius_px": 6, "radius_map_units": 0.00072,
      "scope": "all", "layer": null
    },
    "hit_count": 2,
    "hits": [
      {
        "layer": "cities", "container": "cities.shp", "fid": 0,
        "geometry": "Point", "inside": true,
        "distance_map_units": 0, "distance_px": 0,
        "fields": { "name": "Madrid", "country": "ES", "population": "3280782" }
      }
    ],
    "layers_searched": 2,
    "candidates_tested": 2,
    "features_read": 0,
    "elapsed_ms": 1,
    "truncated": false
  }
  ```

* Texto sin `--json`: una línea de cabecera con `x`, `y`, SRC y radio, y por cada
  resultado `capa`, `#fid · tipo · dentro|a N px` y `campo: valor`. Sin
  resultados: `Sin elementos a menos de N px (x, y).`
* **Códigos de salida** (los actuales, sin cambios): `0` éxito (también sin
  resultados), `1` uso incorrecto (coordenadas malas, radio inválido, capa
  desconocida), `2` archivo no encontrado, `3` GDAL no pudo abrir, `5` no hay
  capas vectoriales. `stdout` = datos; `stderr` = diagnóstico.
* `--help` documenta las cuatro opciones con un ejemplo.

### R9 — Rendimiento

* Con almacén, una consulta recorre **solo los candidatos del R-tree**: no hay
  recorrido de capa ni lectura de atributos de elementos descartados.
* Sobre los datos de ejemplo, una consulta tarda **≤ 50 ms** (se mide y se
  informa en `--check-identify`; es una puerta de humo, no un banco de
  rendimiento).
* El camino sin almacén está acotado por el presupuesto de 250 ms y el tope de
  candidatos; nunca congela la interfaz más que eso.
* Identificar corre en el hilo de la interfaz y es **síncrono**: los topes son la
  garantía, no un hilo aparte (ver §12, supuesto 10).

### R10 — No regresión

* Las **15 pruebas actuales** siguen pasando y el comportamiento de paneo, zoom,
  doble clic, etiquetas, tabla de atributos, diálogos y caché no cambia.
* Sin dependencias nuevas (GDAL, Qt y PROJ ya están).
* Listas de fuentes explícitas en los `CMakeLists.txt` de cada carpeta (sin
  `GLOB`), como exige `AGENTS.md`.

### R11 — Documentación

* `README.md`: las cuatro opciones en la tabla de §6, el atajo y el menú en §5,
  el bullet de Identificar en §7, las tres pruebas nuevas en §8 y el resultado de
  §11 cuando se implemente.
* El estado de este spec pasa a «implementado» con la fecha y la verificación
  real al cerrar la entrega.

---

## 4. Decisiones (y por qué)

1. **Dock, no diálogo modal.** Identificar es una consulta que se repite: con un
   modal cada clic interrumpe y hay que cerrarlo para seguir mirando el mapa. Un
   dock sobrevive a clics sucesivos, conserva el resultado mientras se mueve el
   mapa y se puede capturar con `--snapshot-window` para la prueba visual.
2. **La capa seleccionada es un ámbito exclusivo, no solo un orden.** Es la
   lectura literal de «la capa seleccionada es prioritaria» y evita la ambigüedad
   de «¿por qué me sale esta parcela de otra capa?». La casilla **Buscar en todas
   las capas** da la salida cuando el usuario quiere el resto, y en ese caso la
   seleccionada sigue saliendo primero (R4).
3. **Radio en píxeles, no en unidades de mapa.** «Cerca» es una sensación de
   pantalla: 6 px son 6 px a cualquier zoom. Convertirlo con el mismo `MapToPixel`
   del dibujado garantiza que se identifique lo que se ve bajo el cursor.
4. **Reutilizar el `GeometryStore` y su R-tree, no un segundo índice.** El
   almacén ya tiene las envolventes, el nivel 0 y la caché de geometría del modo
   envolventes. Construir otro índice costaría memoria y tiempo de carga para
   ganar nada.
5. **Prueba exacta después del filtro por envolvente.** Identificar solo por
   envolvente diría que un clic en el hueco de una parcela está dentro de ella.
   La prueba por paridad es barata y es la diferencia entre una respuesta útil y
   una mentira; el hueco de P-005 existe justo para probarlo.
6. **`GeometryStore::hitTest` como interfaz estrecha.** El almacén guarda las
   coordenadas en buffers planos y conoce sus dos modos (memoria y envolventes).
   Exponer una prueba puntual evita que Identificar copie geometrías y evita el
   problema de mapear entradas de `acquireSubset` a FID (que no es 1:1 en modo
   envolventes).
7. **Atributos solo de lo que se muestra.** Leer los campos de 256 candidatos
   para descartar 250 es tirar I/O; se leen después de ordenar y recortar.
8. **Marcador encima del cuadro cacheado.** Cuesta cero milisegundos, no
   invalida la caché y da realimentación inmediata de dónde se hizo clic. Es el
   mismo patrón que ya usan las etiquetas.
9. **Topes visibles, no silenciosos.** Un clic en una capa de millones de
   elementos no puede devolver «nada» sin explicar que se cortó. `truncated` y el
   aviso del panel convierten el límite en información.
10. **Contrato sin ventana.** `--check-identify` y `--identify-at` siguen la
    norma del proyecto: todo comportamiento verificable tiene un modo sin
    pantalla. La prueba de clic real usa eventos de ratón sobre el lienzo
    offscreen, así que cubre también el umbral clic/paneo.

---

## 5. Fuera de alcance (explícito)

* **Valores de píxel del ráster** (bandas, NoData, paleta bajo el clic). Un
  ráster no tiene «elementos» en el modelo actual; merece su propio spec.
* **Resaltar la geometría** del elemento identificado en el mapa (solo se marca
  el punto del clic).
* Selección de elementos, consultas por atributos, expresiones, filtros o SQL.
* Identificar arrastrando un rectángulo o lazo; identificar mientras se panea.
* Resultados persistentes entre sesiones, exportación o impresión de resultados.
* Selección múltiple de capas en el árbol.
* Ordenar o filtrar los resultados del panel por columna.
* Cambiar el SRC del lienzo desde la interfaz (sigue fijo; `--crs` solo en CLI).
* Identificar en servidores OGC, teselas o capas que no sean de GDAL/OGR.

---

## 6. Comandos

### 6.1 Bucle de trabajo (el del proyecto)

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

> Verificado el 2026-10-22: `scripts\build.bat windows-msvc-release` compila y
> `ctest --preset windows-msvc-release` da **15/15**. Este es el bucle que hay que
> repetir en cada tarea de la implementación.

### 6.2 Uso de lo nuevo

```bat
:: contrato completo (lo que ejecutara CTest)
build\windows-msvc-release\bin\minigis.exe --check-identify ^
        -l data\sample\shapefiles\parcels.shp ^
        -l data\sample\shapefiles\cities.shp ^
        -l data\sample\rasters\ortho_rgb.tif

:: consulta puntual en coordenadas del mapa (Madrid, dentro de la parcela P-001)
build\windows-msvc-release\bin\minigis.exe --identify-at -3.70379,40.41678 --json ^
        -l data\sample\shapefiles\parcels.shp -l data\sample\shapefiles\cities.shp

:: solo una capa, y con otro radio
build\windows-msvc-release\bin\minigis.exe --identify-at -3.71,40.41 --identify-layer parcels ^
        --identify-radius 3 -l data\sample\shapefiles\parcels.shp

:: una capa en UTM sobre un mapa en EPSG:4326
build\windows-msvc-release\bin\minigis.exe --identify-at -3.71,40.41 --json ^
        -l data\sample\shapefiles\parcels_utm30.shp

:: captura de la interfaz con el panel de resultados lleno (prueba visual)
build\windows-msvc-release\bin\minigis.exe --snapshot-window resultado.png ^
        --identify-at -3.70379,40.41678 ^
        -l data\sample\shapefiles\parcels.shp -l data\sample\shapefiles\cities.shp
```

### 6.3 Pruebas nuevas

```bat
ctest --preset windows-msvc-release -R "minigis.identificar" --output-on-failure
```

---

## 7. Estructura del proyecto

```
src/core/
  minigisgeometryhit.h/.cpp    prueba punto contra anillos (paridad + distancia)
  minigisidentify.h/.cpp       consulta: alcance, candidatos, orden, recorte, estadisticas
  minigisgeometrystore.*       + hitTest(index, x, y, tol, inside, distance)
src/gui/
  minigisidentifypanel.h/.cpp  dock de resultados (arbol, avisos, copiar)
  minigismapcanvas.*           clic sin arrastre -> identifyRequested; marcador en el overlay
  minigismainwindow.*          alcance, dock, menu/atajo, identifyAtMapPoint(), selectLayer()
src/app/main.cpp               --identify-at, --identify-radius, --identify-layer,
                               --check-identify, --snapshot(-window) con marcador, --help
tests/
  CMakeLists.txt               minigis.identificar, minigis.identificar-cli,
                               minigis.identificar-ventana
  run_cli_test.cmake           modos check-identify e identify-at
README.md                      uso, atajos, pruebas y limites
```

Regla de dependencias: `core` no incluye cabeceras de `gui`; `gui` no llama a GDAL
salvo a través de `core`; `app` no contiene lógica de datos.

---

## 8. Estilo de código

**Stack:** C++17, Qt 6.10.3 (Core/Gui/Widgets/Concurrent), GDAL 3.12.4 vía vcpkg,
MSVC 2022, CMake + Ninja, preset `windows-msvc-release`. Sin dependencias nuevas.

Se mantiene el estilo del proyecto: `namespace minigis`, ambos archivos con el
mismo nombre en minúsculas (`minigisidentify.h/.cpp`), miembros `m`, métodos en
`camelCase`, tipos en `PascalCase`, llaves en línea nueva, 2 espacios y
comentarios en **español sin tildes** que explican el *por qué*.

Interfaz que se propone (core, sin widgets):

```cpp
/**
 * Busca los elementos que contienen el punto o estan a menos de su tolerancia.
 *
 * El orden de `layers` es el de apilado del lienzo (fondo a techo) y el de los
 * resultados es el de lo que el usuario ve: primero la capa prioritaria, luego
 * de arriba abajo. Los atributos se leen solo para los resultados que se van a
 * mostrar, no para todos los candidatos.
 */
IdentifyResult identifyAtPoint(const IdentifyRequest &request);

/**
 * Prueba un punto contra la geometria del elemento `index`, en el SRC del
 * almacen. Devuelve false si la geometria no esta disponible (por ejemplo, en
 * modo envolventes cuando la lectura falla).
 */
bool hitTest(uint32_t index, double x, double y, double tolerance,
             bool &inside, double &distance) const;
```

Cada función pública lleva su comentario Doxygen; cada error de GDAL que pueda
llegar al usuario se convierte en un aviso del panel (causa + qué hacer), nunca
en un resultado vacío sin explicación.

---

## 9. Estrategia de pruebas

### 9.1 Datos deterministas

| Archivo | Contenido | Uso en las pruebas |
|---|---|---|
| `parcels.shp` | 5 polígonos, EPSG:4326; P-001 = [-3.72,-3.70]×[40.40,40.42]; P-003 justo debajo; P-005 = [-3.76,-3.72]×[40.36,40.40] **con hueco** [-3.752,-3.728]×[40.368,40.392] | dentro, cerca, hueco, solape de capas |
| `cities.shp` | 12 puntos, EPSG:4326; Madrid = (-3.70379, 40.41678), fid 0, **dentro de P-001** | punto, prioridad, apilado |
| `parcels_utm30.shp` | los mismos 5 polígonos en EPSG:25830 | capa reproyectada al cargar |
| `ortho_rgb.tif` | ráster 256×256, EPSG:4326 | capa ráster seleccionada |

Las pruebas fijan la escala con **0,00012 unidades de mapa por píxel** (extensión
calculada a partir del tamaño real del lienzo), de modo que las distancias en
píxeles son exactas y no dependen del tamaño de la ventana:

| Clic (x, y) | Resultado esperado | Distancia |
|---|---|---|
| (-3.71, 40.41) | P-001, `inside = true` | 0 px |
| (-3.7205, 40.41) | P-001, `inside = false` | 4,2 px (dentro del radio de 6) |
| (-3.7235, 40.41) | nada | 29,2 px (fuera del radio de 6) |
| (-3.74, 40.38) | nada (hueco de P-005) | 100 px al anillo del hueco |
| (-3.755, 40.375) | P-005, `inside = true` | 0 px |
| (-3.70379, 40.41678) | Madrid (cities) **y** P-001 (parcels) | 0 px en ambos |

### 9.2 Pruebas de CTest

| Prueba | Comando / modo | Qué exige |
|---|---|---|
| `minigis.identificar` | `--check-identify` (parcels, cities, ortho) | todas las líneas de §9.3 en `OK`, ninguna `FALLO`, código 0 |
| `minigis.identificar-cli` | `--identify-at --json` (cuatro invocaciones) | claves JSON de §9.4 y códigos de salida |
| `minigis.identificar-ventana` | `--snapshot-window` + `--identify-at` | PNG escrito (> 10 KB) con el panel y el marcador |

Las tres se ejecutan con `QT_QPA_PLATFORM=offscreen`, como el resto.

### 9.3 Contrato de `--check-identify`

Cada comprobación imprime `OK  <nombre> — <detalle>` o `FALLO <nombre> — <detalle>`
y devuelve 1 si alguna falla. El chequeo carga las capas en el orden
**parcels, cities, ortho** por el camino real (ventana + cargador en segundo
plano). Las comprobaciones de **geometría** (1 a 12) llaman al mismo motor con un
punto de mapa calculado con el `MapToPixel` del lienzo, para poder construirlas
antes que el panel; las de **interacción** (13 a 23) envían `QMouseEvent` de
press/release sobre el lienzo y leen el panel, así que cubren el umbral
clic/paneo, el marcador y el alcance del árbol. Las 8, 9, 10, 11, 12 y 23 usan
instancias propias de lienzo, almacén o panel (sin índice, modo envolventes,
recorte, prioridad, rapidez, aviso); las de geometría usan una sola capa por
consulta para aislar la prueba.

| # | Comprobación | Qué exige |
|---|---|---|
| 1 | `clic en un punto identifica la ciudad` | con `cities` seleccionada: 1 resultado, fid 0, `name = Madrid`, `inside = true` |
| 2 | `clic dentro de un poligono identifica la parcela` | con `parcels` seleccionada: 1 resultado, `parcel_id = P-001`, `inside = true` |
| 3 | `el hueco del poligono no se identifica` | con `parcels` seleccionada: 0 resultados en (-3.74, 40.38) |
| 4 | `la parte solida de un poligono con hueco si se identifica` | con `parcels` seleccionada: 1 resultado, `parcel_id = P-005`, `inside = true` |
| 5 | `un clic a 4 px del borde identifica el poligono como cercano` | con `parcels` seleccionada: 1 resultado, P-001, `inside = false`, `distance` entre 3,5 y 5,0 px |
| 6 | `a 29 px no se identifica nada` | con `parcels` seleccionada: 0 resultados |
| 7 | `el radio es el que se pide: 3 px no alcanza, 6 px si` | con tolerancia de 3 px, 0 resultados; con 6 px, P-001 |
| 8 | `una capa sin indice se identifica por el camino OGR` | una capa abierta sin `GeometryStore` devuelve P-001 |
| 9 | `una capa indexada por envolventes lee solo la geometria del candidato` | P-001 y `features_read = 1` (delta de `geometriesRead` alrededor de la consulta) |
| 10 | `el recorte de resultados se declara` | con `maximumTotal = 1` y dos capas: 1 resultado y `truncated = true` |
| 11 | `la capa prioritaria va primero` | con priority = parcels: el primer resultado es de `parcels`, luego Madrid |
| 12 | `la consulta es rapida` | `elapsedMs ≤ 50` en los datos de ejemplo |
| 13 | `un clic sin arrastre abre el panel y muestra el elemento` | el dock está visible y sus filas contienen la capa, el `#fid` y los campos de Madrid |
| 14 | `sin seleccion se consultan todas las capas visibles, de arriba abajo` | 2 capas; primer resultado Madrid (`cities`, la de arriba); luego P-001 |
| 15 | `la capa seleccionada manda` | con parcels seleccionada: 1 sola capa consultada y resultado P-001 (sin Madrid) |
| 16 | `la capa seleccionada va primero si se buscan todas` | con parcels seleccionada y «buscar en todas»: P-001 primero y Madrid después |
| 17 | `una capa oculta no se identifica y se avisa` | parcels oculta y seleccionada: 0 resultados y el aviso contiene «oculta»; oculta sin selección: no aparece en los resultados |
| 18 | `una capa raster seleccionada no limita la busqueda` | con el ráster seleccionado, el clic en P-001 devuelve P-001 y el aviso menciona «raster» |
| 19 | `el panel agrupa por capa y muestra los campos` | filas: capa (nivel 0), elemento (nivel 1), campo `name` = `Madrid` (nivel 2); el grupo lleva el número de elementos |
| 20 | `el marcador del clic se dibuja en el pixel pulsado` | en `renderToImage`, el píxel del clic es exactamente `#FF00FF`; a 8 px no lo es |
| 21 | `identificar no invalida el cuadro cacheado` | `lastFrameReused()` es true en el repintado posterior (solo parcels) |
| 22 | `arrastrar no identifica: sigue siendo paneo` | un arrastre de 20 px no cambia las filas del panel y sí cambia la extensión |
| 23 | `el panel avisa del recorte de resultados` | `IdentifyPanel` con un resultado `truncated` muestra una fila de aviso |

### 9.4 Contrato de `--identify-at --json`

Cuatro invocaciones en `run_cli_test.cmake`. Las tres primeras fijan
`--size 1000x700 --extent -3.78,40.35,-3.66,40.43` (0,00012 u.m./px) para que el
radio de 6 px sea determinista; la del hueco también.

| Invocación | Debe contener |
|---|---|
| parcels + cities, punto Madrid | `"layer": "cities"`, `"name": "Madrid"`, `"inside": true`, `"layers_searched": 2`, `"truncated": false` |
| la misma, con `--identify-layer parcels` | `"layer": "parcels"`, `"parcel_id": "P-001"`, `"layers_searched": 1`, `"scope": "layer"` |
| solo `parcels_utm30.shp`, punto (-3.71, 40.41) | `"parcel_id": "P-001"`, `"inside": true` |
| punto del hueco (-3.74, 40.38), solo `parcels.shp` | `"hit_count": 0` y código 0 (sin resultados no es error) |

### 9.5 Matriz requisito → prueba

| Requisito | Prueba |
|---|---|
| R1 | 13, 22, `minigis.identificar-ventana` |
| R2 | 14–18 |
| R3 | 1–9 |
| R4 | 10, 11, 14, 16 |
| R5 | 13, 19, 23, `minigis.identificar-ventana` |
| R6 | 20 |
| R7 | 21 |
| R8 | `minigis.identificar-cli`, 3 (código 0 sin resultados) |
| R9 | 12 |
| R10 | las 15 pruebas existentes |
| R11 | revisión de README y `--help` |

---

## 10. Límites

**Siempre**

* Compilar con el preset `windows-msvc-release` y ejecutar **todas** las pruebas
  (las 15 actuales + las 3 nuevas) antes de dar una tarea por terminada.
* Comprobar que identificar **no** sube revisiones ni invalida la caché de
  cuadro; si el marcador obliga a repintar, el repintado debe reutilizar el
  cuadro anterior.
* Errores con causa y acción: una capa que no se puede identificar dice por qué.
* Añadir una comprobación a `--check-identify` por cada comportamiento nuevo que
  pueda romperse en silencio (distancias y píxeles, no impresiones).
* Documentar en `README.md` las opciones, el atajo y los límites nuevos.
* Mantener el contrato de la CLI (códigos de salida y separación
  `stdout`/`stderr`).

**Preguntar antes**

* Añadir un «modo identificar» con botón o cambiar el significado del clic
  izquierdo (paneo) más allá de lo que dice R1.
* Cambiar la arquitectura del lienzo (jerarquía de capas genérica) o el motor de
  dibujado más allá de dibujar el marcador encima.
* Cambiar el contrato JSON de `--info`, `--bench` o `--check-layers`.
* Añadir dependencias (no se prevé ninguna: GDAL y Qt ya están).
* Cambiar los datos de ejemplo existentes (el hueco de P-005 y la posición de
  Madrid **son** parte del contrato de pruebas).
* Persistir el radio o los topes en `QSettings`.

**Nunca**

* Recorrer la capa completa en cada clic cuando hay almacén.
* Materializar geometría de un millón de elementos para identificar uno.
* Devolver «no hay nada» cuando en realidad se recortó la búsqueda: el recorte
  se declara.
* Identificar capas ocultas o ignorar la capa seleccionada sin avisar.
* Silenciar un fallo de GDAL con un resultado vacío.
* Debilitar o borrar una prueba existente para que pase un cambio nuevo.
* Modificar o reutilizar código del repositorio QGIS (política QEP-408).

---

## 11. Criterios de éxito

Se considerará terminado cuando **todo** esto sea cierto y reproducible:

| # | Criterio | Cómo se comprueba |
|---|---|---|
| SC1 | Compila en Release (MSVC, Qt 6.10.3, GDAL 3.12.4) y pasan **18/18** pruebas | §6.1 |
| SC2 | Con `cities` seleccionada, un clic en Madrid muestra la capa, el fid y todos sus campos | `minigis.identificar` 1 |
| SC3 | Con `parcels` seleccionada, un clic dentro de P-001 muestra la parcela; uno en el hueco de P-005 no | `minigis.identificar` 2, 3 |
| SC4 | Un clic a 4 px del borde identifica P-001 como cercano y a 29 px no identifica nada | `minigis.identificar` 5, 6 |
| SC5 | Con parcels seleccionada solo se consulta parcels; sin selección se consultan todas | `minigis.identificar` 14, 15 |
| SC6 | Con «buscar en todas», la capa seleccionada sale primero | `minigis.identificar` 16 |
| SC7 | Capa oculta y capa ráster seleccionada se comportan según R2, con aviso | `minigis.identificar` 17, 18 |
| SC8 | El panel agrupa por capa y muestra los campos con `(nulo)` cuando toca | `minigis.identificar` 19 |
| SC9 | El marcador cae en el píxel pulsado y no invalida la caché | `minigis.identificar` 20, 21 |
| SC10 | Arrastrar sigue paneando y no identifica | `minigis.identificar` 22 |
| SC11 | `--identify-at --json` cumple el esquema y los códigos de salida | `minigis.identificar-cli` |
| SC12 | El recorte de resultados se declara | `minigis.identificar` 10, 23 |
| SC13 | `--snapshot-window` con `--identify-at` produce un PNG con panel y marcador | `minigis.identificar-ventana` |
| SC14 | Las 15 pruebas existentes siguen verdes | `ctest` |
| SC15 | README y `--help` describen opciones, atajo, pruebas y límites | revisión |

**Verificación (2026-10-22).** Compilación limpia con `windows-msvc-release` y
`ctest --preset windows-msvc-release`: **18/18** (15 existentes + 3 nuevas).
`--check-identify` imprime las 23 comprobaciones en `OK`; `minigis.identificar-cli`
cubre las cuatro invocaciones JSON; `minigis.identificar-ventana` escribe
`build\windows-msvc-release\test-output\identify-window.png` y la captura nativa
`build\test-output\identify-native.png` muestra el dock con Madrid y la cruz del
clic. README (§3, §5–§8, §11) y `--help` actualizados.

---

## 12. Supuestos (corregir si alguno no es cierto)

1. «Ventana» significa un **dock** de la ventana principal (no modal), que se
   muestra al identificar y se puede cerrar y reabrir.
2. «La capa seleccionada es prioritaria» significa **solo esa capa** cuando hay
   selección; la casilla «Buscar en todas las capas» es la excepción explícita.
3. «Elemento geográfico» es un **elemento vectorial** (punto, línea o polígono);
   los rásters no tienen elementos en este modelo y quedan fuera (§5).
4. «Cerca del clic» es un radio en **píxeles de pantalla**, 6 por defecto.
5. Un elemento se considera identificado si el clic está **dentro** o a **≤ el
   radio** de su geometría; los polígonos respetan sus huecos.
6. La distancia se mide en unidades del **SRC de destino** del lienzo (EPSG:4326
   por defecto) y se muestra en píxeles.
7. `fid` es el **índice de fila** que ya usan la tabla de atributos y el índice de
   envolventes; los FID son 0..N-1 y estables mientras la capa no cambie.
8. Las capas ocultas no se identifican; una capa seleccionada oculta no delega en
   las demás: se avisa y no se busca.
9. El lienzo sigue siendo de solo lectura: identificar no modifica datos.
10. La consulta es **síncrona** en el hilo de la interfaz, acotada por los topes
    de R3/R9; no se añade un hilo de trabajo para identificar.
11. Identificar no requiere un modo previo ni un botón de herramienta.
12. El panel se rellena en cada clic (aunque no haya resultados) para dar
    realimentación; no se queda con el resultado anterior.
13. `--snapshot-window` es el sitio aceptado para la prueba visual y la captura
    de documentación.
14. El toolchain de referencia es el de `AGENTS.md`: Qt 6.10.3, GDAL 3.12.4,
    MSVC 2022, preset `windows-msvc-release`. GCC/MinGW no se soporta.

---

## 13. Preguntas abiertas

**Resueltas en la aprobación del 2026-10-22:**

1. **Ventana:** dock no modal (§4, decisión 1). Descartado el diálogo modal.
2. **Alcance:** la capa seleccionada **limita** la consulta (§4, decisión 2); la
   casilla **Buscar en todas las capas** permite ampliar con la seleccionada
   primero.
3. **Ráster:** los valores de píxel del ráster quedan **fuera** de esta entrega
   (§5). Un clic sobre una ortofoto avisa de que la capa seleccionada es un ráster
   y de que no hay elementos cercanos.

**Pendientes (se mantiene la propuesta salvo indicación):**

4. **¿Resaltar la geometría identificada?** Propuesto: no; solo la cruz del clic.
   Resaltar exige dibujar la geometría del resultado encima del cuadro, con sus
   pruebas de píxeles y su coste en capas grandes.
5. **¿Radio por defecto de 6 px?** Propuesto: 6 (más tolerante que los 3 px
   típicos de QGIS, pensando en pantallas táctiles y en puntos pequeños). Se
   puede cambiar por CLI (`--identify-radius`), no desde la interfaz en esta
   entrega.
6. **¿Topes de 10 por capa y 50 en total?** Propuesto: sí, visibles en el panel.
   Alternativa: sin tope, con el riesgo de una ventana inmanejable.
7. **¿Hace falta `--identify-at`, o basta `--check-identify`?** Propuesto: sí;
   sirve para depurar un caso concreto y para scripts, igual que `--info`.
8. **¿Identificar debe ser un modo explícito (botón «Identificar») o el clic
   normal basta? ¿Y debe funcionar con el botón derecho o con `Ctrl+clic`?**
   Propuesto: el clic izquierdo sin arrastre basta y no se añade ni modo ni
   botón; no choca con el paneo y evita un estado que hay que recordar.
9. **¿Un grupo seleccionado cuenta como capa?** Propuesto: no; solo una fila de
   capa define el ámbito.
10. **¿Se recuerda el radio y la casilla «buscar en todas» entre sesiones?**
    Propuesto: no en esta entrega (sin `QSettings`); se añade si se pide.
