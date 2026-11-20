# Tareas: apertura y visualización de KML/KMZ

Plan: `tasks/plan.md` · Spec: `SPEC-kml.md`

Estado: **T1–T9 hechas y verificadas (30/30 pruebas CTest)**. Primer incremento:
T1–T4. Segundo: T5–T9.

| Tarea | Estado | Evidencia |
|---|---|---|
| T1 fixtures | hecha | 8 archivos en `data/sample/kml/`; los 8 abren con exit 0 |
| T2 `--check-kml` | hecha | `mixto.kml` daba `storeFeatures 1` de 2 → rojo |
| T3 clasificación | hecha | `mixto.kml` da 2 de 2 y el `--snapshot` dibuja las 4 partes |
| T4 estilos | hecha | `estilos.kml` → `PEN(c:#FF0000FF,w:4.000000px)`; apertura del KML de 21,6 MB: 583 → ~600 ms |
| T5 tabla de estilos | hecha | `estilos_varios.kml` → 2 estilos con su índice por elemento |
| T6 render por estilo | hecha | captura del GUI: dos polígonos rojos y dos azules; el KML de 21,6 MB sigue en ~600 ms |
| T7 panel | hecha | fila «Polígonos · 2 estilos»; `--info` dice `Mixta`/`Poligono`, no `(Desconocido)` |
| T8 KMZ y errores | hecha | el diálogo ofrece `*.kml *.kmz` y un filtro «KML» propio |
| T9 documentación | hecha | `README.md`, `SPEC-kml.md` y estas tareas al día |


---

## Task 1: Fixtures KML/KMZ deterministas

**Description:** Crear el juego de archivos de ejemplo con el que se reproduce el
fallo actual y se prueban los casos límite. Todos propios, pequeños y legibles,
para poder versionarlos y usarlos en `ctest`. No se versiona el KML del usuario
(21,6 MB, datos de terceros): solo se usa para medir a mano.

**Acceptance criteria:**
- [ ] `data/sample/kml/mixto.kml` contiene una `MultiGeometry` (punto + línea +
  polígono) **y** otro polígono suelto: con el binario actual la colección no
  aparece en el mapa.
- [ ] `data/sample/kml/mixto.kmz` es el mismo contenido empaquetado (ZIP con un
  `.kml` dentro) y `--info` da las mismas capas y elementos.
- [ ] `carpetas.kml` tiene dos `<Folder>` con nombre; `solo_puntos.kml` tiene
  puntos con `<name>`; `sin_geometria.kml` tiene un placemark sin geometría.
- [ ] `estilos.kml` declara un `<Style>` con `LineStyle` rojo (`ff0000ff`) y
  `width` 4, y `PolyStyle`; `estilos_varios.kml` tiene tres elementos con un
  estilo y uno con otro.
- [ ] `mundo.kml` (polígonos con `Schema`/`SimpleField`) sirve de caso «real»
  pequeño y sin estilos.

**Verification:**
- [ ] `bin\minigis.exe --info <archivo>` abre los seis sin error (exit 0).
- [ ] `bin\minigis.exe --snapshot mixto.png -l data\sample\kml\mixto.kml` deja
  constancia visual del fallo (falta la colección).
- [ ] `--info` de `mixto.kmz` y de `mixto.kml` coinciden salvo en la ruta.

**Dependencies:** None

**Files likely touched:**
- `data/sample/kml/mixto.kml`, `.kmz`
- `data/sample/kml/carpetas.kml`, `solo_puntos.kml`, `sin_geometria.kml`
- `data/sample/kml/estilos.kml`, `estilos_varios.kml`, `mundo.kml`

**Estimated scope:** S (datos + ningún código)

---

## Task 2: `--check-kml`, el diagnóstico por elemento

**Description:** Añadir un modo sin ventana que recorra el archivo con la API C de
OGR y cuente, **por elemento**: tipo de geometría, partes no soportadas,
elementos sin geometría y estilos distintos. Además compara ese recuento con lo
que el almacén de geometrías construyó, y devuelve `ok: false` si no coinciden.
Es la prueba que hoy debe salir en rojo.

**Acceptance criteria:**
- [ ] Existe `--check-kml <archivo>` con `--json`, documentado en `--help`.
- [ ] La salida JSON sigue el contrato de `SPEC-kml.md` §R11 (`ok`, `driver`,
  `layers[]` con `features`, `geometry`, `kinds`, `withoutGeometry`,
  `unsupportedParts`, `distinctStyles`, `extent`, `crsEpsg`; y `appliedStyles`,
  `style`).
- [ ] Con `mixto.kml` y el binario actual devuelve `ok: false` e informa la
  diferencia entre elementos de OGR y elementos del almacén.
- [ ] Códigos de salida: `0` correcto, `1` uso, `2` archivo no encontrado, `3`
  GDAL no abre, `5` sin capas. Sin códigos nuevos.
- [ ] Funciona igual sobre `.kmz`.

**Verification:**
- [ ] `bin\minigis.exe --check-kml data\sample\kml\mixto.kml --json` imprime el
  JSON esperado y sale con 0 (el `ok: false` va dentro del JSON, no en el código
  de salida).
- [ ] `ctest -R kml-mixto` falla por la comparación de recuentos (rojo
  intencionado).
- [ ] `--check-kml` sobre un archivo inexistente sale con 2, y sobre un `.shp`
  corrupto con 3.

**Dependencies:** Task 1

**Files likely touched:**
- `src/core/minigiskmlinspect.h`, `src/core/minigiskmlinspect.cpp` (nuevos)
- `src/core/CMakeLists.txt`
- `src/app/main.cpp`
- `tests/CMakeLists.txt`

**Estimated scope:** M (4-5 archivos)

---

## Task 3: Clasificación de geometría por elemento

**Description:** Corregir la raíz del fallo: en capas cuyo tipo declarado es
desconocido, clasificar cada elemento por su geometría real, recorrer las
colecciones completas y no descartar ningún elemento. Resumir el tipo de la capa
(`Punto`, `Línea`, `Polígono`, `Mixta`) para que el panel y `--info` muestren algo
legible.

**Acceptance criteria:**
- [ ] Un elemento con `MultiGeometry` mixta se conserva y se dibujan todas sus
  partes, aunque conviva con otros elementos de la misma capa.
- [ ] Las partes de tipo no soportado se cuentan como `unsupportedParts`, no
  tiran el elemento entero.
- [ ] `VectorLayer::geometryKind()` y `geometryTypeName()` devuelven un tipo
  legible para capas KML; nunca «Unknown (any)» si hay geometría.
- [ ] En una capa de tipo conocido (shapefile, GeoPackage, GeoJSON homogéneo) el
  comportamiento no cambia: `--info --json` y `--snapshot` idénticos a `main`.

**Verification:**
- [ ] `ctest --preset windows-msvc-release --output-on-failure` en verde, con
  `kml-mixto` ya pasando.
- [ ] `bin\minigis.exe --snapshot mixto.png -l data\sample\kml\mixto.kml` muestra
  los dos polígonos, la línea y el punto.
- [ ] Comparación manual de `--info --json` y `--snapshot` de
  `data\sample\shapefiles\parcels.shp`, `data\sample\geopackage\ejemplo.gpkg` y un
  GeoJSON contra el binario anterior.

**Dependencies:** Task 2

**Files likely touched:**
- `src/core/minigisgeometrystore.cpp`
- `src/core/minigisvectorlayer.cpp`, `src/core/minigisvectorlayer.h`
- `src/core/minigiskmlinspect.cpp`

**Estimated scope:** M (3-4 archivos)

---

## Task 4: Resolver los estilos del KML con GDAL

**Description:** Activar `LIBKML_RESOLVE_STYLE=YES` en la inicialización de GDAL y
convertir el *style string* de OGR de cada elemento (`PEN(c:#rrggbb,w:Npx)`,
`BRUSH(fc:#rrggbb,fo:N)`) al `Style` de MiniGIS. Medir el coste con el archivo
real de 21,6 MB.

**Acceptance criteria:**
- [ ] Con `LIBKML_RESOLVE_STYLE=YES`, `OGR_F_GetStyleString` devuelve el estilo
  resuelto para un KML con `styleUrl` a un `<Style>` del documento.
- [ ] `parseOgrStyleString()` traduce color y anchura de trazo y color y
  opacidad de relleno, con anchura acotada a 20 px.
- [ ] Tolera variantes del formato (`w:4` sin `px`, claves en otro orden,
  `BRUSH(fc:none)`, ausencia de `PEN` o de `BRUSH`).
- [ ] La apertura del KML de 21,6 MB no supera 1,5× el tiempo actual (583 ms →
  ≤ 875 ms). Si lo supera, se cachea la resolución por `styleUrl` antes de dar la
  tarea por buena.

**Verification:**
- [ ] `bin\minigis.exe --check-kml data\sample\kml\estilos.kml --json` informa
  `appliedStyles: 1` y el color y la anchura esperados.
- [ ] Medición de tres tandas de `--info` del KML de 21,6 MB, anotada en el
  resumen de la tarea.
- [ ] `ctest -R kml-estilos` en verde.

**Dependencies:** Task 1

**Files likely touched:**
- `src/core/minigisstyle.cpp`, `src/core/minigisstyle.h`
- `src/core/minigisgdalinit.cpp`
- `src/core/minigiskmlinspect.cpp`
- `tests/CMakeLists.txt`

**Estimated scope:** M (4 archivos)

---

## Task 5: Tabla de estilos e índice por elemento en el almacén

**Description:** Guardar en el almacén de geometrías, por elemento, un índice a
una tabla de estilos únicos (color y grosor de trazo, color y opacidad de
relleno). Con más de 64 estilos distintos se repliega al dominante y se marca la
capa en modo dominante.

**Acceptance criteria:**
- [ ] `GeometryStore` expone la tabla de estilos y el índice de estilo por
  elemento, sin cambiar el formato de geometría que ya usa el renderizador.
- [ ] Con `estilos_varios.kml`: dos estilos en la tabla y el índice correcto en
  cada elemento.
- [ ] Con más de 64 estilos: tabla de un solo estilo (el dominante) y
  `styleMode: "dominant"` en el diagnóstico.
- [ ] Una capa sin estilos guarda un único estilo y no paga coste extra.

**Verification:**
- [ ] `bin\minigis.exe --check-kml data\sample\kml\estilos_varios.kml --json`
  informa `distinctStyles: 2`.
- [ ] `ctest -R kml-estilos` en verde.

**Dependencies:** Task 3, Task 4

**Files likely touched:**
- `src/core/minigisgeometrystore.h`, `src/core/minigisgeometrystore.cpp`
- `src/core/minigisvectorlayer.cpp`, `src/core/minigisvectorlayer.h`

**Estimated scope:** M (4 archivos)

---

## Task 6: Render agrupado por estilo y medición

**Description:** Dibujar por grupos de estilo, aplicando a cada grupo su `Style`,
sin perder el batching ni la caché por revisión. Medir contra la puerta de
rendimiento.

**Acceptance criteria:**
- [ ] `estilos_varios.kml` se dibuja con los dos colores, cada elemento con el
  suyo (`styleMode: "perFeature"`).
- [ ] Una capa sin estilos sigue haciendo una sola agrupación: mismo número de
  operaciones de dibujo que hoy.
- [ ] `ctest -R minigis.rendimiento` en verde.
- [ ] El `--snapshot` del KML de 21,6 MB no empeora más de un 20 %.
- [ ] Cambiar el estilo desde el diálogo sobrescribe el importado y sube
  `revision()`.

**Verification:**
- [ ] `bin\minigis.exe --snapshot varios.png -l data\sample\kml\estilos_varios.kml`
  con los dos colores comprobados a simple vista.
- [ ] `ctest -R "minigis.rendimiento"` y `ctest -R kml-` en verde.
- [ ] Medición de tres tandas anotada en el resumen de la tarea.

**Dependencies:** Task 5

**Files likely touched:**
- `src/core/minigisfastrenderer.cpp`
- `src/core/minigisgeometrystore.h`, `src/core/minigisgeometrystore.cpp`
- `src/core/minigisvectorlayer.cpp`

**Estimated scope:** M (4 archivos)

---

## Task 7: Panel de capas legible

**Description:** Sustituir el tipo de geometría heredado de OGR por el resumen
propio: `Punto`, `Línea`, `Polígono` o `Mixta`, y añadir el número de estilos
cuando sea mayor que uno.

**Acceptance criteria:**
- [ ] La columna **Geometría** muestra `Polígono` para `mundo.kml` y `Mixta` para
  `mixto.kml`.
- [ ] Ninguna capa con geometría muestra «Unknown (any)» ni «Desconocido».
- [ ] Una capa con varios estilos lo indica («2 estilos») sin romper el ancho de
  la columna.

**Verification:**
- [ ] `bin\minigis.exe --snapshot-window ventana.png -l data\sample\kml\mixto.kml`
  y revisión de la captura.
- [ ] `ctest` completo en verde.

**Dependencies:** Task 3, Task 5

**Files likely touched:**
- `src/gui/minigislayerpanel.cpp`
- `src/gui/minigismainwindow.cpp`

**Estimated scope:** S (2 archivos)

---

## Task 8: KMZ en el diálogo y errores con causa y acción

**Description:** Añadir `*.kmz` al filtro de *Archivo > Abrir capa* y al
arrastrar y soltar, y comprobar que un KML/KMZ que GDAL no puede abrir da un
`Error` con causa y acción en vez de un mensaje genérico.

**Acceptance criteria:**
- [ ] El filtro incluye `*.kml *.kmz` y el `.kmz` se abre desde el diálogo.
- [ ] Arrastrar `mixto.kmz` sobre el lienzo lo abre.
- [ ] Un `.kmz` que no es ZIP, o un `.kml` mal formado, muestran un mensaje con
  la causa de GDAL y qué hacer, y la CLI sale con 3.

**Verification:**
- [ ] `ctest -R kml-error` en verde.
- [ ] Prueba manual del diálogo y del arrastre con `mixto.kmz`.

**Dependencies:** Task 1

**Files likely touched:**
- `src/gui/minigismainwindow.cpp`

**Estimated scope:** S (1 archivo)

---

## Task 9: Documentación, medición final y cierre de la spec

**Description:** Actualizar `README.md` (tabla de la CLI con `--check-kml`,
formatos con `.kmz`, sección de comprobaciones), cerrar las preguntas de
`SPEC-kml.md` con lo decidido y registrar las mediciones finales.

**Acceptance criteria:**
- [ ] `README.md` documenta `--check-kml` y `.kmz` en la tabla de la CLI y en la
  lista de formatos.
- [ ] `SPEC-kml.md` pasa a estado **aprobada**, con Q1 y Q7 cerradas y el estado
  de partida actualizado con los tiempos finales.
- [ ] Los ocho criterios de éxito de §11 están verificados uno a uno y anotados.

**Verification:**
- [ ] `ctest --preset windows-msvc-release --output-on-failure` en verde,
  incluida `minigis.rendimiento`.
- [ ] Repaso de los ocho criterios con la salida de los comandos en la mano.

**Dependencies:** Tasks 3, 5, 6, 7

**Files likely touched:**
- `README.md`
- `SPEC-kml.md`
- `tasks/plan.md`

**Estimated scope:** S (3 archivos)
