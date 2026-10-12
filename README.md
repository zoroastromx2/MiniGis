# MiniGIS

Visor SIG mínimo en **C++ con Qt 6 y GDAL**: carga un *shapefile* (o GeoJSON, GeoPackage,
KML…), lo dibuja, permite hacer zoom y paneo, consultar la tabla de atributos y cambiar la
simbología básica.

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
| Qt (oficial) | `D:\QtWin\6.10.3\msvc2022_64` | 6.10.3 |
| Qt Creator | `D:\QtWin\Tools\QtCreator\bin\qtcreator.exe` | 20.0.2 |
| Compilador | `C:\Program Files\Microsoft Visual Studio\2022\Community` | MSVC 19.44 (x64) |
| CMake | `C:\Develop\4.4.2\bin\cmake.exe` | 4.4.4 |
| Ninja | `D:\QtWin\Tools\Ninja\ninja.exe` | — |
| vcpkg | `E:\vcpkg` | GDAL 3.12.4 (`x64-windows`), release **y** debug |
| Datos de ejemplo | `data\sample\shapefiles` (generados con `ogr2ogr`) | — |

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
│   │   ├── minigisvectorlayer.*# capa vectorial sobre GDAL/OGR
│   │   ├── minigismaprenderer.*# render a QImage + reproyección al vuelo
│   │   ├── minigisrectangle.*  # extensión/zoom
│   │   ├── minigisstyle.*      # simbología
│   │   └── minigiserror.h      # error = mensaje + acción sugerida
│   ├── gui/                    # widgets (equivalente a qgis_gui)
│   │   ├── minigismapcanvas.*  # lienzo: zoom, paneo, arrastrar y soltar
│   │   ├── minigislayerpanel.* # árbol de capas
│   │   ├── minigisattributetablemodel.*
│   │   ├── minigisstyledialog.*
│   │   └── minigismainwindow.*
│   └── app/main.cpp            # GUI + modos de línea de comandos
├── tests/                      # pruebas de humo con CTest
├── data/sample/                # GeoJSON origen + shapefiles generados
└── scripts/                    # env / configure / build / run / deploy / datos
```

---

## 4. Compilar

### 4.1 Con los scripts (lo más rápido)

```bat
scripts\make_sample_data.bat              :: genera los shapefiles de ejemplo
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
cmake -S . -B build\manual -G Ninja ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DVCPKG_INSTALLED_DIR=E:/vcpkg/installed ^
  -DQt6_DIR=D:/QtWin/6.10.3/msvc2022_64/lib/cmake/Qt6 ^
  -DCMAKE_PREFIX_PATH=D:/QtWin/6.10.3/msvc2022_64
cmake --build build\manual
```

> **Nota (importante) sobre vcpkg.** Por defecto se usa el **árbol ya compilado**
> (`E:\vcpkg\installed`, `MINIGIS_USE_VCPKG_MANIFEST=OFF`). Activar el modo manifiesto
> (`-DMINIGIS_USE_VCPKG_MANIFEST=ON`) haría que vcpkg **recompilase GDAL y su cadena** en un
> árbol propio del proyecto; además, si se apuntase al árbol compartido, vcpkg **eliminaría**
> paquetes ajenos al manifiesto (por ejemplo `qtbase` o `qgis-o2`). Por eso el modo manifiesto
> fuerza un árbol `build/<preset>/vcpkg_installed` y avisa por consola.

---

## 5. Depurar en Qt Creator

1. **Abre el proyecto**: `File > Open File or Project…` → `E:\AAA\Proyecto nuevo\MiniGIS\CMakeLists.txt`.
2. **Selecciona el preset**: en `Projects > Build & Run > CMake`, elige
   `windows-msvc-debug` (Qt Creator 20 los muestra en la lista de *presets*). Si prefieres
   configurar a mano, usa:
   * *Generator*: `Ninja`
   * *CMake tool*: `D:\QtWin\Tools\CMake_64\bin\cmake.exe` (o `C:\Develop\4.4.2\bin\cmake.exe`)
   * *Kit*: `Desktop Qt 6.10.3 MSVC2022 64bit`
   * Variables de CMake: las del apartado 4.2.
3. **Kit**: `Desktop Qt 6.10.3 MSVC2022 64bit`. El compilador debe ser el de
   `C:\Program Files\Microsoft Visual Studio\2022\Community` (MSVC 19.44), que es el que
   detecta vcpkg.
4. **Entorno de ejecución**: el proyecto **no necesita que toques `PATH`**. Tras compilar,
   `MINIGIS_COPY_RUNTIME_DLLS=ON` copia junto al ejecutable Qt, GDAL y sus dependencias, y
   `MINIGIS_DEPLOY_QT_RUNTIME=ON` ejecuta `windeployqt` para desplegar los *plugins* de Qt
   (plataformas, estilos, formatos de imagen) más el plugin `offscreen` que usan los modos sin
   ventana. Si aun así quieres tenerlo a mano al depurar, añade en `Projects > Run >
   Environment`:
   ```
   D:\QtWin\6.10.3\msvc2022_64\bin;E:\vcpkg\installed\x64-windows\bin
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
| `archivos...` / `-l, --layer <archivo>` | Capas a cargar (se pueden repetir) |
| `-i, --info` | Resumen de las capas (nombre, driver, geometría, elementos, extensión, SRC, campos) |
| `--json` | Salida JSON (junto a `--info`), pensada para scripts |
| `--list-drivers` | Drivers vectoriales disponibles en GDAL |
| `--snapshot <archivo.png>` | Renderiza el mapa a PNG sin abrir ventana |
| `--snapshot-window <archivo.png>` | Captura la interfaz completa a PNG |
| `--extent <xmin,ymin,xmax,ymax>` | Extensión inicial del mapa |
| `--size <AnchoxAlto>` | Tamaño de la imagen (por defecto `1200x800`) |
| `--crs <EPSG:xxxx>` | SRC de destino del dibujo (por defecto `EPSG:4326`) |
| `--max-features <n>` | Límite de elementos dibujados por capa (`0` = sin límite) |
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
minigis --info --layer data\sample\shapefiles\cities.shp | findstr /i "SRC"
```

En la interfaz:

* `Ctrl+O` abrir capa · `Ctrl+W` cerrar capa · `Ctrl+Q` salir
* `Ctrl+ +` / `Ctrl+ -` zoom · `Ctrl+Shift+F` extensión completa · `Ctrl+Shift+L` zoom a la capa
* rueda del ratón = zoom anclado al cursor · arrastrar con el botón izquierdo = paneo
* `F6` tabla de atributos · `Ctrl+Shift+Y` simbología
* arrastrar un `.shp` desde el Explorador y soltarlo sobre el mapa

---

## 7. Qué hace y qué no hace

**Hace**

* Abre cualquier formato vectorial soportado por GDAL/OGR (probado con shapefile).
* Dibuja puntos, líneas y polígonos **con huecos**; reproyecta al vuelo si el SRC de la capa
  difiere del de destino (por ejemplo, un shapefile UTM sobre un mapa en EPSG:4326), tanto las
  geometrías como la extensión con la que se encuadra la capa.
* Tabla de atributos paginada y bajo demanda (máximo 100 000 filas mostradas).
* Simbología: color de relleno/contorno, grosor, tamaño de punto y opacidad.
* Localiza automáticamente `proj.db` y los datos de GDAL si no hay variables de entorno.
* Errores accionables: cada fallo dice qué ha pasado y qué hacer (`minigis::Error`).

**No hace (todavía)**

* Ráster (GeoTIFF, etc.): GDAL ya está enlazado, así que añadir `GDALRasterBand::RasterIO`
  es el siguiente paso natural.
* Edición de geometrías, guardado, simbología por categorías, etiquetado, servidores OGC.
* Reproyección del *render* con rejilla (*warp*): se transforman las geometrías, no las teselas.

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

### 8.1 Rendimiento

El despliegue vectorial está optimizado y **medido**: índice espacial en memoria, simplificación
por nivel de zoom, dibujado por lotes, modo borrador durante los gestos, reutilización del cuadro
anterior y carga en segundo plano con barra de progreso. El análisis completo, las medidas y el
trabajo pendiente están en **[RENDIMIENTO.md](RENDIMIENTO.md)**.

```bat
:: medir el paneo/zoom sin abrir ventana (40 cuadros, con desglose por fase)
bin\minigis.exe --bench 40 --bench-scenario mixed -l capa.gpkg --size 1200x800 -V

:: comparar con el motor clásico de lectura directa
bin\minigis.exe --bench 40 --renderer legacy -l capa.gpkg --size 1200x800

:: puerta de regresión: falla con código 6 si el p90 supera el presupuesto
bin\minigis.exe --bench 24 --bench-budget 150 -l capa.gpkg --json
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
| `No se encuentra Qt` o Qt equivocado (vcpkg) | Fija el kit: `-DQt6_DIR=D:/QtWin/6.10.3/msvc2022_64/lib/cmake/Qt6`. MiniGIS fija además las herramientas (`Qt6CoreTools`…) al mismo Qt para que vcpkg no mezcle su `qtbase` |
| Al configurar con el kit aparece `Qt6: 6.11.1 (E:/vcpkg/...)` o `Unknown CMake command "_qt_internal_should_include_targets"` | El toolchain de vcpkg antepone su árbol a `CMAKE_PREFIX_PATH`, y **Qt Creator inyecta un *provider* de `find_package` que busca justo ahí**, así que encontraba el `qtbase` de vcpkg y reescribía `Qt6_DIR`. MiniGIS lo evita: detecta el Qt del kit, descarta las variables `Qt6*_DIR` cacheadas que apunten a vcpkg y **excluye ese árbol de `CMAKE_PREFIX_PATH` y `CMAKE_FIND_ROOT_PATH` durante la búsqueda de Qt**. Basta con volver a configurar (Qt Creator → *Run CMake*); si ves el aviso `"debug_and_release" set to "OFF"` al cambiar de Qt, borra `build/<kit>` para partir de cero |
| `Target ... "gdal.dll" is not a directory` al copiar DLLs | Ocurría con CMake 3.30 (el del kit): la opción `-t` de `copy_if_different` no existe ahí. Ya se usa la forma `origenes... destino`, compatible con todas las versiones |
| `Could not find the Qt platform plugin "windows"` (o `"offscreen"`) al ejecutar | Los plugins de plataforma se copian **siempre** junto al ejecutable (antes dependían de `windeployqt`, que no existe si Qt sale de vcpkg). Si aun así falta, comprueba `build\<kit>\bin\platforms\` y recompila |
| Error `Mezcla de versiones de Qt: Qt6 6.x con Qt6Core 6.y` | `CMAKE_PREFIX_PATH` está apuntando a dos instalaciones de Qt: deja solo el Qt oficial |
| `ASSERT: "theme" in qstylehints.cpp` o `Could not find the Qt platform plugin` | Hay un Qt de vcpkg copiado junto al ejecutable. MiniGIS **excluye a propósito** las DLLs `Qt6*.dll` de vcpkg al copiar dependencias: borra `build\<preset>\bin\Qt6*.dll` y recompila |
| `gdal.dll` / `Qt6Core.dll` no encontradas al ejecutar | Ejecuta desde Qt Creator o con `scripts\run.bat`; para una carpeta autónoma usa `scripts\deploy.bat` |
| `could not find or load the Qt platform plugin "offscreen"` | `MINIGIS_DEPLOY_QT_RUNTIME` está en `OFF` y las pruebas necesitan ese plugin: actívalo, ejecuta `scripts\deploy.bat`, o define `QT_QPA_PLATFORM=windows` |
| Aviso `No se encontro proj.db` | Define `PROJ_DATA=E:\vcpkg\installed\x64-windows\share\proj` (o copia esa carpeta junto al `.exe`) |
| `La configuracion Debug necesita la variante Debug de GDAL` | Usa `windows-msvc-reldbg`, o instala la variante debug: `E:\vcpkg\vcpkg.exe install gdal --triplet x64-windows` |
| Aviso de que no se pudo detectar el prefijo de `/showIncludes` | Es MSVC en español sin paquete de idioma inglés. Compila igual, pero conviene instalar el idioma inglés en Visual Studio: así CMake registra las cabeceras y las recompilaciones incrementales son fiables |
| CMake no encuentra GDAL | `E:\vcpkg\vcpkg.exe install gdal --triplet x64-windows` y reconfigura |
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
  `E:\vcpkg\installed` ya tiene GDAL compilado (release y debug). El modo manifiesto
  recompilaría la cadena completa y, apuntando al árbol compartido, **eliminaría** paquetes
  ajenos al manifiesto (`qtbase`, `qgis-o2`…). El manifiesto se conserva en `vcpkg/vcpkg.json`
  para documentar la dependencia y fijar el `baseline`.

---

## 11. Verificación realizada

Todo lo siguiente se ejecutó sobre este equipo con el preset `windows-msvc-debug`:

| Comprobación | Resultado |
|---|---|
| `scripts\configure.bat windows-msvc-debug` | Qt 6.10.3, GDAL 3.12.4, vcpkg `x64-windows` (modo clásico) |
| `scripts\build.bat windows-msvc-debug` | Compila sin errores; 87 DLLs de vcpkg + Qt oficial + plugins desplegados |
| `minigis --version` | GDAL 3.12.4, PROJ 9.8.1, `PROJ_DATA` y `GDAL_DATA` localizados |
| `minigis --info --json -l cities.shp` | 12 elementos, `ESRI Shapefile`, `Point`, `EPSG:4326 - WGS 84`, 5 campos con tipo y ancho |
| `minigis --info -l parcels_utm30.shp` | 5 polígonos, `EPSG:25830 - ETRS89 / UTM zone 30N` |
| `minigis --snapshot` sobre `cities.shp` | PNG con los 12 puntos en su posición geográfica correcta (península + Canarias) |
| `minigis --snapshot` sobre `parcels.shp` | PNG con los 5 polígonos y el **hueco** del polígono `P-005` |
| `minigis --snapshot --crs EPSG:4326 -l parcels_utm30.shp` | Reproyección UTM→geográfica correcta (encuadre y geometrías) |
| `minigis --snapshot-window` | Captura de la interfaz: menús, barra, panel de capas, lienzo y barra de estado |
| `ctest --preset windows-msvc-debug` | **5/5 pruebas superadas** |
| Configuración con el **kit de Qt Creator** (CMake 3.30 del kit, sin presets, partiendo de una caché que apuntaba al Qt de vcpkg) | Qt 6.10.3 oficial detectado, las variables `Qt6*_DIR` de vcpkg descartadas, compila y el binario lee el shapefile y captura la interfaz a 900x600 |
| `scripts\deploy.bat` + ejecución con `PATH=C:\Windows\system32` | La carpeta `dist\<preset>` funciona sin Qt ni vcpkg instalados en `PATH` |

Las imágenes de esa verificación están en `build\windows-msvc-debug\*.png`.

---

## 12. Siguientes pasos sugeridos

1. **Ráster**: una `RasterLayer` sobre `GDALRasterBand` + estiramiento de contraste, dibujada
   bajo las capas vectoriales.
2. **Consulta por atributos**: filtros tipo `QgsExpression` (o directamente `OGR_L_SetAttributeFilter`).
3. **Etiquetado**: dibujar un campo de la tabla sobre las geometrías.
4. **Exportación**: guardar el mapa a PDF/SVG con `QPdfWriter` / `QSvgGenerator`.
