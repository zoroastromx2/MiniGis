# Spec: capa ráster (GeoTIFF y otros formatos de GDAL)

> Módulo: `raster` · Estado: **propuesta para revisión** · Fecha: 2026-10-20
>
> Este documento define *qué* se construye y *cómo se sabe que está bien*. No se
> escribe código hasta que se apruebe. El plan técnico y las tareas van después,
> en `tasks/plan.md` y `tasks/todo.md`.

---

## 1. Objetivo

MiniGIS abre, dibuja y consulta capas vectoriales, pero no puede mostrar un
ráster: una ortofoto bajo las parcelas, un modelo digital de elevaciones junto a
la red de drenaje o una capa de usos del suelo. GDAL ya está enlazado y
registrado, así que el motor de datos existe; lo que falta es la capa, el
dibujado y su interfaz.

Este cambio añade **capas ráster de solo lectura**:

* abrir cualquier archivo que GDAL reconozca como ráster (GeoTIFF en primer
  lugar, pero también PNG/JPEG georreferenciados, JPEG2000, VRT, IMG…);
* dibujarlo en su posición geográfica correcta, **reproyectándolo al SRC del
  lienzo** cuando sea necesario;
* ajustar **bandas, estiramiento de contraste y opacidad** por capa;
* verificarlo todo sin ventana, con el mismo estilo de contrato que ya usan
  `--info`, `--snapshot` y `--check-refresh`.

**Usuario:** quien revisa datos en MiniGIS (una ortofoto con sus parcelas
encima, un DEM con la red) y necesita ver el ráster colocado y legible sin
abrir QGIS.

**Criterio de éxito del producto:** abrir un GeoTIFF y verlo en el mapa, bajo
las capas vectoriales, con colores razonables y sin pantallas en blanco,
congelaciones ni desplazamientos; y que las mismas comprobaciones se puedan
ejecutar sin ventana para no depender de mirar una imagen.

---

## 2. Estado de partida (verificado hoy)

Estas comprobaciones se ejecutaron sobre este equipo antes de escribir el spec:

| Comprobación | Resultado |
|---|---|
| Configuración CMake + Ninja | MSVC 19.44.35229, Qt 6.8.3 (`D:/Qt/6.8.3/msvc2022_64`), GDAL 3.12.4 (`D:/VCPKG/installed/x64-windows`), Release |
| Compilación completa | `minigis.exe` enlazado y desplegado con sus DLL y plugins |
| `minigis --version` | Qt 6.8.3 · GDAL 3.12.4 · PROJ 9.9.0 · MSVC 19.44 |
| `ctest` | **9/9 pruebas superadas** (`help`, `version`, `info`, `snapshot`, `salida-por-error-de-archivo`, `bench`, `indice-envolventes`, `redibujado`, `etiquetas`) |

**Desviaciones de entorno detectadas** (importantes para que los comandos de la
sección 6 sean ejecutables):

* `CMakePresets.json`, `scripts/env.bat` y el README apuntan a
  `D:/QtWin/6.10.3` y `E:/vcpkg`, que **no existen** en este equipo. El Qt
  instalado es 6.8.3 (`D:/Qt/6.8.3/msvc2022_64`) y vcpkg está en `D:/VCPKG`.
* El `rc.exe` del SDK 10.0.28000 hace fallar el enlace (`LNK1327: invalid
  option: -ologo`). Se fija el de 10.0.26100.0 en la configuración.
* El compilador del kit de Qt Creator es MinGW/GCC 13, que **rechaza** el código
  actual (`default member initializer ... required before the end of its
  enclosing class` en `minigisgeometrystore.h`). MSVC es el toolchain que
  compila el proyecto.

El motor vectorial (almacén indexado, renderizador acelerado, etiquetas, caché
de cuadro) **no se modifica** salvo dos puntos de integración: el orden de
dibujado y la revisión de contenido que invalida la caché.

---

## 3. Requisitos funcionales

### R1 — Apertura y detección de tipo

* Se abre un archivo con `GDALOpenEx` en modo lectura. La detección es **por
  contenido, no por extensión**: si el dataset tiene capas vectoriales se trata
  como vectorial (comportamiento actual); si no, si tiene bandas, como ráster.
* Un archivo que GDAL abre en ambos modos (p. ej. un GeoPackage con teselas y
  capas) se trata como **vectorial** en esta versión (documentado en §5).
* Un archivo que no es ni una cosa ni la otra devuelve un `minigis::Error` con
  causa y acción sugerida (`gdalinfo`), código de salida 3 en la CLI.

### R2 — Modelo de capa ráster

`RasterLayer` expone, como mínimo:

| Dato | Uso |
|---|---|
| ruta, nombre, driver | panel, título, `--info` |
| ancho × alto en píxeles | panel, `--info` |
| número de bandas, tipo de dato de cada una y su interpretación de color | `--info`, elección de bandas |
| SRC (EPSG y descripción) | reproyección, panel |
| geotransform | encuadre y lectura por ventana |
| extensión en su SRC y `extentInEpsg(destino)` | zoom, encuadre, orden de dibujado |
| NoData por banda | transparencia |
| tabla de color (si existe) | rasters paletizados |
| overviews disponibles | calidad de muestreo |
| visibilidad, opacidad y estilo (revisión) | redibujado |

`extentInEpsg()` transforma las **cuatro esquinas y puntos intermedios de cada
borde** (no solo las esquinas), para que un ráster en un SRC con curvatura no
quede recortado.

### R3 — Capas en el lienzo y en el panel

* El lienzo mantiene las capas ráster en su propia lista y las dibuja **antes**
  que las vectoriales, respetando el orden de carga dentro del grupo ráster (la
  última cargada queda encima). Las vectoriales conservan su orden y su
  comportamiento actual.
* `zoomToFullExtent()`, `zoomToLayer()` y `layersExtent()` incluyen los rásters
  visibles.
* El panel de capas muestra las dos clases en una sola lista, con la misma
  semántica de orden que el lienzo (primera fila = fondo):
  * columna **Capa**: nombre + casilla de visibilidad;
  * columna **Geometría**: `Vectorial (Polígono)` / `Ráster (3 bandas)`;
  * columna **Elementos**: recuento vectorial / `anchoxalto` del ráster;
  * columna **SRC**: igual que hoy.
* Para una capa ráster seleccionada se habilitan **Quitar**, **Zoom** y
  **Estilo**; se deshabilitan **Etiquetas** y **Tabla**.

### R4 — Dibujado fiel

* El ráster se dibuja en el **rectángulo de pantalla** que corresponde a su
  extensión, con muestreo **bilineal** por defecto (`nearest` seleccionable),
  sin deformar la relación de aspecto.
* Si el ráster es mayor que el lienzo, solo se lee la **ventana visible
  redimensionada al tamaño en píxeles del lienzo**: nunca se materializa el
  ráster completo en memoria.
* **NoData** se dibuja transparente; **banda alfa** se respeta; un ráster con
  tabla de color usa esa tabla (no una escala de grises).
* Un ráster de una banda se dibuja en **escala de grises** con el estiramiento
  configurado.
* **Modo borrador**: durante el arrastre/zoom se lee a media resolución y se
  escala al lienzo; al parar, el temporizador de calidad pide el cuadro bueno.
  El borrador no puede introducir costuras al reutilizar el cuadro anterior.
* **Caché de cuadro**: el ráster participa en la reutilización del cuadro
  anterior (es una traslación pura). La revisión de contenido incluye el estado
  de las capas ráster, de modo que cambiar el estilo, la opacidad o la
  visibilidad **redibuja de inmediato** (el fallo que ya se corrigió para el
  vectorial no se puede reintroducir para el ráster).

### R5 — Reproyección al SRC del lienzo

* Si el SRC de la capa coincide con el de destino, se lee y se dibuja sin warp.
* Si no coincide, se prepara un **warp con GDAL** (`GDALCreateWarpedVRT`, o
  `GDALReprojectImage` si el VRT no es viable) cacheado por SRC de destino, y
  se lee por ventana sobre el resultado. La reproyección debe usar los
  overviews del origen cuando existan.
* La extensión que encuadra la capa (`extentInEpsg`) y la posición del dibujado
  deben coincidir: la esquina del ráster tiene que caer en el píxel que le
  corresponde con un error ≤ 1,5 px (criterio SC4).

### R6 — Estiramiento de contraste

Estilos de estiramiento por capa:

| Modo | Comportamiento | Por defecto |
|---|---|---|
| `percentclip` | Recorte de percentiles (2–98 configurable) de una muestra de la banda | **sí, 2–98** |
| `minmax` | Mínimo y máximo reales de la muestra | no |
| `none` | Byte tal cual (0–255); en otros tipos, normalización de tipo | no |

* Las estadísticas se calculan al abrir, con una **lectura decimada** de como
  máximo 512×512 píxeles por banda (usa overviews si existen): nunca se recorre
  el ráster completo para pintarlo.
* El estiramiento se aplica con tabla de 256 entradas (LUT), no por píxel.
* Un valor NoData nunca entra en el cálculo de percentiles.

### R7 — Opacidad y bandas

* Opacidad 0–100 % por capa (por defecto 100).
* Para rasters de 3 o más bandas: selección de bandas **R, G, B** y banda alfa
  (ninguna, 4.ª banda…), con 1, 2, 3 por defecto.
* Para 1 o 2 bandas: modo de una banda, con alfa opcional en la segunda.
* Cambiar cualquier parámetro sube la revisión de la capa y redibuja.

### R8 — Interfaz

* **Abrir** (`Ctrl+O`), arrastrar y soltar y archivos recientes aceptan rásters;
  el diálogo de archivos añade los filtros de ráster (`*.tif *.tiff *.img
  *.png *.jpg *.jpeg *.jp2 *.vrt`) y mantiene los vectoriales.
* Menú y botón **Estilo** abren un `RasterStyleDialog` con: bandas R/G/B,
  banda alfa, modo de estiramiento, mínimo/máximo (para `minmax`), percentiles
  (para `percentclip`), resampling y opacidad. Se comporta como el diálogo
  vectorial: modal, con *Aceptar/Cancelar* y *Restablecer*.
* La barra de estado informa del tamaño del ráster y de los ms de dibujado como
  ya hace con las capas vectoriales.
* `--snapshot-raster-dialog <archivo.png>` captura el diálogo sin abrir la
  aplicación (mismo patrón que `--snapshot-dialog` para etiquetas), para revisar
  el diseño y para las capturas de la documentación.

### R9 — Línea de comandos

* `--info` reconoce rásters (texto y `--json`). El JSON conserva las claves
  actuales de las capas vectoriales (`features`, SRC…) y añade `type` a todas
  (`"vector"` / `"raster"`); las ráster añaden `width`, `height`, `bands`
  (índice, tipo, color), `crs`, `epsg`, `extent`, `nodata` y `overviews`.
* `--snapshot` y `--snapshot-window` incluyen los rásters (debajo del
  vectorial).
* `--bench` incluye rásters: el banco mide el cuadro completo (ráster +
  vectorial) y el presupuesto `--bench-budget` se aplica igual.
* Nuevas opciones (contrato de la CLI, se documentan en `--help`):

| Opción | Efecto |
|---|---|
| `--raster-stretch <none\|minmax\|percentclip>` | fuerza el modo de estiramiento |
| `--raster-bands <r,g,b \| 1>` | fuerza la selección de bandas |
| `--raster-opacity <0-100>` | fuerza la opacidad |
| `--check-raster` | ejecuta las comprobaciones de §9 y sale (0 = todo bien) |
| `--snapshot-raster-dialog <archivo.png>` | captura el diálogo de estilo ráster |

* Códigos de salida: los actuales (0 éxito, 1 uso, 2 no existe, 3 GDAL no pudo
  abrir, 4 no se pudo escribir, 5 sin capas, 6 presupuesto de rendimiento). Un
  valor inválido en las opciones nuevas → 1.

### R10 — Rendimiento y memoria

* Objetivo inicial, medido sobre un GeoTIFF de **4096×4096 con overviews**,
  lienzo de 1200×800, en Release y en este equipo:
  * cuadro de calidad: **p90 ≤ 100 ms**, mediana ≤ 70 ms;
  * cuadro de borrador: **p90 ≤ 45 ms**, mediana ≤ 30 ms.
* La lectura por cuadro se acota al tamaño del lienzo: bytes leídos ≤
  `ancho × alto × bandas × 4` (≈ 11,5 MB con 3 bandas a 1200×800). El renderizador
  lo cuenta y `--check-raster` lo verifica.
* Un ráster sin overviews se dibuja correctamente pero más lento; el objetivo
  de rendimiento se mide con overviews y así se documenta.

### R11 — Diagnóstico verificable

`--check-raster` ejecuta, sobre las capas indicadas, las comprobaciones de la
sección 9 y escribe una línea `OK …` por comprobación (y `FALLO …` con código 1
si algo no cuadra). Es el equivalente de `--check-refresh` y `--check-labels`
para el ráster, y la base de las pruebas de CTest.

### R12 — No regresión

* Las 9 pruebas actuales siguen pasando sin cambios de comportamiento.
* El panel, los atajos, la tabla de atributos, las etiquetas y la reutilización
  de cuadro vectorial se comportan igual. Si un cambio de arquitectura amenaza
  esto, se replantea antes de tocar el renderizador vectorial.

---

## 4. Decisiones de dibujo (y por qué)

1. **Lista paralela de rásters, no una jerarquía de capas genérica.** El lienzo,
   el panel, el cargador y la tabla están tipados sobre `VectorLayer` y
   funcionan; introducir una clase base `Layer` obliga a tocar todos esos
   caminos y arriesga las 9 pruebas verdes. En esta versión el ráster se dibuja
   siempre debajo del vectorial y el panel lo refleja. La jerarquía genérica
   queda como refactor futuro (§5).
2. **Ventana visible + tamaño de lienzo.** Leer el ráster completo o copiarlo a
   memoria no escala (un 8192² RGB son 200 MB). Leer solo lo visible al tamaño
   de pantalla acota el coste por cuadro por diseño.
3. **Warp cacheado por SRC de destino.** Crear el VRT por cuadro costaría más
   que dibujar; el SRC del lienzo cambia poco y el VRT se reutiliza.
4. **Estadísticas de una muestra decimada.** Los percentiles exactos exigen
   recorrer el ráster; una muestra de 512×512 con overviews da un contraste
   estable en milisegundos y es suficiente para ver.
5. **LUT de 256 entradas.** Convierte banda→RGBA con una tabla, sin ramas por
   píxel; es la diferencia entre decenas y cientos de ms en un cuadro.
6. **El ráster participa en la caché de cuadro con su revisión.** El fallo de
   "cambio el estilo y el mapa no se entera" ya ocurrió con el vectorial; la
   revisión de contenido es la defensa que funcionó y se extiende al ráster.

---

## 5. Fuera de alcance (explícito)

* Edición, guardado o creación de rásters; recorte, mosaico, fusión de capas.
* Hillshade, rampas de color multicolor para DEM, cálculo de pendientes,
  curvas de nivel (el estiramiento en grises sí entra).
* Ráster por encima de capas vectoriales y reordenación de capas en el panel.
* Cambiar el SRC del lienzo desde la interfaz (sigue fijo en EPSG:4326; `--crs`
  existe en la CLI, como hoy).
* Tabla de atributos o etiquetas para rásters.
* Teselas, GPU, streaming de rásters gigantes sin overviews.
* Un GeoPackage que contenga a la vez vectorial y ráster se abre como vectorial.
* Formatos exóticos más allá de lo que GDAL soporte: funcionan por construcción,
  pero las pruebas cubren GeoTIFF (RGB, DEM y paleta).

---

## 6. Comandos

### 6.1 Compilar y probar (verificado hoy)

Consola **x64 Native Tools de VS 2022** (o `vcvars64.bat`), desde la raíz del
repositorio:

```bat
cmake -S . -B build\raster -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_RC_COMPILER="D:/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe" ^
  -DCMAKE_MT="D:/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe" ^
  -DQt6_DIR=D:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6 ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64 ^
  -DCMAKE_TOOLCHAIN_FILE=D:/VCPKG/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DVCPKG_INSTALLED_DIR=D:/VCPKG/installed

cmake --build build\raster --parallel
ctest --test-dir build\raster --output-on-failure
```

> El `rc.exe` explícito es necesario en este equipo: el del SDK 10.0.28000 rompe
> el enlace con `LNK1327`. Es el mismo comando con el que se verificaron las 9
> pruebas de §2.

### 6.2 Datos de ejemplo (nuevo)

```bat
:: GeoTIFF pequeños y deterministas (RGB, DEM UTM con NoData y paleta)
"D:\OSGeo4w\apps\Python312\python.exe" scripts\make_sample_rasters.py

:: Ráster grande solo para medir (no se versiona; va a build\)
"D:\OSGeo4w\apps\Python312\python.exe" scripts\make_sample_rasters.py --big build\perf
```

Los tres pequeños se generan también desde `scripts\make_sample_data.bat`, que
ya existe para los shapefiles.

### 6.3 Uso

```bat
:: información (texto y JSON)
build\raster\bin\minigis.exe --info -l data\sample\rasters\ortho_rgb.tif
build\raster\bin\minigis.exe --info --json -l data\sample\rasters\dem_utm30.tif

:: dibujo sin ventana
build\raster\bin\minigis.exe --snapshot mapa.png -l data\sample\rasters\ortho_rgb.tif ^
        -l data\sample\shapefiles\parcels.shp --size 1200x800

:: comprobaciones del ráster (contrato de las pruebas)
build\raster\bin\minigis.exe --check-raster ^
        -l data\sample\rasters\ortho_rgb.tif ^
        -l data\sample\rasters\dem_utm30.tif ^
        -l data\sample\rasters\landcover.tif

:: captura de la interfaz con el ráster en el panel y en el lienzo
build\raster\bin\minigis.exe --snapshot-window ventana.png ^
        -l data\sample\rasters\ortho_rgb.tif -l data\sample\shapefiles\parcels.shp

:: captura del diálogo de estilo
build\raster\bin\minigis.exe --snapshot-raster-dialog dialogo.png ^
        -l data\sample\rasters\dem_utm30.tif

:: medida de cuadros (contrato JSON de --bench, ahora con ráster)
build\raster\bin\minigis.exe --bench 24 --bench-scenario mixed --json ^
        -l build\perf\terrain_big.tif --size 1200x800
```

### 6.4 Puerta de rendimiento (opcional, con dataset propio)

```bat
cmake -S . -B build\rendimiento-raster -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_RC_COMPILER="D:/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe" ^
  -DCMAKE_MT="D:/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe" ^
  -DQt6_DIR=D:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6 ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64 ^
  -DCMAKE_TOOLCHAIN_FILE=D:/VCPKG/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DVCPKG_INSTALLED_DIR=D:/VCPKG/installed ^
  -DMINIGIS_RASTER_BENCH_DATASET=D:/datos/terrain.tif ^
  -DMINIGIS_BENCH_BUDGET_MS=100

cmake --build build\rendimiento-raster --parallel
ctest --test-dir build\rendimiento-raster -R minigis.rendimiento-raster --output-on-failure
```

(La variable y la prueba se añaden en esta entrega, con el mismo patrón que
`MINIGIS_BENCH_DATASET`.)

---

## 7. Estructura del proyecto

```
src/core/
  minigisrasterlayer.h/.cpp     capa ráster: apertura, metadatos, extensión, estilo, revisión
  minigisrasterstyle.h          bandas, estiramiento, opacidad, resampling
  minigisrasterrenderer.h/.cpp  dibujado por ventana: warp, LUT, nodata, alfa, borrador
src/gui/
  minigisrasterstyledialog.h/.cpp  diálogo de estilo ráster
  minigismapcanvas.*            lista de rásters, orden de dibujo, extensión, revisión
  minigislayerpanel.*           filas de ráster, botones habilitados/deshabilitados
  minigismainwindow.*           apertura/clasificación, menús, zoom, estado
src/app/main.cpp                --info, --snapshot, --bench, --check-raster,
                                --snapshot-raster-dialog, opciones --raster-*
tests/
  run_cli_test.cmake            modos raster-info, check-raster, raster-dialog
  CMakeLists.txt                pruebas nuevas y su dataset
scripts/
  make_sample_rasters.py        generador determinista (GDAL + numpy)
  make_sample_data.bat          añade la llamada al generador de rásters
data/sample/rasters/            ortho_rgb.tif, dem_utm30.tif, landcover.tif
README.md                       uso, estructura, pruebas y límites del ráster
RENDIMIENTO.md                  sección con las medidas del ráster
```

Regla de dependencias: `core` no incluye cabeceras de `gui`; `app` no contiene
lógica de datos; `gui` no llama a GDAL salvo a través de `core`.

---

## 8. Estilo de código

Se mantiene el estilo del proyecto (C++17, `namespace minigis`, miembros `m`,
2 espacios, llaves en línea nueva, comentarios en español que explican el *por
qué*, no el *qué*). Cabeceras autocontenidas y RAII para todo recurso de GDAL.

Ejemplo real del proyecto (referencia de estilo):

```cpp
class VectorLayer
{
public:
  static std::shared_ptr<VectorLayer> open(const QString &path, Error *error = nullptr);

  bool isVisible() const { return mVisible; }
  void setVisible(bool visible)
  {
    if (mVisible == visible)
      return;
    mVisible = visible;
    bumpRevision();     // la cache de cuadro depende de esto
  }

private:
  void bumpRevision() { ++mRevision; }
  bool mVisible = true;
  quint64 mRevision = 1;
};
```

Convenciones que hereda el ráster:

* miembro `mXxx`, método en minúscula inicial, tipo en mayúscula inicial;
* `quint64`/`qint64` para contadores y tamaños, `double` para coordenadas;
* cada error de apertura devuelve `Error` (mensaje + acción) y nunca una cadena
  suelta;
* comentario de clase en el `.h` explicando la responsabilidad y las decisiones
  no obvias.

---

## 9. Estrategia de pruebas

### 9.1 Datos de ejemplo (deterministas)

| Archivo | Tamaño | Bandas / tipo | SRC y extensión | Contenido |
|---|---|---|---|---|
| `ortho_rgb.tif` | 256×256 | 3 × Byte | EPSG:4326 · `[-4.6, 39.8, -3.0, 41.2]` | 4 cuadrantes de color plano (rojo, verde, azul, blanco) |
| `dem_utm30.tif` | 256×256 | 1 × Int16 | EPSG:25830 · `[430000, 4470000, 431000, 4471000]` | 100 + (fila+columna) % 11; un píxel 1000; NoData −9999 en un bloque 16×16 |
| `landcover.tif` | 128×128 | 1 × Byte + paleta | EPSG:4326 · misma que `ortho_rgb` | 3 clases: bosque, cultivo, agua; 0 = NoData |
| `terrain_big.tif` | 4096×4096 | 1 × Byte + overviews 2–16 | EPSG:4326 | Solo rendimiento; se genera en `build\perf`, no se versiona |

### 9.2 Pruebas de CTest

| Prueba | Comando / modo | Qué exige |
|---|---|---|
| `minigis.raster-info` | `--info --json` sobre `ortho_rgb`, `dem_utm30` y `landcover` | `"type": "raster"` en las tres, `width`/`height`, 3 bandas en la ortofoto, `EPSG:25830` en el DEM |
| `minigis.raster-dibujo` | `--check-raster` con las tres capas | todas las líneas `OK` de §9.3 y ninguna `FALLO` |
| `minigis.raster-ventana` | `--snapshot-window` con ráster + parcelas | PNG escrito (> 20 KB): el ráster aparece en panel y lienzo |
| `minigis.raster-dialogo` | `--snapshot-raster-dialog` | PNG del diálogo escrito |
| `minigis.rendimiento-raster` | opcional, con `MINIGIS_RASTER_BENCH_DATASET` | p90 ≤ presupuesto (`--bench-budget`) |

### 9.3 Contrato de `--check-raster`

1. **Cuadrantes en su sitio**: el color de cada cuadrante en el píxel que le
   corresponde coincide con el esperado (±2 por canal).
2. **Extensión**: los bordes del ráster caen en el píxel esperado (±1 px).
3. **Reproyección**: una esquina del DEM en UTM 30N cae en el píxel que dicta la
   transformación de GDAL (±1,5 px).
4. **NoData transparente**: el bloque NoData del DEM se ve exactamente del color
   de fondo del lienzo.
5. **Estiramiento**: con el píxel 1000, `percentclip` reparte el relieve por
   todo el rango (gris máximo − mínimo ≥ 200) y `minmax` lo aplana (≤ 10);
   ambos modos se ejercitan y comparan.
6. **Paleta**: las tres clases de `landcover` salen con su color exacto.
7. **Redibujado**: cambiar el estilo/opacidad y ocultar/mostrar la capa cambia
   el cuadro de inmediato (revisión de la caché).
8. **Paneo reutilizado**: reutilizar el cuadro da lo mismo que redibujar desde
   cero (0 píxeles distintos en calidad; ≤ 0,1 % en borrador, como ya se hace
   con el vectorial).
9. **Lectura acotada**: bytes leídos por cuadro ≤ `ancho × alto × bandas × 4`.

Cada comprobación imprime `OK <descripción> — <dato medido>`; `--json` añade un
objeto con los valores para las pruebas y para las medidas de la documentación.

### 9.4 Matriz requisito → prueba

| Requisito | Prueba |
|---|---|
| R1, R2 | `minigis.raster-info` |
| R3, R8 | `minigis.raster-ventana`, `minigis.raster-dialogo` (manual y captura) |
| R4 | `--check-raster` 1, 2, 4, 6, 8, 9 |
| R5 | `--check-raster` 3 |
| R6 | `--check-raster` 5 |
| R7 | `--check-raster` 7 |
| R9 | `minigis.raster-info`, contrato JSON de `--bench` |
| R10 | `minigis.rendimiento-raster` (opcional), `--check-raster` 9 |
| R12 | las 9 pruebas actuales |

---

## 10. Límites

**Siempre**

* Compilar con MSVC y ejecutar **todas** las pruebas (las 9 actuales + las
  nuevas) antes de dar una tarea por terminada.
* Cerrar los handles de GDAL con RAII; un dataset pertenece al hilo que lo abre
  y se dibuja en el hilo de la GUI.
* Errores con causa y acción sugerida; nada de `return nullptr` sin explicar.
* Añadir una comprobación a `--check-raster` por cada comportamiento nuevo que
  pueda romperse en silencio (píxeles, no milisegundos).
* Documentar en README las opciones y límites nuevos.

**Preguntar antes**

* Cambiar la arquitectura del lienzo (jerarquía de capas genérica) o tocar el
  renderizador vectorial más allá de los puntos de integración.
* Cambiar el contrato JSON de `--info` o `--bench` que ya consumen las pruebas.
* Añadir dependencias (no se prevé ninguna: GDAL y Qt ya están).
* Modificar `CMakePresets.json` / `scripts/env.bat` para las rutas reales.
* Cambiar los datos de ejemplo vectoriales existentes.

**Nunca**

* Modificar o reutilizar código del repositorio QGIS (política QEP-408 del
  proyecto).
* Escribir en el ráster de origen ni convertirlo en el disco sin pedirlo.
* Materializar un ráster completo en memoria para dibujarlo.
* Debilitar o borrar una prueba existente para que pase un cambio nuevo.
* Silenciar un fallo con un valor por defecto que oculte el problema (p. ej.,
  dibujar negro cuando el warp falla).

---

## 11. Criterios de éxito

Se considerará terminado cuando **todo** esto sea cierto y reproducible:

| # | Criterio | Cómo se comprueba |
|---|---|---|
| SC1 | Compila en Release con MSVC/Qt 6.8.3/GDAL 3.12.4 | §6.1 |
| SC2 | `--info --json` describe RGB, DEM y paleta con driver, tamaño, bandas, SRC, extensión y NoData | `minigis.raster-info` |
| SC3 | Los cuatro cuadrantes de `ortho_rgb` salen en su posición y color (±2) | `--check-raster` |
| SC4 | El DEM en UTM se coloca sobre el lienzo EPSG:4326 con error ≤ 1,5 px | `--check-raster` |
| SC5 | El bloque NoData es exactamente del color de fondo | `--check-raster` |
| SC6 | `percentclip` reparte el relieve por todo el rango (≥ 200) y `minmax` lo aplana (≤ 10) frente al valor atípico | `--check-raster` |
| SC7 | La paleta de `landcover` se respeta (3/3 colores exactos) | `--check-raster` |
| SC8 | Cambiar estilo, opacidad o visibilidad redibuja al instante | `--check-raster` |
| SC9 | El paneo reutilizado con ráster coincide con el redibujado (0 px en calidad) | `--check-raster` |
| SC10 | Lectura por cuadro ≤ `ancho × alto × bandas × 4` | `--check-raster` |
| SC11 | Calidad p90 ≤ 100 ms y borrador p90 ≤ 45 ms en 4096² con overviews | `--bench` + §6.4 |
| SC12 | Panel, diálogo, apertura y captura funcionan (ráster listado, estilo aplicado) | `minigis.raster-ventana`, `minigis.raster-dialogo` |
| SC13 | Las 9 pruebas existentes siguen verdes | `ctest` |
| SC14 | README y RENDIMIENTO.md describen el ráster y sus medidas | revisión |

---

## 12. Supuestos (corregir si alguno no es cierto)

1. El ráster se dibuja **siempre debajo** del vectorial; el orden entre rásters
   es el de carga.
2. El SRC del lienzo sigue fijo en EPSG:4326 en la interfaz; `--crs` solo en la
   CLI.
3. Por defecto: estiramiento por percentiles 2–98, muestreo bilineal, opacidad
   100 %, NoData activo.
4. Las estadísticas de contraste salen de una muestra decimada (≤ 512×512 por
   banda); no se recorre el ráster completo al abrirlo.
5. La apertura es **síncrona** (solo metadatos y muestra de estadísticas, pocos
   ms); no se añade un cargador en segundo plano para ráster en esta versión.
6. Se usa la API C de GDAL (`GDALOpenEx`, `GDALRasterIO`,
   `GDALCreateWarpedVRT`), coherente con la convención del proyecto.
7. Los rásters de prueba se versionan (≈ 200 KB en total); el grande de
   rendimiento se genera en `build\` y no se versiona.
8. El toolchain de referencia es MSVC; GCC/MinGW no se soporta en esta entrega
   (el código actual ni siquiera compila con él).
9. `--bench` seguirá midiendo paneo/zoom mixto; el presupuesto se aplica al
   cuadro completo, con o sin ráster.
10. El diálogo de estilo no lleva vista previa (el vectorial tampoco la tiene);
    el resultado se ve en el lienzo al aceptar.

---

## 13. Preguntas abiertas (para la revisión)

1. **¿Actualizamos `CMakePresets.json` y `scripts/env.bat` a las rutas reales**
   (`D:/Qt/6.8.3`, `D:/VCPKG`) como parte de esta entrega? Lo propuesto es que
   sí: sin eso, los comandos documentados del README no funcionan en este
   equipo y "compilar" depende de copiar una línea larga a mano.
2. **¿Rampas de color para el DEM** (verde→marrón→blanco) en esta versión, o
   solo grises + estiramiento? Propuesto: solo grises; la rampa es un añadido
   pequeño pero con su propio diálogo y pruebas.
3. **¿Hillshade?** Propuesto: fuera; es otra función (derivadas, azimuth,
   escala) y merece su propio spec.
4. **¿Ráster por encima de vectorial o reordenación de capas?** Propuesto: no;
   el panel no reordena hoy y hacerlo obliga a tocar el modelo de capas.
5. **¿Los objetivos de rendimiento (p90 100 ms calidad / 45 ms borrador) son
   los correctos?** Se confirman con la primera medida real en §6.4; si el
   equipo da menos margen, se ajusta el presupuesto, no la funcionalidad.
6. **¿La captura del diálogo (`--snapshot-raster-dialog`) es necesaria** o basta
   con `--snapshot-window`? Propuesto: sí, es barata y permite revisar el
   diseño sin abrir la aplicación, igual que se hizo con etiquetas.
