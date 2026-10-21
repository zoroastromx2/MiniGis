# MiniGIS

Visor SIG mínimo en **C++ con Qt 6 y GDAL**: carga capas vectoriales (shapefile, GeoJSON,
GeoPackage, KML…) y **ráster** (GeoTIFF y cualquier formato que abra GDAL), las dibuja, permite
hacer zoom y paneo, consultar la tabla de atributos, cambiar la simbología básica y ajustar el
contraste de los rásters.

El objetivo del proyecto es tener un programa pequeño, comprensible y **depurable paso a paso
en Qt Creator**, construido con las mismas convenciones que usa QGIS: CMake, vcpkg en modo
manifiesto, capas separadas `core` / `gui` / `app` y GDAL/OGR como motor de datos.

---

## 1. Aviso importante sobre QGIS

El repositorio `QGISa` que hay junto a este proyecto contiene su propio `AGENTS.md` con la
política oficial del proyecto QGIS (QEP-408): **no se permiten contribuciones asistidas por
IA** (código, *pull requests*, *issues*, revisiones…).

Por eso **MiniGIS no modifica ni reutiliza el código de QGIS**. Se ha usado ese repositorio
únicamente como **referencia de lectura** para copiar sus *convenciones de compilación*:

| Convención de QGIS | Cómo se aplica en MiniGIS |
|---|---|
| `cmake_minimum_required(VERSION 3.22...4.3)` | Igual en `CMakeLists.txt` |
| Módulos propios en `cmake/` | `cmake/MiniGisVcpkg.cmake`, `cmake/MiniGisRuntime.cmake` |
| vcpkg con manifiesto `vcpkg/vcpkg.json` | Igual, con `baseline` fijado |
| `find_package(GDAL REQUIRED)` | Igual (`GDAL::GDAL`, vía `GDALConfig.cmake` de vcpkg) |
| Capas `src/core`, `src/gui`, `src/app` | Igual (bibliotecas `minigis_core`, `minigis_gui` + ejecutable) |
| Listas de fuentes explícitas (sin `GLOB`) | Igual |

MiniGIS es, por tanto, un proyecto **independiente y downstream**: usa GDAL como dependencia
externa igual que cualquier otra aplicación SIG.

---

## 2. Estado del entorno detectado

| Componente | Ruta | Versión |
|---|---|---|
| Qt (oficial) | `D:\Qt\6.8.3\msvc2022_64` | 6.8.3 |
| Qt Creator | `D:\Qt\Tools\QtCreator\bin\qtcreator.exe` | 19.0.2 |
| Compilador | `C:\Program Files\Microsoft Visual Studio\2022\Community` | MSVC 19.44 (x64) |
| CMake | `C:\Strawberry\c\bin\cmake.exe` (el del `PATH`) | 3.29.2 (el kit trae 3.30.5 en `D:\Qt\Tools\CMake_64`) |
| Ninja | `C:\Strawberry\c\bin\ninja.exe` | 1.12.0 |
| vcpkg | `D:\VCPKG` | GDAL 3.12.4 (`x64-windows`), release **y** debug |
| Windows SDK (rc/mt) | `D:\Windows Kits\10\bin\10.0.26100.0\x64` | 10.0.26100 (fijado en los presets; el 28000 rompe el enlace) |
| Datos de ejemplo | `data\sample\shapefiles` (generados con `ogr2ogr`) | — |

> Estas son las rutas de este equipo. Si cambian, edita `scripts\env.bat` (el
> único sitio con rutas absolutas para los scripts) y las variables de
> `CMakePresets.json`.

---

## 3. Estructura del proyecto

```
MiniGIS/
├── CMakeLists.txt              # configuración raíz (Qt 6 + GDAL + opciones)
├── CMakePresets.json           # presets Debug / RelWithDebInfo / Release
├── vcpkg/vcpkg.json            # manifiesto vcpkg (dependencia: gdal)
├── cmake/
│   ├── MiniGisVcpkg.cmake      # localiza vcpkg y el árbol instalado
│   ├── MiniGisRuntime.cmake    # copia de DLLs junto al ejecutable
│   └── MiniGisCopyDlls.cmake   # script de copia (post-build)
├── src/
│   ├── core/                   # datos y render (equivalente a qgis_core)
│   │   ├── minigisgdalinit.*   # registro de drivers, PROJ_DATA/GDAL_DATA
│   │   ├── minigislayerkind.*  # detección vectorial/ráster por contenido
│   │   ├── minigisvectorlayer.*# capa vectorial sobre GDAL/OGR
│   │   ├── minigisrasterlayer.*# capa ráster: metadatos, extensión, warp cacheado
│   │   ├── minigisrasterrenderer.* # dibujo por ventana, LUT, NoData, paleta
│   │   ├── minigisrasterstyle.h# bandas, estiramiento, muestreo, opacidad
│   │   ├── minigismaprenderer.*# render a QImage + reproyección al vuelo
│   │   ├── minigisrectangle.*  # extensión/zoom
│   │   ├── minigisstyle.*      # simbología
│   │   └── minigiserror.h      # error = mensaje + acción sugerida
│   ├── gui/                    # widgets (equivalente a qgis_gui)
│   │   ├── minigismapcanvas.*  # lienzo: zoom, paneo, arrastrar y soltar
│   │   ├── minigislayerpanel.* # árbol de capas (vectorial y ráster)
│   │   ├── minigisattributetablemodel.*
│   │   ├── minigisstyledialog.*
│   │   ├── minigisrasterstyledialog.* # contraste, bandas y opacidad del ráster
│   │   └── minigismainwindow.*
│   └── app/main.cpp            # GUI + modos de línea de comandos
├── tests/                      # pruebas de humo con CTest
├── data/sample/                # GeoJSON origen, shapefiles y GeoTIFF de ejemplo
└── scripts/                    # env / configure / build / run / deploy / datos
```

---

## 4. Compilar

### 4.1 Con los scripts (lo más rápido)

```bat
scripts\make_sample_data.bat              :: genera los shapefiles y GeoTIFF de ejemplo
scripts\configure.bat windows-msvc-debug  :: configurar (Debug)
scripts\build.bat     windows-msvc-debug  :: compilar
scripts\run.bat       windows-msvc-debug  :: ejecutar
ctest --preset windows-msvc-debug         :: pruebas de humo
```

`scripts\env.bat` prepara el entorno (llama a `vcvars64.bat`, añade Qt y las DLLs de vcpkg al
`PATH`). Si tus rutas cambian, edita ese archivo: es el único sitio con rutas absolutas.

Presets disponibles:

| Preset | Uso |
|---|---|
| `windows-msvc-debug` | **Recomendado para Qt Creator**: símbolos completos, sin optimizar |
| `windows-msvc-reldbg` | Alternativa con dependencias release (útil si faltase GDAL debug) |
| `windows-msvc-release` | Binario optimizado para repartir |

### 4.2 Configuración manual equivalente

```bat
:: desde una consola "x64 Native Tools" de VS 2022 (o tras vcvars64.bat)
cmake -S . -B build\manual -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_RC_COMPILER=D:/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe" ^
  "-DCMAKE_MT=D:/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe" ^
  -DCMAKE_TOOLCHAIN_FILE=D:/VCPKG/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DVCPKG_INSTALLED_DIR=D:/VCPKG/installed ^
  -DQt6_DIR=D:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6 ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64
cmake --build build\manual
```

> El `rc.exe`/`mt.exe` explícitos son necesarios aquí: el del SDK 10.0.28000
> hace fallar el enlace con `LNK1327: invalid option: -ologo`. Los presets ya
> los fijan.

> **Nota (importante) sobre vcpkg.** Por defecto se usa el **árbol ya compilado**
> (`D:\VCPKG\installed`, `MINIGIS_USE_VCPKG_MANIFEST=OFF`). Activar el modo manifiesto
> (`-DMINIGIS_USE_VCPKG_MANIFEST=ON`) haría que vcpkg **recompilase GDAL y su cadena** en un
> árbol propio del proyecto; además, si se apuntase al árbol compartido, vcpkg **eliminaría**
> paquetes ajenos al manifiesto (por ejemplo `qtbase` o `qgis-o2`). Por eso el modo manifiesto
> fuerza un árbol `build/<preset>/vcpkg_installed` y avisa por consola.

---

## 5. Depurar en Qt Creator

1. **Abre el proyecto**: `File > Open File or Project…` → `D:\AAA\MiniGIS\MiniGisCode\CMakeLists.txt`.
2. **Selecciona el preset**: en `Projects > Build & Run > CMake`, elige
   `windows-msvc-debug` (Qt Creator los muestra en la lista de *presets*). Si prefieres
   configurar a mano, usa:
   * *Generator*: `Ninja`
   * *CMake tool*: `C:\Strawberry\c\bin\cmake.exe` (o `D:\Qt\Tools\CMake_64\bin\cmake.exe`)
   * *Kit*: el de Qt 6.8.3 MSVC2022 64bit
   * Variables de CMake: las del apartado 4.2.
3. **Kit**: `Qt 6.8.3 MSVC2022 64bit`. El compilador debe ser el de
   `C:\Program Files\Microsoft Visual Studio\2022\Community` (MSVC 19.44), que es el que
   detecta vcpkg.
4. **Entorno de ejecución**: el proyecto **no necesita que toques `PATH`**. Tras compilar,
   `MINIGIS_COPY_RUNTIME_DLLS=ON` copia junto al ejecutable Qt, GDAL y sus dependencias, y
   `MINIGIS_DEPLOY_QT_RUNTIME=ON` ejecuta `windeployqt` para desplegar los *plugins* de Qt
   (plataformas, estilos, formatos de imagen) más el plugin `offscreen` que usan los modos sin
   ventana. Si aun así quieres tenerlo a mano al depurar, añade en `Projects > Run >
   Environment`:
   ```
   D:\Qt\6.8.3\msvc2022_64\bin;D:\VCPKG\installed\x64-windows\bin
   ```
5. **Punto de entrada**: `src\app\main.cpp`. Pon un punto de ruptura en
   `MainWindow::openLayer` (`src\gui\minigismainwindow.cpp`) y abre un `.shp` con `Ctrl+O`:
   verás el recorrido `openLayer → VectorLayer::open → GDALOpenEx → MapRenderer::paintLayer`.
6. **Argumentos de depuración útiles** (`Projects > Run > Command line arguments`):
   * `data\sample\shapefiles\cities.shp` → arranca con el shapefile cargado
   * `--snapshot salida.png -l data\sample\shapefiles\parcels.shp` → render sin ventana
     (perfecto para depurar el motor de dibujo sin interactuar con la interfaz)
   * `--info --layer data\sample\shapefiles\parcels_utm30.shp` → comprobar lectura y SRC

> El ejecutable se construye como **aplicación de consola** (sin `WIN32`): así los modos de
> línea de comandos escriben en la consola y Qt Creator muestra esa salida en
> *Application Output*. Si prefieres no ver la consola, cambia `WIN32_EXECUTABLE` a `TRUE` en
> `src/app/CMakeLists.txt`.

---

## 6. Uso

```
minigis [opciones] [archivos...]
```

| Opción | Descripción |
|---|---|
| `archivos...` / `-l, --layer <archivo>` | Capas vectoriales o ráster a cargar (se pueden repetir) |
| `-i, --info` | Resumen de las capas (vectoriales y ráster: bandas, tamaño, SRC, NoData, paleta) |
| `--json` | Salida JSON (junto a `--info`), pensada para scripts |
| `--list-drivers` | Drivers vectoriales disponibles en GDAL |
| `--snapshot <archivo.png>` | Renderiza el mapa a PNG sin abrir ventana |
| `--snapshot-window <archivo.png>` | Captura la interfaz completa a PNG |
| `--extent <xmin,ymin,xmax,ymax>` | Extensión inicial del mapa |
| `--size <AnchoxAlto>` | Tamaño de la imagen (por defecto `1200x800`) |
| `--crs <EPSG:xxxx>` | SRC de destino del dibujo (por defecto `EPSG:4326`) |
| `--max-features <n>` | Límite de elementos dibujados por capa vectorial (`0` = sin límite) |
| `--raster-stretch <none\|minmax\|percentclip>` | Estiramiento de contraste de los rásters (por defecto percentiles 2–98) |
| `--raster-bands <R,G,B \| 1>` | Bandas que usan los rásters (una sola = escala de grises) |
| `--raster-opacity <0-100>` | Opacidad de los rásters |
| `--check-raster` | Comprueba el dibujado ráster: cuadrantes, extensión, warp, NoData, contraste, paleta, paneo y márgenes del warp |
| `--snapshot-raster-dialog <archivo.png>` | Captura el diálogo de estilo ráster y sale |
| `-V, --verbose` | Trazas en `stderr` (los datos siguen limpios en `stdout`) |
| `-h, --help` / `-v, --version` | Ayuda con ejemplos / versiones de Qt, GDAL y PROJ y rutas de datos |

**Códigos de salida** (contrato estable, usado por las pruebas):

| Código | Significado |
|---|---|
| 0 | Éxito |
| 1 | Uso incorrecto |
| 2 | Archivo no encontrado |
| 3 | GDAL no pudo abrir la capa |
| 4 | No se pudo escribir la imagen |
| 5 | No hay capas que renderizar |

Ejemplos:

```bat
minigis data\sample\shapefiles\cities.shp
minigis --info --json -l data\sample\shapefiles\parcels.shp
minigis --snapshot mapa.png -l data\sample\shapefiles\parcels_utm30.shp --size 1600x900

:: raster: la ortofoto debajo de las parcelas, el DEM reproyectado a 4326
minigis --snapshot mapa.png -l data\sample\rasters\ortho_rgb.tif ^
        -l data\sample\shapefiles\parcels.shp
minigis --snapshot dem.png -l data\sample\rasters\dem_utm30.tif --crs EPSG:4326
minigis --snapshot paleta.png -l data\sample\rasters\landcover.tif --raster-stretch none

:: contrato del dibujado raster (lo que ejecutan las pruebas de CTest)
minigis --check-raster -l data\sample\rasters\ortho_rgb.tif ^
        -l data\sample\rasters\dem_utm30.tif -l data\sample\rasters\landcover.tif
```

En la interfaz:

* `Ctrl+O` abrir capa (vectorial o ráster) · `Ctrl+W` cerrar capa · `Ctrl+Q` salir
* `Ctrl+ +` / `Ctrl+ -` zoom · `Ctrl+Shift+F` extensión completa · `Ctrl+Shift+L` zoom a la capa
* rueda del ratón = zoom anclado al cursor · arrastrar con el botón izquierdo = paneo
* `F6` tabla de atributos · `Ctrl+Shift+Y` simbología (en un ráster, diálogo de contraste)
* arrastrar un `.shp` o un `.tif` desde el Explorador y soltarlo sobre el mapa

---

## 7. Qué hace y qué no hace

**Hace**

* Abre cualquier formato vectorial soportado por GDAL/OGR (probado con shapefile).
* Dibuja puntos, líneas y polígonos **con huecos**; reproyecta al vuelo si el SRC de la capa
  difiere del de destino (por ejemplo, un shapefile UTM sobre un mapa en EPSG:4326), tanto las
  geometrías como la extensión con la que se encuadra la capa.
* **Ráster de solo lectura**: abre cualquier formato que GDAL reconozca como ráster (probado con
  GeoTIFF), lo dibuja **debajo del vectorial**, lo reproyecta al SRC del lienzo (warp cacheado por
  capa, SRC y escala), respeta NoData y banda alfa, usa la paleta del archivo y permite elegir bandas,
  estiramiento de contraste (percentiles 2–98, mín-máx o ninguno), remuestreo y opacidad por capa.
  La lectura por cuadro se limita a la ventana visible, a la resolución del lienzo. Al reproyectar,
  todas las capas comparten la misma rejilla de píxel (a la escala del lienzo) y el dibujado se
  recorta a la huella real del ráster, de modo que las hojas de un mosaico encajan sin márgenes
  negros ni costuras.
* Tabla de atributos paginada y bajo demanda (máximo 100 000 filas mostradas).
* Simbología: color de relleno/contorno, grosor, tamaño de punto y opacidad.
* **Etiquetas por capa**: uno o varios campos, familia y tamaño de fuente, negrita/cursiva,
  color, halo (color y grosor), margen de escala en el que aparecen, tamaño en puntos de
  pantalla o en unidades del mapa, colocación respecto al elemento con desplazamiento,
  distancia mínima entre etiquetas, tamaño mínimo del elemento, máximo de etiquetas por
  cuadro, descarte de textos repetidos y prioridad (menú **Ver → Etiquetas de la capa…**,
  botón **Etiquetas** del panel o **F7**; **Ver → Mostrar etiquetas** las apaga todas). El
  diálogo indica en todo momento el tamaño real en píxeles y convierte el valor al cambiar
  entre puntos de pantalla y unidades del mapa, para que el texto no cambie de tamaño al
  cambiar de modo.
* Localiza automáticamente `proj.db` y los datos de GDAL si no hay variables de entorno.
* Errores accionables: cada fallo dice qué ha pasado y qué hacer (`minigis::Error`).

**No hace (todavía)**

* Ráster: edición, mosaicos, hillshade, rampas de color para DEM y ráster por encima del
  vectorial. Un ráster **sin NoData** reproyectado deja negras las esquinas fuera de su huella
  (con NoData salen transparentes).
* Edición de geometrías, guardado, simbología por categorías, servidores OGC.
* Etiquetas giradas o curvas siguiendo una línea, y control de prioridad por campo (la
  prioridad es un valor por capa).
* Reproyección del *render* vectorial con rejilla (*warp*): se transforman las geometrías; el
  ráster sí se reproyecta con GDAL.

---

## 8. Pruebas

```bat
ctest --preset windows-msvc-debug --output-on-failure
```

* `minigis.help` y `minigis.version` — contrato de la CLI.
* `minigis.info` — abre el shapefile de ejemplo con GDAL y comprueba que el JSON informa de
  `features` y de `EPSG:4326`.
* `minigis.snapshot` — renderiza el shapefile a PNG (plataforma `offscreen`) y verifica que la
  imagen no está vacía.
* `minigis.salida-por-error-de-archivo` — un archivo inexistente debe devolver el código 2.
* `minigis.bench` — contrato del banco de pruebas de rendimiento (`--bench --json`).
* `minigis.raster-info` — `--info --json` describe la ortofoto y el DEM (tipo, tamaño, bandas, SRC).
* `minigis.raster-dibujo` — `--check-raster` con las tres capas: cuadrantes, extensión, warp,
  NoData, contraste, paleta, redibujado, paneo reutilizado, lectura acotada y márgenes del warp.
* `minigis.raster-dialogo` — captura del diálogo de estilo ráster.
* `minigis.raster-ventana` — captura de la interfaz con ráster y vectorial.
* `minigis.rendimiento-raster` — puerta opcional de rendimiento con `MINIGIS_RASTER_BENCH_DATASET`.

### 8.1 Rendimiento

El despliegue vectorial está optimizado y **medido**: índice espacial en memoria, simplificación
por nivel de zoom, dibujado por lotes, modo borrador durante los gestos, reutilización del cuadro
anterior y carga en segundo plano con barra de progreso. El análisis completo, las medidas y el
trabajo pendiente están en **[RENDIMIENTO.md](RENDIMIENTO.md)**.

Las capas que **no caben en memoria** (millones de elementos) se indexan por envolventes leyendo el
`.shp` sin la geometría: así una capa de 3,24 M de edificios (584 MB) se abre en ~1 s con 146 MB de
índice, responde en 13 ms por cuadro a escala de ciudad y se dibuja como mapa de densidad con el
país entero en pantalla. A escalas muy lejanas la geometría se sustituye por puntos (envolventes),
porque a esa escala no aporta y leerla costaría segundos de I/O por cuadro.

```bat
:: medir el paneo/zoom sin abrir ventana (40 cuadros, con desglose por fase)
bin\minigis.exe --bench 40 --bench-scenario mixed -l capa.gpkg --size 1200x800 -V

:: comparar con el motor clásico de lectura directa
bin\minigis.exe --bench 40 --renderer legacy -l capa.gpkg --size 1200x800

:: puerta de regresión: falla con código 6 si el p90 supera el presupuesto
bin\minigis.exe --bench 24 --bench-budget 150 -l capa.gpkg --json

:: diagnosticar el índice de envolventes de un shapefile (lo verifica contra GDAL)
bin\minigis.exe --index-shp "E:\datos\capas\nz-building-outlines.shp"

:: comprobar que el mapa se redibuja al cambiar simbología o visibilidad, y que
:: el paneo reutilizado (copiar el cuadro y repintar la franja) no deja costuras
bin\minigis.exe --check-refresh -l data\sample\shapefiles\parcels.shp

:: comprobar el etiquetado: campos, escalas, solapes, repeticiones y caché de textos
bin\minigis.exe --check-labels -l data\sample\shapefiles\parcels.shp

:: etiquetas desde la linea de ordenes (util para capturas y para el banco de pruebas)
bin\minigis.exe --snapshot mapa.png -l capa.shp --label-field "nombre,uso" --label-size 11
bin\minigis.exe --snapshot mapa.png -l capa.shp --label-field nombre --label-map-size 25
bin\minigis.exe --snapshot mapa.png -l capa.shp --label-field nombre --no-labels

:: capturar el dialogo de etiquetas (para revisar el diseno sin abrir la aplicacion)
bin\minigis.exe --snapshot-dialog dialogo.png -l capa.shp --label-field nombre

:: contrato del dibujado raster: cuadrantes, extension, warp, NoData, contraste,
:: paleta, redibujado, paneo reutilizado y lectura acotada al lienzo
bin\minigis.exe --check-raster -l data\sample\rasters\ortho_rgb.tif ^
        -l data\sample\rasters\dem_utm30.tif -l data\sample\rasters\landcover.tif

:: medir el dibujado raster (JSON con el bloque "raster": bytes leidos y pixeles)
bin\minigis.exe --bench 24 --bench-scenario mixed --json ^
        -l build\perf\terrain_big.tif --size 1200x800

:: capturar el dialogo de estilo raster
bin\minigis.exe --snapshot-raster-dialog dialogo.png -l data\sample\rasters\dem_utm30.tif
```

El ráster se dibuja leyendo **solo la ventana visible**, a la resolución del lienzo, con una LUT
de contraste y el warp cacheado por SRC: un GeoTIFF de 4096×4096 con overviews tarda **4 ms de
mediana (p90 5 ms)** por cuadro a 1200×800, y menos de 1 ms en el cuadro de borrador a media
resolución. La puerta opcional de rendimiento raster:

```bat
cmake --preset windows-msvc-release ^
      -DMINIGIS_RASTER_BENCH_DATASET=C:/datos/terrain.tif ^
      -DMINIGIS_BENCH_BUDGET_MS=100
ctest --preset windows-msvc-release -R minigis.rendimiento-raster
```

Con una capa pesada (12.000 polígonos, 2,2 M de vértices, 47,8 MB) el cuadro de paneo pasa de
2.616 ms a 45 ms, y el cuadro de borrador que se ve al arrastrar, a 18 ms (56 fps). Activar la
puerta de rendimiento en CTest:

```bat
cmake --preset windows-msvc-release -DMINIGIS_BENCH_DATASET=C:/datos/capa.gpkg ^
      -DMINIGIS_BENCH_BUDGET_MS=150
ctest --preset windows-msvc-release -R minigis.rendimiento
```

---

## 9. Solución de problemas

| Síntoma | Causa y solución |
|---|---|
| `No se encuentra Qt` o Qt equivocado (vcpkg) | Fija el kit: `-DQt6_DIR=D:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6`. MiniGIS fija además las herramientas (`Qt6CoreTools`…) al mismo Qt para que vcpkg no mezcle su `qtbase` |
| Al configurar con el kit aparece `Qt6: 6.11.1 (D:/VCPKG/...)` o `Unknown CMake command "_qt_internal_should_include_targets"` | El toolchain de vcpkg antepone su árbol a `CMAKE_PREFIX_PATH`, y **Qt Creator inyecta un *provider* de `find_package` que busca justo ahí**, así que encontraba el `qtbase` de vcpkg y reescribía `Qt6_DIR`. MiniGIS lo evita: detecta el Qt del kit, descarta las variables `Qt6*_DIR` cacheadas que apunten a vcpkg y **excluye ese árbol de `CMAKE_PREFIX_PATH` y `CMAKE_FIND_ROOT_PATH` durante la búsqueda de Qt**. Basta con volver a configurar (Qt Creator → *Run CMake*); si ves el aviso `"debug_and_release" set to "OFF"` al cambiar de Qt, borra `build/<kit>` para partir de cero |
| `LNK1327: error durante la ejecución de rc.exe` / `RC1106: invalid option: -ologo` | El `rc.exe` del SDK 10.0.28000 no acepta la opción que le pasa el enlazador. Los presets fijan el de `D:/Windows Kits/10/bin/10.0.26100.0/x64`; en una configuración manual añade `-DCMAKE_RC_COMPILER` y `-DCMAKE_MT` como en 4.2 |
| `Target ... "gdal.dll" is not a directory` al copiar DLLs | Ocurría con CMake 3.30 (el del kit): la opción `-t` de `copy_if_different` no existe ahí. Ya se usa la forma `origenes... destino`, compatible con todas las versiones |
| `Could not find the Qt platform plugin "windows"` (o `"offscreen"`) al ejecutar | Los plugins de plataforma se copian **siempre** junto al ejecutable (antes dependían de `windeployqt`, que no existe si Qt sale de vcpkg). Si aun así falta, comprueba `build\<kit>\bin\platforms\` y recompila |
| Error `Mezcla de versiones de Qt: Qt6 6.x con Qt6Core 6.y` | `CMAKE_PREFIX_PATH` está apuntando a dos instalaciones de Qt: deja solo el Qt oficial |
| `ASSERT: "theme" in qstylehints.cpp` o `Could not find the Qt platform plugin` | Hay un Qt de vcpkg copiado junto al ejecutable. MiniGIS **excluye a propósito** las DLLs `Qt6*.dll` de vcpkg al copiar dependencias: borra `build\<preset>\bin\Qt6*.dll` y recompila |
| `gdal.dll` / `Qt6Core.dll` no encontradas al ejecutar | Ejecuta desde Qt Creator o con `scripts\run.bat`; para una carpeta autónoma usa `scripts\deploy.bat` |
| `could not find or load the Qt platform plugin "offscreen"` | `MINIGIS_DEPLOY_QT_RUNTIME` está en `OFF` y las pruebas necesitan ese plugin: actívalo, ejecuta `scripts\deploy.bat`, o define `QT_QPA_PLATFORM=windows` |
| Aviso `No se encontro proj.db` | Define `PROJ_DATA=D:\VCPKG\installed\x64-windows\share\proj` (o copia esa carpeta junto al `.exe`) |
| `La configuracion Debug necesita la variante Debug de GDAL` | Usa `windows-msvc-reldbg`, o instala la variante debug: `D:\VCPKG\vcpkg.exe install gdal --triplet x64-windows` |
| Aviso de que no se pudo detectar el prefijo de `/showIncludes` | Es MSVC en español sin paquete de idioma inglés. Compila igual, pero conviene instalar el idioma inglés en Visual Studio: así CMake registra las cabeceras y las recompilaciones incrementales son fiables |
| CMake no encuentra GDAL | `D:\VCPKG\vcpkg.exe install gdal --triplet x64-windows` y reconfigura |
| El shapefile se abre sin SRC | Al `.shp` le falta el `.prj`; usa `ogr2ogr ... -a_srs EPSG:xxxx` (ver `scripts\make_sample_data.bat`) |

---

## 10. Decisiones de diseño (y por qué)

* **Solo API C de OGR para leer datos** (`OGR_L_*`, `OGR_F_*`): es la superficie más estable
  entre versiones de GDAL; la API C++ de `OGRLayer` cambia con más frecuencia.
* **`minigis::Error` con causa + acción**: cada fallo de apertura explica qué ha pasado y qué
  hacer (comprobar los `.shx/.dbf`, usar `gdalinfo`, revisar el `.prj`…).
* **Reproyección al vuelo y extensión en el SRC del lienzo**: una capa UTM dibujada sobre un
  mapa en EPSG:4326 necesita que *tanto las geometrías como la extensión* estén en el SRC de
  destino (`VectorLayer::extentInEpsg`), si no la capa queda fuera de la vista.
* **Modos sin ventana para todo lo verificable** (`--info`, `--snapshot`, `--snapshot-window`):
  permiten comprobar el motor de datos y el de render sin pantalla, y son la base de las
  pruebas de CTest. La interfaz se construye también en `--snapshot-window` para detectar
  fallos de widgets, modelo de atributos y menús.
* **Nunca mezclar instalaciones de Qt**: la copia de dependencias de vcpkg excluye `Qt6*.dll`
  y CMake fija las herramientas de Qt sobre `Qt6_DIR`. Mezclar el Qt oficial con el de vcpkg
  produce fallos desconcertantes (plugins de plataforma ausentes, `ASSERT` en `qstylehints`).
* **Ejecutable de consola**: los modos CLI escriben en `stdout`/`stderr` (datos en `stdout`,
  diagnóstico en `stderr`) con códigos de salida estables, y Qt Creator muestra esa salida en
  *Application Output*.
* **Modo manifiesto de vcpkg desactivado por defecto**: en este equipo el árbol compartido
  `D:\VCPKG\installed` ya tiene GDAL compilado (release y debug). El modo manifiesto
  recompilaría la cadena completa y, apuntando al árbol compartido, **eliminaría** paquetes
  ajenos al manifiesto (`qtbase`, `qgis-o2`…). El manifiesto se conserva en `vcpkg/vcpkg.json`
  para documentar la dependencia y fijar el `baseline`.
* **Ráster por ventana y warp cacheado**: de un ráster solo se lee la parte visible, al tamaño
  del lienzo, y la reproyección se cachea por capa y SRC de destino. Es lo que permite mover un
  GeoTIFF de 4096² a 4 ms por cuadro sin copiarlo a memoria (ver RENDIMIENTO.md §13).

---

## 11. Verificación realizada

Todo lo siguiente se ejecutó sobre este equipo con el preset `windows-msvc-release`:

| Comprobación | Resultado |
|---|---|
| `scripts\configure.bat windows-msvc-release` | Qt 6.8.3, GDAL 3.12.4, vcpkg `x64-windows` (modo clásico) |
| `scripts\build.bat windows-msvc-release` | Compila sin errores; DLLs de vcpkg + Qt oficial + plugins desplegados |
| `minigis --version` | Qt 6.8.3, GDAL 3.12.4, PROJ 9.9.0, `PROJ_DATA` y `GDAL_DATA` en `D:\VCPKG` |
| `minigis --info --json -l cities.shp` | 12 elementos, `ESRI Shapefile`, `EPSG:4326 - WGS 84` |
| `minigis --info -l parcels_utm30.shp` | 5 polígonos, extensión en UTM 30N |
| `minigis --info --json -l ortho_rgb.tif` | `"type": "raster"`, 256×256, 3 bandas, `EPSG:4326` |
| `minigis --snapshot` sobre `parcels.shp` | PNG 900x600 (4 ms de render) |
| `minigis --snapshot --crs EPSG:4326 -l dem_utm30.tif` | DEM UTM reproyectado, NoData transparente y esquina con error ≤ 1,5 px |
| `minigis --check-raster` (tres capas) | **13 comprobaciones OK**: cuadrantes, extensión, warp, NoData, contraste, paleta, redibujado, paneo, lectura y márgenes del warp |
| `minigis --snapshot-window` sobre ráster + vectorial | Captura de la interfaz con la fila del ráster en el panel |
| `ctest --preset windows-msvc-release` | **13/13 pruebas superadas** |

Las imágenes de esa verificación están en `build\verify-*.png`.

---

## 12. Siguientes pasos sugeridos

1. **Hillshade y rampas de color** para los DEM (hoy se ven en grises con estiramiento).
2. **Consulta por atributos**: filtros tipo `QgsExpression` (o directamente `OGR_L_SetAttributeFilter`).
3. **Exportación**: guardar el mapa a PDF/SVG con `QPdfWriter` / `QSvgGenerator`.
4. **Teselas y GPU** para el régimen ligado a píxeles (ver RENDIMIENTO.md).
