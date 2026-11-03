# Task List: Proyectos QGIS (`.qgz`/`.qgs`)

> Derivado de [plan-proyecto.md](plan-proyecto.md) y
> [SPEC-proyecto.md](../SPEC-proyecto.md), aprobado el 2026-11-03 con estas
> decisiones: `Ctrl+Shift+O` para abrir proyecto (y `Ctrl+O` sigue abriendo
> capas), se guardan las capas añadidas y se quitan las cerradas, copia `.bak` en
> cada guardado, nombre de capa OGR y rutas del entorno actualizadas.
> Cada tarea se completa con: compila, **18/18** pruebas verdes y la verificación
> indicada.
> **Estado: T1–T5 implementadas y checkpoints A y B superados (2026-11-03)** —
> 23/23 pruebas. La copia guardada se reabre en QGIS 3.40.

## Comandos de referencia

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

Ejecutable: `build\windows-msvc-release\bin\minigis.exe`.

Contrato nuevo (se completa en T9): `--project-info [--json]`,
`--check-project <proyecto>` (27 comprobaciones de §9.2 del spec) y
`--save-project <salida> <proyecto>`; código de salida **7** para un proyecto que
no se puede abrir o guardar. Las 18 pruebas actuales no cambian.

Datos: `data/sample/projects/qgis_ejemplo.qgz` y `.qgs` (fixture versionado que
crea T1).

---

## Fase 0 — Entorno

### [x] T0 — Entorno: rutas reales de Qt, vcpkg y SDK

**Descripción:** `scripts\env.bat` y `CMakePresets.json` apuntaban a
`D:\QtWin\6.10.3` y `E:\vcpkg`, que no existen. Se actualizan a Qt 6.10.3
(`C:\Qtwin\6.10.3\msvc2022_64`), vcpkg (`D:\VCPKG`) y Windows SDK
(`D:\Windows Kits\10\bin\10.0.26100.0\x64`); se actualizan también las rutas del
`README.md`, el fallback de `cmake/MiniGisVcpkg.cmake` y la ruta de `ogr2ogr` de
`scripts/make_sample_data.bat`.

**Criterios de aceptación:**
- [x] `scripts\configure.bat windows-msvc-release` configura una carpeta limpia
      con Qt 6.10.3 y GDAL 3.12.4 de `D:\VCPKG`
- [x] Compila y `ctest` da **18/18**
- [x] `minigis --version` informa Qt 6.10.3, GDAL 3.12.4 y PROJ 9.9.0
- [x] `README.md` ya no menciona `D:\QtWin` ni `E:\vcpkg`

**Verificación:**
- [x] `cmake --preset windows-msvc-release` (build limpio) + build + `ctest`
- [x] `build\windows-msvc-release\bin\minigis.exe --version`

**Dependencias:** ninguna · **Archivos:** `scripts/env.bat`,
`CMakePresets.json`, `cmake/MiniGisVcpkg.cmake`, `README.md`,
`scripts/make_sample_data.bat` · **Tamaño:** S · **Hecho:** 2026-11-03

---

## Fase A — Lectura del proyecto

### [x] T1 — Fixture QGIS `.qgz`/`.qgs`

**Descripción:** escribir `tests/qgis/make_project_fixture.py` (PyQGIS 3.40) que
genere el fixture descrito en el spec §9.1: `cities` con etiquetas, `parcels`
con relleno naranja, `parcels_categorizado` (renderizador categorizado y oculta),
`orto` (multibanda 1/2/3 al 80 %), y `no_existe` (datasource roto, oculta). El
`.qgs` lleva `<mapcanvas>` en EPSG:25830 con extensión en metros, un nodo
desconocido `<minigisPrueba>` y `datasource` relativos. Se versionan el script y
los dos proyectos; el script reabre con QGIS y falla si no los lee.

**Criterios de aceptación:**
- [x] `qgis_ejemplo.qgz` y `qgis_ejemplo.qgs` existen y QGIS los reabre
- [x] Declaran 5 capas (4 abribles + `no_existe`) con las rutas relativas
      esperadas y el SRC/extensión del `<mapcanvas>`
- [x] El XML contiene `<minigisPrueba>` y los estilos/etiquetas del spec
- [x] `git status` solo muestra el script y los dos fixtures

**Verificación:**
- [x] `D:\OSGeo4w\bin\python-qgis-ltr.bat tests\qgis\make_project_fixture.py`
- [x] Reapertura con PyQGIS dentro del propio script (`QgsProject.read()`)

**Resultado:** fixture generado (`.qgs` 147 KB, `.qgz` 18 KB); QGIS 3.40 lo
reabre con las 4 capas, SRC EPSG:25830, escalas de etiquetas 1:5.000–1:250.000 y
la capa rota y el nodo desconocido en el XML.

**Dependencias:** T0 · **Archivos:** `tests/qgis/make_project_fixture.py`,
`data/sample/projects/qgis_ejemplo.qgz`, `data/sample/projects/qgis_ejemplo.qgs`
· **Tamaño:** S

---

### [x] T2 — XmlNode y ZipArchive

**Descripción:** añadir a `minigis_core` el árbol XML conservador (`XmlNode`:
leer con `QXmlStreamReader`, conservar atributos ordenados, hijos, texto y
comentarios; escribir con `QXmlStreamWriter` con el `DOCTYPE` de QGIS, sin
declaración XML) y el archivo ZIP (`ZipArchive`: listar y extraer entradas,
escribir un ZIP completo con `minizip`). Enlazar `MINIZIP::minizip` y declarar
`minizip` en `vcpkg/vcpkg.json`.

**Criterios de aceptación:**
- [x] Parsear y volver a serializar el `.qgs` del fixture conserva el nodo
      desconocido, los atributos y el texto de cada elemento
- [x] `ZipArchive` extrae la entrada `.qgs` de `qgis_ejemplo.qgz` y escribe un
      ZIP que `cmake -E tar tf` y QGIS pueden abrir
- [x] No se usa `Qt6::Xml`; las 18 pruebas siguen verdes
- [x] La API pública está documentada con Doxygen en español sin tildes

**Verificación:**
- [x] `scripts\build.bat windows-msvc-release` y `ctest` (18/18)
- [x] `minigisproyectoiotest` (`minigis.proyecto-io`): lee el fixture, serializa
      el XML, reescribe el ZIP y lo vuelve a leer; conserva `<minigisPrueba>` y
      las entradas ajenas
- [x] Revisión de que `ZipArchive` no deja el archivo abierto si falla

**Resultado:** `minigis.proyecto-io` en verde; el ZIP escrito conserva
`qgis_ejemplo.qgs` y el archivo de estilos de QGIS.

**Dependencias:** T1 · **Archivos:** `src/core/minigisxmltree.h/.cpp`,
`src/core/minigisziparchive.h/.cpp`, `src/core/CMakeLists.txt`,
`vcpkg/vcpkg.json` · **Tamaño:** M

---

### [x] T3 — Carga del proyecto y `--project-info`

**Descripción:** `QgisProject::load` (SRC del proyecto, `<mapcanvas>`
extensión/SRC, árbol invertido a fondo→frente, `projectlayers` vector/raster,
`datasource` con `|layername=`, capas no disponibles y `styleRepresentable`) y
`QgisSymbology` en el sentido de lectura (`Style`, `LabelStyle` en `simple` y
`basic`, `RasterStyle`). Añadir `--project-info [--json]`, el código 7 y el modo
`project-info` de `run_cli_test.cmake` con la prueba `minigis.proyecto-info`.

**Criterios de aceptación:**
- [x] `--project-info --json` informa SRC 25830, extensión, 3 capas
      vectoriales + 1 ráster en orden, visibilidad, `datasource` y `no_existe`
      como no disponible
- [x] Los valores de simbología, etiquetas y ráster del fixture son los de las
      comprobaciones 6–9 del spec
- [x] Un `.qgz` corrupto o un XML mal formado devuelve 7 con mensaje y acción
- [x] `minigis.proyecto-info` pasa y las 18 pruebas siguen verdes

**Verificación:**
- [x] `--project-info --json data\sample\projects\qgis_ejemplo.qgz`
- [x] `ctest --preset windows-msvc-release -R "proyecto|help" --output-on-failure`
- [x] `.qgs` sin comprimir y proyecto inexistente (código 2)

**Resultado:** 20/20 pruebas. El JSON informa 5 capas declaradas, 4 disponibles,
SRC y extensión EPSG:25830, `parcels` (relleno `#c85028`, contorno `#50140a`,
grosor 1,8898 px), `cities` (punto 9,4488 px, etiquetas `name`, halo 5,6693 px,
escala 66,1458/1,3229 mpp), `orto` (bandas 1/2/3, opacidad 80) y
`parcels_categorizado` con `styleRepresentable=false`. `no_existe` se lista como
no disponible con el motivo.

**Dependencias:** T2 · **Archivos:** `src/core/minigisqgisproject.h/.cpp`,
`src/core/minigisqgissymbology.h/.cpp`, `src/core/CMakeLists.txt`,
`src/app/main.cpp`, `tests/CMakeLists.txt`, `tests/run_cli_test.cmake` ·
**Tamaño:** L

---

### Checkpoint A — lectura

- [x] Compila y 20/20 pruebas verdes
- [x] `--project-info` describe el fixture completo y la capa rota
- [x] Revisión humana del JSON y de los valores de estilo

---

## Fase B — Abrir y guardar sin cambios

### [x] T4 — Abrir el proyecto en la ventana

**Descripción:** variante del cargador por capa concreta
(`startRequests(QVector<LayerRequest>)`, orden garantizado) y apertura del
proyecto en `MainWindow`: acciones `Abrir proyecto…` (`Ctrl+Shift+O`), filtro de
archivos, arrastrar y soltar, argumento posicional, SRC y extensión, visibilidad
y estilos por capa, aviso único con las capas no disponibles, título con el
nombre del proyecto y accesores de prueba. Carga vectorial en segundo plano y
rásters en el hilo de la GUI.

**Criterios de aceptación:**
- [x] Abrir `qgis_ejemplo.qgz` deja 4 capas en el orden del spec, 3 vectoriales y
      el ráster, con visibilidad, simbología, etiquetas y SRC 25830 aplicados
- [x] El lienzo encuadra la extensión del `<mapcanvas>` y `no_existe` se informa
      sin romper el mapa
- [x] Arrastrar el `.qgz` y `minigis proyecto.qgz` abren el proyecto; el filtro
      del diálogo lista proyectos
- [x] `--snapshot-window` con el proyecto escribe un PNG con las 4 filas del
      panel y el nombre del proyecto en el título
- [x] 23/23 verdes

**Verificación:**
- [x] `minigis --snapshot-window build\test-output\proyecto-ventana.png data\sample\projects\qgis_ejemplo.qgz`
- [x] Revisar el PNG: `orto`, `parcels`, `parcels_categorizado` (oculta) y
      `cities`, encuadre de Madrid y etiqueta «Madrid»
- [x] `ctest --preset windows-msvc-release` (`minigis.proyecto-ventana`)
- [x] Proyecto real `TCAR01.qgz` (18 capas, SRC local sin EPSG): se abre y
      encuadra en EPSG:4326 transformando la extensión

**Resultado:** los nombres del panel son los `<layername>` del proyecto; dos
capas sobre el mismo shapefile (parcels y parcels_categorizado) conviven sin que
el lienzo las deduplique. Se descubrió que QGIS guarda `scaleMin` como límite
alejado y `scaleMax` como cercano, y que los SRC con WKT propio necesitan
transformar el encuadre con GDAL; ambas cosas quedaron implementadas.

**Dependencias:** T3 · **Archivos:** `src/core/minigisvectorloader.h/.cpp`,
`src/gui/minigismainwindow.h/.cpp`, `src/app/main.cpp`, `tests/CMakeLists.txt`,
`tests/run_cli_test.cmake` · **Tamaño:** L

---

### [x] T5 — Guardar sin cambios (ZIP, `.bak`, escritura atómica)

**Descripción:** `QgisProject::save` para una foto idéntica al proyecto abierto:
serializar el XML, reescribir el `.qgz` conservando todas las entradas (la
entrada `.qgs` se nombra con el archivo de destino y el resto se copia tal cual),
guardar `.qgs` plano, escribir temporal + `.bak` + sustitución, y las acciones
`Guardar` (`Ctrl+S`) y `Guardar como…` (`Ctrl+Shift+S`) con conversión de
contenedor y reescritura de rutas relativas si cambia la carpeta. Añadir
`--save-project`.

**Criterios de aceptación:**
- [x] Guardar una copia sin cambios y reabrirla da el mismo SRC, extensión,
      capas, orden, visibilidad y estilos (comprobación 11)
- [x] Las entradas ajenas del ZIP y `<minigisPrueba>` siguen presentes (12, 13)
- [x] `.qgs` y `.qgz` se convierten entre sí (22) y «Guardar como» a otra carpeta
      reescribe las rutas relativas (23)
- [x] Tras guardar dos veces existe `.bak` con el contenido anterior (24)
- [x] 23/23 verdes

**Verificación:**
- [x] `--save-project build\test-output\copia.qgz data\sample\projects\qgis_ejemplo.qgz`
- [x] `--project-info` de la copia (4 disponibles con rutas reescritas)
- [x] `cmake -E tar tf build\test-output\copia.qgz` (entradas conservadas)
- [x] QGIS 3.40 reabre la copia con capas, SRC, estilos y etiquetas
- [x] `ctest --preset windows-msvc-release` (`proyecto-copia`, `-qgs` y `-io`)

**Resultado:** guardado en `.qgz` conserva `mwNACu_styles.db` y reescribe
`../../data/sample/...`; `.qgs` plano con las mismas capas; `.bak` con el
tamaño de la versión anterior. El guardado in-place y la copia del proyecto real
`TCAR01.qgz` también se probaron.

**Dependencias:** T4 · **Archivos:** `src/core/minigisqgisproject.h/.cpp`,
`src/core/minigisziparchive.cpp`, `src/gui/minigismainwindow.h/.cpp`,
`src/app/main.cpp`, `tests/run_cli_test.cmake` · **Tamaño:** L

---

### Checkpoint B — abrir y guardar

- [x] El proyecto se abre con todos los valores del fixture y su captura cuadra
- [x] La copia guardada sin cambios reabre igual en MiniGIS
- [x] `.qgz` guardado se abre en QGIS (prueba manual preliminar de T10)
- [x] Revisión humana antes de escribir cambios

---

## Fase C — Cambios y ciclo de vida

### [x] T6 — Guardar cambios de estado

**Descripción:** escribir en el XML solo lo que cambió: visibilidad
(árbol/leyenda), `<renderer-v2 type="singleSymbol">`, etiquetas
`<labeling type="simple">` (incluida la expresión `concat` para varios campos),
`<rasterrenderer>` con bandas/opacidad/contraste y `<mapcanvas><extent>`.
Comparar contra la foto cargada para no tocar capas sin cambios.

**Criterios de aceptación:**
- [x] Ocultar `parcels` y guardar deja el árbol coordinado (y la leyenda, si el
      proyecto la tiene con entradas; QGIS 3.40 la escribe vacía) (15)
- [x] Cambiar relleno/grosor de `parcels` reabre con los valores nuevos (16)
- [x] Etiquetas con dos campos se escriben como expresión y reabren igual (17)
- [x] Mover/zoom guarda la extensión sin tocar `projectCrs` (21)
- [x] Una capa con renderizador no soportado conserva su XML (14)
- [x] 24/24 verdes

**Verificación:**
- [x] `--check-project` (visibilidad, estilos, etiquetas, extensión)
- [x] QGIS 3.40 abre `build\windows-msvc-release\test-output\proyecto-modificado.qgz`
      con relleno `#0a141e`, etiquetas `concat("name", ' · ', "country")` y
      opacidad 0,5 en el ráster
- [x] `ctest --preset windows-msvc-release`

**Resultado:** el escritor reemplaza solo el renderizador, las etiquetas o el
`rasterrenderer` de las capas que cambian, y conserva la leyenda, el `DOCTYPE`,
el nodo desconocido y el resto del proyecto.

**Corrección (2026-11-03):** con un `<mapcanvas>` en un SRC local sin código
EPSG, el encuadre se escribía en grados y al reabrir se interpretaba como
metros, dejando el mapa en blanco hasta «Zoom a la capa». Ahora se transforma al
SRC real del `mapcanvas` y no se reescribe si la vista no cambió. Fixture
`data/sample/projects/qgis_local.qgz` y prueba de ida y vuelta añadidas.

**Dependencias:** T5 · **Archivos:** `src/core/minigisqgissymbology.h/.cpp`,
`src/core/minigisqgisproject.cpp`, `src/gui/minigismainwindow.cpp`,
`tests/run_cli_test.cmake` · **Tamaño:** L

---

### [x] T7 — Capas nuevas y cerradas

**Descripción:** persistir las capas abiertas en la sesión y quitar las que el
usuario cierre: `id` nuevo (`<nombre>_<uuid con guiones bajos>`), `datasource`
relativo, `srs` con WKT/`authid`, renderizador y etiquetas, inserción arriba en
`projectlayers`/árbol/`layerorder`/`legend`, y eliminación de los cuatro sitios.
Plantilla de `maplayer` copiada de una capa del mismo tipo del proyecto.

**Criterios de aceptación:**
- [x] Abrir `parcels_utm30.shp` en una sesión con el proyecto, guardar y reabrir
      muestra la capa nueva arriba, con id único y en árbol/`layerorder`/
      `projectlayers` (18)
- [x] Cerrar `parcels_categorizado` y guardar la elimina de los sitios
      correspondientes sin tocar las capas no disponibles (19, 20)
- [x] QGIS abre el proyecto con la capa nueva (verificado en la puerta)
- [x] Un segundo guardado no duplica la capa nueva
- [x] 24/24 verdes

**Verificación:**
- [x] `--check-project` (añadir/quitar y segundo guardado)
- [x] QGIS: `parcels_utm30` válida, `parcels_categorizado` ausente
- [x] `ctest --preset windows-msvc-release`

**Resultado:** las capas nuevas reciben `<nombre>_<uuid>` al guardar y, tras
releer el proyecto, el mapa capa→id se reconstruye para que el siguiente guardado
actualice en vez de duplicar.

**Dependencias:** T6 · **Archivos:** `src/core/minigisqgisproject.h/.cpp`,
`src/gui/minigismainwindow.h/.cpp`, `tests/run_cli_test.cmake` · **Tamaño:** M

---

### [x] T8 — Estado modificado, título y diálogos

**Descripción:** firma del estado (lista/orden/visibilidad/estilos/etiquetas/
ráster/SRC/extensión), `isProjectModified()`, `*` en el título, habilitación de
`Guardar`, diálogo Guardar/Descartar/Cancelar al cerrar o abrir otro proyecto,
proyectos en «Abrir recientes» y atajos definitivos.

**Criterios de aceptación:**
- [x] Recién abierto está limpio; cambiar visibilidad lo marca; al volver al
      estado original vuelve a estar limpio (25)
- [x] El título muestra el proyecto y `*` solo con cambios (26)
- [x] Cerrar con cambios pregunta y «Cancelar» no cierra ni guarda (código);
      falta la prueba manual interactiva
- [x] `--snapshot-window` no muestra diálogos ni modifica la configuración
- [x] 24/24 verdes

**Verificación:**
- [x] `--check-project`: limpio → modificado → guardar → limpio y título con `*`
- [ ] Prueba manual de guardar/descartar/cancelar (pendiente en la revisión final)
- [x] `ctest --preset windows-msvc-release`

**Dependencias:** T7 · **Archivos:** `src/gui/minigismainwindow.h/.cpp`,
`tests/run_cli_test.cmake` · **Tamaño:** M

---

### Checkpoint C — cambios y ciclo de vida

- [x] Visibilidad, simbología, etiquetas, ráster y extensión se guardan y reabren
- [x] Añadir/quitar capas deja el proyecto coherente y sin pérdidas
- [x] El título marca los cambios y guardar lo deja limpio
- [ ] Diálogo Guardar/Descartar/Cancelar: revisión manual pendiente
- [ ] Revisión humana antes del contrato definitivo

---

## Fase D — Contrato y compatibilidad

### [ ] T9 — Contrato CLI completo

**Descripción:** implementar `--check-project` con las 27 comprobaciones del
spec §9.2 (incluye guardar/reabrir, ZIP, XML desconocido, capas rotas, `.bak`,
firma, título y `--project-info`), `--save-project`, los códigos 2/7/1, la ayuda
y los modos de `run_cli_test.cmake`; registrar `minigis.proyecto`,
`minigis.proyecto-cli` y `minigis.proyecto-ventana`.

**Criterios de aceptación:**
- [ ] `--check-project` imprime 27 `OK` y sale con 0; cualquier `FALLO` sale 1
- [ ] `minigis.proyecto`, `minigis.proyecto-cli` y `minigis.proyecto-ventana`
      pasan; las 18 anteriores siguen verdes
- [ ] Proyecto inexistente → 2, ZIP/XML corrupto → 7, `--info` sobre `.qgz` → 1
- [ ] `--help` lista las opciones nuevas y el código 7

**Verificación:**
- [ ] `minigis --check-project data\sample\projects\qgis_ejemplo.qgz`
- [ ] `ctest --preset windows-msvc-release --output-on-failure`

**Dependencias:** T8 · **Archivos:** `src/app/main.cpp`,
`tests/CMakeLists.txt`, `tests/run_cli_test.cmake` · **Tamaño:** M

---

### [ ] T10 — Puerta de compatibilidad con QGIS

**Descripción:** `tests/qgis/verify_project.py` con PyQGIS 3.40 que abra el
`build/test-output/proyecto-modificado.qgz`, compruebe capas/orden/visibilidad,
SRC, extensión, relleno de `parcels`, etiqueta de `cities`, bandas/opacidad del
ráster y la capa rota; CTest opcional con `MINIGIS_QGIS_PYTHON`.

**Criterios de aceptación:**
- [ ] El script imprime `OK` para el proyecto modificado y devuelve 0
- [ ] Falla con 1 si el proyecto no se puede leer o no coincide
- [ ] `minigis.proyecto-qgis` se registra solo si se configura la variable
- [ ] El resultado queda anotado para el cierre

**Verificación:**
- [ ] `D:\OSGeo4w\bin\python-qgis-ltr.bat tests\qgis\verify_project.py build\test-output\proyecto-modificado.qgz`
- [ ] `ctest --preset windows-msvc-release -R proyecto-qgis` con la variable

**Dependencias:** T9 · **Archivos:** `tests/qgis/verify_project.py`,
`tests/CMakeLists.txt` · **Tamaño:** S

---

### Checkpoint D — contrato

- [ ] 27/27 comprobaciones en `OK` y pruebas nuevas verdes
- [ ] Puerta de QGIS en verde sobre el proyecto modificado
- [ ] Códigos de salida y ayuda documentados
- [ ] Revisión humana antes del cierre

---

## Fase E — Cierre

### [ ] T11 — README, spec y verificación final

**Descripción:** documentar en `README.md` las opciones nuevas, el código 7, el
comportamiento de abrir/guardar, la tabla de fidelidad del mapeo, los límites
(ráster debajo del vectorial, nombre OGR, resampling/NoData no persistidos), las
pruebas y la puerta de QGIS; pasar `SPEC-proyecto.md` a «implementado» con los
resultados reales y cerrar esta lista.

**Criterios de aceptación:**
- [ ] README §5, §6, §7, §8, §10 y §11 actualizados y sin rutas antiguas
- [ ] `SPEC-proyecto.md` en «implementado» con la verificación (27/27, QGIS, 18/18)
- [ ] SC1–SC14 comprobados uno a uno
- [ ] `git status` solo muestra los archivos previstos

**Verificación:**
- [ ] Bucle completo (configurar, compilar, `ctest`)
- [ ] Ejecutar los ejemplos nuevos del README
- [ ] `git status --short`

**Dependencias:** T10 · **Archivos:** `README.md`, `SPEC-proyecto.md`,
`tasks/todo-proyecto.md` · **Tamaño:** S

---

### Checkpoint E — cierre

- [ ] SC1–SC14 verificados y anotados
- [ ] README y spec al día
- [ ] Revisión humana final: el trabajo del spec está terminado
