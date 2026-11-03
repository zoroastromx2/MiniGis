# Spec: Proyectos QGIS (`.qgz`/`.qgs`): abrir y guardar los cambios

> Módulo: `proyecto` · Estado: **aprobado** (2026-11-03) · Fecha: 2026-11-03
>
> Depende de: la capa vectorial y raster ya existentes (`VectorLayer`,
> `RasterLayer`), su simbología y etiquetas. No hay módulos previos que cambien.
>
> El requisito es **una sola capacidad** (persistencia de proyecto: leer, aplicar y
> reescribir), así que no se propone mapa de capacidades; el trabajo se desglosa
> después en `tasks/plan-proyecto.md` y `tasks/todo-proyecto.md`.
>
> Aprobado con estas decisiones (detalle en §14): `Ctrl+Shift+O` para abrir
> proyecto y `Ctrl+O` sigue abriendo capas; se guardan las capas añadidas y se
> quitan las cerradas; copia `.bak` en cada guardado; nombre de capa OGR; y
> `scripts\env.bat`/presets actualizados a las rutas reales del equipo
> (Qt 6.10.3 en `C:\Qtwin`, vcpkg en `D:\VCPKG`, SDK en `D:\Windows Kits`).

---

## 1. Objetivo

Hoy MiniGIS abre archivos (shapefile, GeoPackage, GeoTIFF…) y permite cambiar
simbología, etiquetas, visibilidad y encuadre, pero **todo eso vive solo en
memoria**: al cerrar la aplicación se pierde, y no hay forma de abrir el archivo
con el que el usuario organiza su trabajo en QGIS (un `.qgz`).

Este cambio añade el concepto de **proyecto**:

* abrir un proyecto QGIS (`.qgz` comprimido o `.qgs` XML) y reconstruir en MiniGIS
  lo que muestra: capas, orden de apilado, visibilidad, SRC, encuadre, simbología,
  etiquetas y estilo del ráster;
* **guardar** en el mismo archivo (o en otro) los cambios que el usuario haga,
  sin destruir lo que MiniGIS no entiende: renderizadores complejos, capas de
  otros proveedores, maquetas, relaciones, propiedades desconocidas y cualquier
  otro archivo que viaje dentro del `.qgz`;
* que el proyecto guardado se pueda **volver a abrir en QGIS** con los cambios
  aplicados y el resto intacto.

**Usuario:** quien prepara y revisa proyectos en QGIS y quiere un visor ligero
para ajustar simbología, etiquetas y visibilidad (y comprobar el resultado) sin
abrir QGIS, guardando sobre el mismo `.qgz` que luego sigue usando en QGIS.

**Criterio de éxito del producto:** abrir un `.qgz` real de QGIS, cambiar la
visibilidad de una capa y la simbología de otra, guardar con `Ctrl+S`, y que
QGIS 3.40 abra ese mismo archivo mostrando los cambios, las capas que MiniGIS no
pudo abrir y las partes del proyecto que MiniGIS no interpreta.

---

## 2. Estado de partida (verificado leyendo el código y el equipo)

| Comprobación | Resultado |
|---|---|
| ¿Existe algún concepto de proyecto? | **No.** `main.cpp` solo trabaja con capas; `MainWindow` no tiene ruta de proyecto, ni estado «modificado», ni guardar |
| ¿Se guarda algo de la sesión? | Solo la geometría de la ventana, el estado de los docks y la lista de recientes en `QSettings`. Ni simbología ni capas |
| Simbología, etiquetas, visibilidad | Viven en `VectorLayer` (`Style`, `LabelStyle`, `mVisible`) y `RasterLayer` (`RasterStyle`, `mVisible`); cambiarlas sube `revision()` y redibuja, pero no se serializan en ningún sitio |
| Orden de apilado | `MapCanvas::layers()` es **de fondo a frente** (se dibujan en orden); el panel muestra primero los rásters y luego las vectoriales en ese mismo orden. QGIS guarda el árbol **de arriba abajo** (primera capa = la de encima) |
| SRC y encuadre | `MapCanvas::destinationEpsg()` (por defecto 4326) y `extent()`; el SRC no tiene interfaz (solo `--crs`), el encuadre cambia con el zoom y el paneo |
| `.qgz` hoy | `detectLayerKind()` lo manda a GDAL, que no lo abre: error «GDAL no reconoce el archivo» |
| Carga | `VectorLayerLoader::start(QStringList)` expande **todos** los archivos a todas sus capas; no se puede pedir «esta capa concreta de este GeoPackage» (necesario para `|layername=`) ni conservar el orden del proyecto entre capas |
| Diálogo de archivos | `kOpenFilter` solo lista capas; no incluye proyectos ni distingue `.qgs`/`.qgz` |
| QGIS en el equipo | **QGIS 3.40.15 LTR** en `D:\OSGeo4w`; PyQGIS funciona (`python-qgis-ltr.bat`), lo que permite una puerta de compatibilidad real |
| ZIP | `minizip` está instalado en vcpkg (`zip.h`, `unzip.h`, `minizip.dll`) como dependencia de GDAL; exporta `MINIZIP::minizip`. Qt trae `QZipWriter` **privado** (no se usa) |
| XML | No hay lector/escritor XML en el proyecto; Qt proporciona `QXmlStreamReader`/`QXmlStreamWriter` en `QtCore`, sin dependencia nueva |
| Datos de ejemplo | No hay ningún `.qgz`/`.qgs` en `data/sample`; habrá que crear un fixture (ver §9.1) |
| Línea base de pruebas | **18/18** en verde con Qt 6.10.3 y GDAL 3.12.4 tras la configuración limpia del 2026-11-03 (`build\windows-msvc-release`) |
| Entorno documentado | `README.md`/`AGENTS.md` decían Qt 6.10.3 en `D:\QtWin` y vcpkg en `E:\vcpkg`; **esas rutas ya no existen**. Se han actualizado a Qt 6.10.3 en `C:\Qtwin\6.10.3\msvc2022_64`, vcpkg en `D:\VCPKG` y SDK en `D:\Windows Kits` (ver §14.8) |

---

## 3. Qué significa «abrir un proyecto» en MiniGIS

MiniGIS implementa un **subconjunto** del formato de QGIS y conserva el resto.
La regla de oro: *solo se reescribe el XML que MiniGIS sabe representar y que el
usuario ha cambiado; todo lo demás se conserva tal cual (mismos elementos,
atributos, orden y valores).*

### 3.1 Elementos del proyecto que se interpretan

| Elemento QGIS | Uso en MiniGIS |
|---|---|
| `<projectCrs>/<spatialrefsys>/<authid>` (o `<srid>`) | SRC del proyecto; candidato a SRC del lienzo |
| `<mapcanvas><destinationsrs>` | SRC del lienzo (tiene prioridad sobre `projectCrs`) |
| `<mapcanvas><extent>` | Encuadre inicial (coordenadas en el SRC de `destinationsrs`) |
| `<ProjectViewSettings><DefaultViewExtent>` | Respaldo del encuadre cuando no hay `<mapcanvas>` (proyectos guardados sin interfaz) |
| `<layer-tree-group>` (y grupos anidados) | Orden de apilado y visibilidad (`<layer-tree-layer checked="Qt::Checked">`) |
| `<layerorder><layer id="…"/>` | Orden de dibujo; se conserva y se actualiza al añadir/quitar capas |
| `<legend><legendlayer checked=…><filegroup><legendlayerfile visible=…>` | Se mantiene coherente con la visibilidad y con las capas añadidas/quitadas |
| `<projectlayers><maplayer>` de `type="vector"` con `provider="ogr"` | Capas vectoriales |
| `<projectlayers><maplayer>` de `type="raster"` con `provider="gdal"` | Capas ráster |
| `<maplayer><datasource>` | Ruta y capa: `ruta` o `ruta|layername=nombre` (relativa al `.qgs`/`.qgz`) |
| `<maplayer><id>`, `<layername>`, `<srs>` | Identidad y SRC de la capa |
| `<renderer-v2 type="singleSymbol">` con `SimpleFill`/`SimpleLine`/`SimpleMarker` | Simbología vectorial de MiniGIS |
| `<labeling type="simple">` y `<labeling type="basic">` | Etiquetas (se leen ambos formatos; se escribe siempre `simple`, que QGIS 3.40 escribe y vuelve a leer) |
| `<pipe><rasterrenderer type="multibandcolor"|"singlebandgray">` | Bandas, opacidad y contraste del ráster |
| `<customproperties>` del `maplayer` | Ajustes de MiniGIS sin equivalente en QGIS (§5.3) |

### 3.2 Elementos que **se conservan sin interpretar**

Todo lo demás: `<Layouts>`, `<Annotations>`, `<relations>`, `<snapping-settings>`,
`<projectMetadata>`, `<properties>`, `<dataDefinedServerProperties>`, capas de
proveedores no soportados, `<main-annotation-layer>`, renderizadores
`categorizedSymbol`/`graduatedSymbol`/`rule`/`mesh`/`pointcloud`/`vectorTile`,
efectos, expresiones de datos, `<pipe>` con etapas desconocidas, comentarios y
atributos desconocidos. No se borran, no se reordenan y no se reformatean salvo
el reformateo global del documento (§4.2).

### 3.3 Límites conocidos del mapeo (explícitos)

* Ráster y vectorial se dibujan como hoy: **siempre el ráster debajo del
  vectorial**. Un proyecto que intercale ráster y vectorial se abre con el orden
  de capas vectoriales correcto y el ráster al fondo; al guardar **no se toca**
  el orden existente, así que QGIS lo sigue viendo como lo dejó su autor.
* El **nombre visible** de una capa de proyecto es su `<layername>` (lo que el
  usuario ve en QGIS); las capas abiertas sueltas siguen mostrando el nombre de
  la capa de datos. El nombre OGR se conserva para abrir los datos.
* Si el proyecto usa un SRC **sin código EPSG** (WKT propio, frecuente en
  proyectos locales), su XML se conserva y MiniGIS dibuja en `EPSG:4326`
  transformando el encuadre con GDAL/PROJ; las capas se reproyectan igual que en
  QGIS si GDAL reconoce su SRC. No se reescribe el SRC del proyecto.
* No se soportan capas de PostGIS/WMS/WFS/memoria/delimitadas: se informan como
  no disponibles y su XML se conserva.
* `RasterStyle::resampling` y `RasterStyle::useNodata` no tienen equivalente
  estable en el proyecto: **no se guardan** (solo viven en la sesión). El resto
  de `RasterStyle` sí (§5.3).

---

## 4. Requisitos funcionales

### R1 — Abrir un proyecto

* Acciones nuevas: **Archivo → Abrir proyecto…** (`.qgz` y `.qgs`) y también
  arrastrar y soltar el archivo sobre el mapa o pasarlo como argumento
  posicional (`minigis proyecto.qgz`). El diálogo de archivos incluye los
  filtros `Proyectos de QGIS (*.qgz *.qgs)`.
* Al abrir se **reemplaza** el contenido del mapa (con confirmación si había
  cambios sin guardar, R7). Se cargan las capas en el orden del proyecto y se
  aplican visibilidad, simbología, etiquetas y estilo ráster.
* SRC del lienzo, por prioridad: `--crs` explícito de la CLI > `<mapcanvas>
  <destinationsrs>` > `<projectCrs>` > `EPSG:4326`. El encuadre es el del
  proyecto **cuando el SRC del lienzo es el de `<mapcanvas>`**; si `--crs` lo
  fuerza a otro, se encuadra la unión de las capas (no se reproyecta el
  rectángulo del proyecto en esta versión).
* Las capas vectoriales se abren **en segundo plano** con el cargador actual, en
  el orden del proyecto y con progreso; los rásters se abren en el hilo de la
  interfaz, como hoy. La ventana no se congela con un shapefile pesado.
* Si una capa no se puede abrir (archivo movido, proveedor no soportado, capa
  sin SRC), **no se abre**, se avisa una sola vez con la lista de capas y el
  motivo, y su XML se conserva para el guardado. Un proyecto con capas rotas se
  abre igualmente (con lo que se pueda).
* Si no se puede abrir el proyecto en sí (no es un ZIP válido, XML mal formado,
  no es un proyecto QGIS), se muestra `Error` con causa y acción, no se toca el
  mapa y el código de salida es **7** en la CLI.
* La barra de estado resume: «Proyecto «x.qgz» abierto: N capas (M no
  disponibles)». El título incluye el nombre del proyecto (R7).

### R2 — Modelo en memoria y conservación del XML

* Se lee el `.qgs` a un árbol XML propio (`XmlNode`) con `QXmlStreamReader`
  (solo `QtCore`): nombre, atributos ordenados, hijos, texto y comentarios.
  No se usa `QDom` (evita `Qt6::Xml`) ni se guarda el texto original completo.
* Se construye una lista de capas del proyecto con: `id`, `nombre visible`,
  `datasource`, `provider`, `type`, `visible`, ruta resuelta, nombre de capa OGR,
  y los estilos ya interpretados. Una capa cuyo renderizador no está soportado
  se marca `styleRepresentable = false`: se abre con el estilo por defecto para
  esa geometría y **su nodo de simbología no se reescribe** mientras el usuario
  no edite el estilo de esa capa (si lo edita, se sustituye a sabiendas, §5.1).
* Cada capa cargada en el lienzo recuerda el `id` de su entrada de proyecto
  (mapa capa→id); así el guardado no depende de emparejar por nombre ni de que
  todas las capas se hayan abierto. Las capas nuevas no tienen `id` y reciben uno
  al guardar. La pareja `(ruta, capa OGR)` solo se usa como respaldo cuando una
  capa se abre por una vía que no pasó por el proyecto.
* La serialización conserva el `<!DOCTYPE qgis PUBLIC 'http://mrcc.com/qgis.dtd'
  'SYSTEM'>`, escribe UTF-8 sin declaración XML (como QGIS) y no toca
  `version`, `saveUser` ni `saveUserFull`; `saveDateTime` tampoco se actualiza
  (QGIS lo reescribe y no lo interpreta al leer). Las etiquetas vacías pueden
  salir como `<x/>` en vez de `<x></x>`: es equivalente en XML.
* Las capas del proyecto se ordenan **de fondo a frente** (inverso al árbol de
  QGIS). Los grupos anidados se aplanan conservando el orden; el grupo se
  conserva en el XML.
* El orden entre capas existentes **no se regenera**: al guardar, cada capa
  conserva su posición; las nuevas se insertan **arriba** (principio del árbol y
  de `<layerorder>`), que es donde MiniGIS las coloca; las cerradas se eliminan
  de los cuatro sitios (árbol, `<layerorder>`, `<legend>`, `<projectlayers>`).
* Las capas que no se pudieron abrir se guardan en una lista aparte y **nunca se
  eliminan** del XML (su archivo puede reaparecer; borrarlas destruiría el
  proyecto).

### R3 — Guardar

* **Archivo → Guardar** (`Ctrl+S`): si el proyecto se abrió de un `.qgz`,
  reescribe el ZIP completo; si es un `.qgs`, reescribe el XML.
* **Archivo → Guardar como…** (`Ctrl+Shift+S`): permite `.qgz` o `.qgs`; convierte
  el contenedor (crear/quitar el ZIP). Si la carpeta de destino cambia, los
  `datasource` se reescriben para seguir apuntando al mismo archivo: ruta
  relativa al nuevo proyecto cuando está en la misma unidad, absoluta si no.
* Escritura **atómica**: primero se escribe un temporal en la misma carpeta y
  solo si termina bien se sustituye el original. Antes de la sustitución se
  guarda la versión anterior en `<archivo>.bak` (una copia por guardado, se
  sobrescribe; solo al sobrescribir un archivo existente, no al crear uno nuevo
  con «Guardar como»). Si algo falla, el proyecto original queda intacto, el
  estado sigue «modificado» y se muestra `Error` con la causa y la acción.
* `.qgz`: se copian **todas** las entradas del ZIP original (archivos de estilos,
  adjuntos, etc.) byte a byte y solo se sustituye la entrada `.qgs`. La entrada se
  nombra con el archivo de destino (`<nombre>.qgs`), que es lo que hace QGIS al
  guardar como; el resto se copia tal cual.
* No se guarda si no hay proyecto abierto (acción deshabilitada con explicación
  en la barra de estado). No se crea un proyecto desde cero en esta versión
  (§14.6).
* Al terminar: estado limpio, `.bak` disponible, título sin `*`, y un mensaje en
  la barra de estado con el número de capas y el tiempo.

### R4 — Cambios que se guardan (y cómo se representan)

| Cambio en MiniGIS | Dónde se escribe | Fidelidad |
|---|---|---|
| SRC del lienzo | no se modifica: no hay interfaz para cambiarlo en v1, así que `<projectCrs>` y `<mapcanvas><destinationsrs>` se conservan | exacta (sin cambios) |
| Encuadre del lienzo | `<mapcanvas><extent>` (y `<DefaultViewExtent>` si existe), **solo si el SRC del lienzo coincide con el del `<mapcanvas>`**; con `--crs` distinto se deja el encuadre como estaba | exacta (coordenadas) |
| Orden de apilado | posición de cada `<maplayer>`/`<layer-tree-layer>`/`<layerorder>`; nuevas arriba | exacta |
| Visibilidad | `layer-tree-layer@checked` y, si existe leyenda con entradas, `legendlayer@checked` y `legendlayerfile@visible` | exacta |
| Simbología vectorial | `<renderer-v2 type="singleSymbol">` (§5.1) | aproximada (mm↔px) |
| Etiquetas | `<labeling type="simple">` (§5.2) | aproximada (escala, halo, desplazamiento) |
| Estilo ráster | `<rasterrenderer …>` + contraste por bandas (§5.3) | aproximada (contraste único de MiniGIS) |
| Capas añadidas | `<maplayer>` nuevo + árbol + `<layerorder>` + `<legend>` | verificada en QGIS (§9.4) |
| Capas cerradas | se eliminan de los cuatro sitios | exacta |
| Capas no disponibles o de proveedor no soportado | no se tocan | exacta (byte a byte) |
| Renderizadores no soportados | solo se actualiza la visibilidad (árbol y leyenda); el nodo de simbología no se toca | exacta en simbología |

Nunca se modifican geometrías, atributos ni los archivos de datos: las capas
siguen siendo de **solo lectura** (se mantiene la regla del proyecto: GDAL solo
por la API C de OGR y sin escritura).

### R5 — Conversiones y detalles del formato

**5.1 Simbología vectorial.** Se escribe un `<symbol>` de una sola capa
(`SimpleFill`, `SimpleLine` o `SimpleMarker`, según la geometría) a partir de
`Style`:

* `fillColor` → `color` (`"r,g,b,a"`); `strokeColor` → `outline_color`;
  `strokeWidth` (px) → `outline_width` con `outline_width_unit="MM"` y factor
  `25.4/96`; `pointSize` (px) → `size` con `size_unit="MM"`; `opacity` (0–100) →
  `symbol@alpha` (0–1). `fillEnabled=false` → `style="no"`; al leer,
  `style="no"` → `fillEnabled=false`.
* Al leer se aceptan los valores ya usados por QGIS (mm y px) y se combina la
  opacidad con el alfa del color: `opacity = alpha_symbol × alpha_color / 255`.
  Al escribir, el color se emite opaco (alfa 255) y la opacidad va en
  `symbol@alpha`, de modo que el resultado visual en QGIS es el mismo.
* Solo se interpreta `renderer-v2 type="singleSymbol"` con **un** `<symbol>` y
  **una** capa de símbolo cuyo `class` corresponda a la geometría. Cualquier otro
  caso (varias capas, símbolos de marcador SVG, líneas compuestas) deja
  `styleRepresentable=false`. Si el usuario **edita** el estilo de esa capa, se
  reemplaza el renderizador entero por uno simple (decisión del usuario,
  documentada en el mensaje al guardar si había pérdida).

**5.2 Etiquetas.** Se leen `type="simple"` (`<settings>`) y `type="basic"`
(`<styles><style><settings>`; solo si hay **un** estilo habilitado). Se escribe
siempre `type="simple"`:

| `LabelStyle` | XML | Nota |
|---|---|---|
| `enabled` | presencia de `<labeling>` y `maplayer@labelsEnabled` | |
| `fields` (1) | `text-style@fieldName` + `@isExpression="0"` | |
| `fields` (N>1) | `text-style@fieldName = concat("f1", ' · ', "f2", …)` + `@isExpression="1"` | comillas dobladas y comillas simples escapadas |
| `separator` | dentro de la expresión `concat` | |
| `fontFamily` | `@fontFamily` | |
| `fontSize` (pantalla) | `@fontSize` + `@fontSizeUnit="Point"` | |
| `fontSize` (unidades de mapa) | `@fontSizeUnit="MapUnit"` | |
| `bold`/`italic` | `@fontWeight` 75/50, `@fontItalic` 1/0 | |
| `color` | `@textColor="r,g,b,a"` | |
| `haloEnabled`/`haloWidth`/`haloColor` | `<text-buffer bufferDraw bufferSize bufferSizeUnits="MM" bufferColor>` | `haloWidth` está en píxeles: se convierte con `×25.4/96` al escribir y la inversa al leer |
| `minimumScale`/`maximumScale` | `<rendering scaleVisibility="1" scaleMin scaleMax>` | se convierten a denominador de escala (abajo); en QGIS `scaleMin` es el límite alejado y `scaleMax` el cercano |
| `placement` | `<placement placement="0|1|2">` (`AroundPoint`, `OverPoint`, `Line`) | |
| `offsetX`/`offsetY` | `<placement xOffset yOffset offsetUnits="Point" quadOffset="4">` | signo de Y invertido; aproximado |
| `maximumLabels` | `<rendering maxNumLabels>` | |
| `minimumFeatureSize` (px) | `<rendering minFeatureSize>` (mm) | aproximado |
| `priority` | `<placement priority="10 − min(priority,10)">` | QGIS: mayor = más prioridad |
| `minimumDistance`, `discardDuplicates` | `<customproperties>` `MiniGIS/labelMinimumDistance` y `MiniGIS/labelDiscardDuplicates` | QGIS los ignora y los conserva |

Conversión de escala: `denominador = mpp × metrosPorUnidad × 96 / 0.0254`, con
`metrosPorUnidad = 1` en SRC proyectado y `111319.49079327358` en geográfico;
`minimumScale` (alejado) se escribe en `scaleMin` y `maximumScale` (cercano) en
`scaleMax` (los nombres de QGIS están invertidos respecto a la intuición: su
`scaleMin` es el límite más alejado). La vuelta usa la misma fórmula; tolerancia
de la prueba: 1 %.

**5.3 Ráster.**

| `RasterStyle` | XML |
|---|---|
| `BandMode::Rgb` | `rasterrenderer type="multibandcolor"` con `redBand`/`greenBand`/`blueBand` |
| `BandMode::Grayscale` | `rasterrenderer type="singlebandgray"` con `grayBand` |
| `alphaBand` (0 = ninguna) | `alphaBand` (`-1` si no hay) |
| `opacity` | `rasterrenderer@opacity` (0–1) |
| `Stretch::None` | sin `<redContrastEnhancement>`; `minMaxOrigin/limits = None` |
| `Stretch::MinMax` | `<redContrastEnhancement>`… con `minValue`/`maxValue` y `algorithm=StretchToMinimumMaximum` |
| `Stretch::PercentClip` | `algorithm=ClipToMinimumMaximum`, `minValue`/`maxValue` = p2/p98 de la muestra y `minMaxOrigin` con `cumulativeCutLower/Upper` = 0.02/0.98 |

Al leer, si las tres bandas tienen rangos distintos se usa el rango común (mínimo
de los mínimos, máximo de los máximos) con `MinMax`; el estilo se marca como
aproximado en la descripción de la capa. `resampling` y `useNodata` no se
persisten (§3.3).

Un `rasterrenderer` de otro tipo (`paletted`, `singlebandpseudocolor`,
`hillshade`, `contour`…) deja la capa con `styleRepresentable=false`: se dibuja
con las bandas 1/2/3 (o la 1 en gris) y **su XML no se reescribe** salvo que el
usuario abra el diálogo de estilo y lo cambie; en ese caso se sustituye por un
`multibandcolor`/`singlebandgray` simple y se avisa de la pérdida de la paleta o
del hillshade.

**5.4 Capas nuevas.** Se genera un `<maplayer>` completo a partir de la capa
abierta: `id` nuevo (`<nombre>_<uuid con guiones bajos>`, único), `datasource`
(relativo al proyecto si es posible, con `/`), `layername`, `provider`, `srs`
(WKT de `crsWkt()` + `authid`), `renderer-v2` simple, `labeling` si procede,
`customproperties` y `<blendMode>0`. Para no inventar atributos se parte de la
plantilla de un `maplayer` del mismo tipo ya presente en el proyecto y, si no la
hay, de una plantilla constante en el código. La validez se comprueba abriendo
el proyecto guardado en QGIS (§9.4) y en MiniGIS.

### R6 — Resolución de `datasource`

* Formatos aceptados: `ruta`, `ruta|layername=nombre`; se ignoran claves
  adicionales (`|geometrytype=`, `|uniqueid=`) y se toleran `/` y `\`.
* Rutas relativas al directorio del `.qgs`/`.qgz` (`.` y `..` incluidos), `file://`
  se reduce a ruta local, y las absolutas se respetan.
* El nombre de capa se resuelve con `VectorLayer::layerNames()` (comparación
  exacta; si no aparece, se informa como no disponible con la lista de capas del
  archivo en el `hint`). Sin `|layername=` se usa la primera capa.
* El cargador gana una variante que acepta peticiones `(archivo, índice,
  nombre visible)` para cargar **una capa concreta** de un contenedor y conservar
  el orden del proyecto; `start(QStringList)` sigue comportándose igual.

### R7 — Interfaz, estado «modificado» y cierre

* Menú **Archivo**: `Abrir capa…` (`Ctrl+O`, sin cambios), `Abrir proyecto…`
  (`Ctrl+Shift+O`), `Abrir recientes` (incluye proyectos y capas),
  `Guardar` (`Ctrl+S`), `Guardar como…` (`Ctrl+Shift+S`), `Cerrar capa(s)`.
  `Guardar` está deshabilitado si no hay proyecto o no hay cambios.
* Título: `MiniGIS <versión> — <archivo del proyecto><*> — <capa actual> —
  <N> capa(s)`. El `*` aparece solo si hay cambios (convención de QGIS).
* **Modificado** se calcula comparando una firma del estado actual con la del
  último estado abierto/guardado: lista de capas con su orden, visibilidad,
  simbología, etiquetas, estilo ráster y encuadre. La firma no depende de
  señales: si el usuario deshace sus cambios a mano, el proyecto vuelve a
  considerarse limpio. Se evalúa al actualizar el título y antes de guardar.
* Al cerrar la ventana, abrir otro proyecto o salir con cambios sin guardar:
  diálogo **Guardar / Descartar / Cancelar** (Cancelar aborta la operación). En
  los modos sin ventana no hay diálogos: `--snapshot-window` y `--check-project`
  trabajan con el proyecto tal como se abre.
* El estado «modificado», las capas no disponibles y el resultado del último
  guardado se exponen en `MainWindow` para el contrato sin ventana
  (`projectPath()`, `isProjectModified()`, `unavailableProjectLayers()`).

### R8 — Modos sin ventana (contrato de pruebas)

* `--project-info [--json] <proyecto>`: resume el proyecto sin abrir ventana
  (SRC, extensión, capas con id, tipo, visibilidad, `datasource`, estilo y
  etiquetas resumidos, capas no disponibles). JSON con claves estables.
* `--check-project <proyecto>`: ejecuta el contrato de §9.2 (lectura, cambios,
  guardado, reapertura, conservación de lo desconocido, capas rotas, `.qgs`,
  `.bak`, atomicidad) e imprime líneas `OK`/`FALLO` como `--check-layers`. Deja
  los artefactos en `build/test-output/` (`proyecto-modificado.qgz`)
  para la puerta de QGIS.
* `--save-project <salida> <proyecto>`: abre el proyecto y lo guarda sin cambios
  (ida y vuelta del escritor); sirve para generar el artefacto de la puerta de
  QGIS y para comprobar `.qgz`↔`.qgs`.
* Códigos de salida: se mantienen 0–6 y se añade **7 = no se pudo abrir o
  guardar el proyecto** (se documenta en `README.md` y se prueba). Archivo
  inexistente sigue siendo **2**; uso incorrecto, **1**.
* `--info`/`--snapshot` sobre un `.qgz` no lo abren como capa: error de uso (1)
  con la sugerencia de usar `--project-info`/`--snapshot-window` o abrirlo en la
  interfaz.

### R9 — Rendimiento y seguridad de los datos

* Abrir un proyecto no lee geometrías ni atributos de más: solo XML/ZIP y la
  apertura de cada capa que ya hace el cargador. Presupuesto blando para un
  proyecto de hasta 1 MB y 50 capas: **< 250 ms** de XML+ZIP (sin contar la
  apertura e indexación de capas).
* Guardar no vuelve a leer datos: recorre el árbol XML y escribe el ZIP. Los
  archivos que no son el `.qgs` se copian sin descomprimir dos veces en memoria
  más de lo necesario.
* Nunca se escribe en las fuentes de datos (vectoriales o ráster).
* Toda escritura es atómica y deja copia `.bak`; un fallo a mitad no puede dejar
  un `.qgz` truncado ni pérdida del original.
* Un ZIP corrupto, cifrado o con entradas duplicadas produce un error accionable,
  no un fallo silencioso ni una pérdida.

### R10 — No regresión

* Las **18 pruebas actuales** siguen pasando; los modos existentes
  (`--info`, `--snapshot`, `--check-*`, `--bench`) no cambian su contrato.
* Abrir capas y arrastrar archivos funciona igual; el filtro de archivos sigue
  listando las capas de siempre y añade los proyectos.
* Listas de fuentes explícitas en los `CMakeLists.txt`; sin `GLOB`.
* Sin dependencias nuevas de Qt; la única dependencia añadida es `minizip`
  (ya instalada como dependencia de GDAL; se declara en `vcpkg/vcpkg.json`).

### R11 — Documentación

* `README.md`: sección 6 (opciones nuevas y código 7), sección 7 («Hace» / «No
  hace»), sección 8 (pruebas nuevas y puerta de QGIS), sección 10 (decisiones de
  proyecto), sección 11 (verificación) y los atajos de la sección 6.
* Tabla de fidelidad del mapeo (qué se guarda exacto, aproximado y qué no) en
  `README.md`, enlazada desde este spec.
* Este spec pasa a «implementado» al cerrar la entrega, con la verificación
  anotada en §11.

### R12 — Fuera de alcance (explícito)

* Editar geometrías o atributos y guardarlos en las fuentes (sigue siendo un
  visor).
* Simbología completa de QGIS: categorizada, graduada, basada en reglas,
  expresiones de datos, símbolos SVG, efectos, mezclas, etiquetas curvas o con
  reglas, máscaras, callouts.
* Maquetas (`Layouts`), atlas, informes, animaciones, 3D, relaciones, uniones,
  filtros, formularios, metadatos del proyecto.
* Crear un proyecto nuevo desde cero o una plantilla; varios proyectos a la vez.
* Proveedores remotos (PostGIS, WMS/WFS, XYZ, bases de datos), capas temporal o
  delimitada.
* Selección de elementos, expresión de filtro o tema de capas guardado en el
  proyecto.
* Interpretar o modificar los archivos auxiliares del `.qgz` (solo se copian).

---

## 5. Decisiones (y por qué)

1. **Conservar el XML desconocido, no regenerarlo.** Un `.qgs` real contiene
   cientos de elementos que MiniGIS no entiende. Reescribir solo lo que se toca
   es la única forma de abrir un proyecto sin destruirlo. La alternativa
   (serializar un modelo propio) perdería maquetas, relaciones y estilos.
2. **Solo se reescribe la simbología de las capas que cambian.** Así un
   renderizador categorizado, un SVG o una etiqueta con expresiones sobreviven a
   un guardado en el que el usuario solo ocultó una capa. Si el usuario edita el
   estilo de esa capa, se sustituye a sabiendas (es lo que pidió) y se avisa.
3. **`minizip` y no un ZIP propio ni `QZipWriter`.** `QZipReader/Writer` son API
   privada de Qt; escribir un ZIP a mano (CRC, deflate, directorio central) es
   código delicado sin necesidad. `minizip` ya está en el árbol de vcpkg por
   GDAL, es C estable y permite copiar entradas desconocidas y controlar el
   nombre de la entrada `.qgs`.
4. **Árbol XML propio con `QXmlStreamReader`.** `QDom` obligaría a añadir
   `Qt6::Xml`; el lector en streaming permite conservar atributos, orden,
   comentarios y texto, y escribir con `QXmlStreamWriter`.
5. **Invertir el orden del árbol.** QGIS lista de arriba abajo (la primera capa
   es la de encima) y `MapCanvas` dibuja de fondo a frente. No invertir
   cambiaría el apilado de cualquier proyecto con más de una capa.
6. **No reordenar lo existente al guardar.** El panel de MiniGIS no permite
   reordenar, así que reescribir el orden desde el lienzo solo puede introducir
   diferencias (sobre todo con rásters intercalados). Las capas nuevas van arriba,
   que es donde el usuario las ve aparecer; las existentes conservan su sitio.
7. **Las capas no disponibles no se borran.** Un archivo movido no puede
   destruir la referencia del proyecto. Igual criterio para proveedores no
   soportados: se avisa y se conserva.
8. **Guardar capas añadidas y quitar las cerradas.** «Guardar» debe dejar el
   archivo coherente con lo que el usuario ve; si no, la siguiente apertura le
   devuelve un mapa distinto del que guardó. El coste es generar `maplayer`
   nuevos, que se valida con la puerta de QGIS.
9. **Firma de estado en vez de banderas de «sucio».** Marcar sucio en cada
   señal es fácil de olvidar (el estilo se cambia con un método sin señal) y no
   detecta cuando el usuario vuelve al estado guardado. La firma se calcula al
   vuelo, cuesta O(capas) y hace la prueba determinista.
10. **Escritura atómica + `.bak`.** Reescribir un ZIP es una operación con
    muchos puntos de fallo; el temporal evita dejar un proyecto truncado y el
    `.bak` da una salida si el resultado no convence.
11. **`type="simple"` al escribir etiquetas.** Es el formato que QGIS 3.40
    escribe y vuelve a leer; el formato `basic` de 3.44 solo se lee. Escribir
    `basic` obligaría a reproducir estilos múltiples y expresiones que MiniGIS no
    modela. La compatibilidad de lo escrito se valida con la puerta de QGIS 3.40.
12. **`.qgs` además de `.qgz`.** Cuesta muy poco (mismo XML, sin ZIP) y permite
    inspeccionar el resultado con cualquier editor, lo que hace las pruebas y la
    depuración mucho más baratas.
13. **Puerta de QGIS con PyQGIS.** Que MiniGIS reabra su propio archivo no
    demuestra compatibilidad. Un script que abre el proyecto guardado con QGIS
    3.40 y comprueba capas, visibilidad, SRC, extensión y estilo es la única
    prueba real de que el formato es válido.

---

## 6. Estructura del proyecto

```
src/core/
  minigisxmltree.*          árbol XML mínimo (leer, buscar, modificar, escribir)
  minigisziparchive.*       lectura/escritura de ZIP con minizip (entradas completas)
  minigisqgissymbology.*    Style, LabelStyle y RasterStyle <-> XML y conversiones
  minigisqgisproject.*      proyecto: cargar, capas, datasource, snapshot, guardar
  minigisvectorloader.*     variante de carga por capa concreta (archivo, índice)
  CMakeLists.txt            fuentes nuevas y enlace a MINIZIP::minizip
src/gui/
  minigismainwindow.*       acciones, abrir/guardar, título, firma, diálogos, accesores
  minigismapcanvas.*        (sin cambios; se usa lo existente)
src/app/
  main.cpp                  .qgz/.qgs posicional, --project-info, --check-project,
                            --save-project, código 7, ayuda
tests/
  CMakeLists.txt            pruebas nuevas (info, contrato, CLI, ventana, QGIS opcional)
  run_cli_test.cmake        modos project-info / check-project / save-project / error
  qgis/make_project_fixture.py   genera el fixture con PyQGIS (documentado)
  qgis/verify_project.py         puerta de compatibilidad con QGIS
data/sample/projects/
  qgis_ejemplo.qgz          fixture (capas del sample, estilos, etiquetas, capa rota)
  qgis_ejemplo.qgs          el mismo proyecto sin comprimir
README.md                   opciones, límites, pruebas y verificación
vcpkg/vcpkg.json            dependencia explícita minizip (ya instalada por GDAL)
```

Regla de dependencias: el proyecto se lee y escribe en `minigis_core` (sin
widgets); la interfaz solo aplica el resultado al lienzo y construye la foto del
estado actual.

---

## 7. Comandos

```bat
:: compilar (el entorno MSVC debe estar cargado; ver §14.8)
cmake --build build\windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure

:: resumen del proyecto sin ventana
build\windows-msvc-release\bin\minigis.exe --project-info --json ^
        data\sample\projects\qgis_ejemplo.qgz

:: contrato completo (deja build\test-output\proyecto-modificado.qgz)
build\windows-msvc-release\bin\minigis.exe --check-project ^
        data\sample\projects\qgis_ejemplo.qgz

:: ida y vuelta del escritor, y conversion .qgz -> .qgs
build\windows-msvc-release\bin\minigis.exe --save-project build\test-output\copia.qgz ^
        data\sample\projects\qgis_ejemplo.qgz
build\windows-msvc-release\bin\minigis.exe --save-project build\test-output\copia.qgs ^
        data\sample\projects\qgis_ejemplo.qgz

:: abrir en la interfaz y capturarla (comprueba menu, titulo y panel)
build\windows-msvc-release\bin\minigis.exe data\sample\projects\qgis_ejemplo.qgz
build\windows-msvc-release\bin\minigis.exe --snapshot-window proyecto.png ^
        data\sample\projects\qgis_ejemplo.qgz
```

Puerta de compatibilidad con QGIS (este equipo):

```bat
D:\OSGeo4w\bin\python-qgis-ltr.bat tests\qgis\verify_project.py ^
        build\test-output\proyecto-modificado.qgz
```

Regenerar el fixture (solo si cambian los datos de ejemplo):

```bat
D:\OSGeo4w\bin\python-qgis-ltr.bat tests\qgis\make_project_fixture.py
```

---

## 8. Estilo de código e interfaz propuesta

Se mantiene el estilo del proyecto: C++17, `namespace minigis`, miembros con
prefijo `m`, comentarios Doxygen en español sin tildes, `#pragma once`, listas de
fuentes explícitas y errores por `minigis::Error` (mensaje + acción). Nada de
excepciones para fallos reportables.

```cpp
/** Arbol XML minimo: conserva lo que MiniGIS no interpreta. */
class XmlNode
{
public:
  static bool parse(const QByteArray &xml, XmlNode *root, Error *error);
  QByteArray toXml() const;

  XmlNode *child(const QString &name);
  QVector<XmlNode> children;
  QVector<QPair<QString, QString>> attributes; // orden original
  QString name;
  QString text;
};

/** ZIP completo en memoria: entradas copiadas tal cual y .qgs sustituido. */
class ZipArchive
{
public:
  static bool read(const QString &path, QVector<ZipEntry> *entries, Error *error);
  static bool write(const QString &path, const QVector<ZipEntry> &entries, Error *error);
};

/** Capa del proyecto, ya resuelta a una capa de datos. */
struct ProjectLayer
{
  QString id;          // vacio = capa nueva de esta sesion
  QString name;        // nombre visible en QGIS
  QString datasource;  // tal como esta en el XML (relativo)
  QString provider;    // ogr | gdal | otro
  QString sourcePath;  // ruta absoluta resuelta
  QString layerName;   // capa OGR pedida (puede estar vacia)
  enum class Type { Vector, Raster, Unsupported } type = Type::Unsupported;
  bool visible = true;
  bool styleRepresentable = true; // false: no tocar su XML de simbologia
  Style style;
  LabelStyle labels;
  RasterStyle raster;
};

/** Foto de lo que hay en el mapa, de fondo a frente. */
struct ProjectSnapshot
{
  int crsEpsg = 4326;
  Rectangle extent;
  QVector<ProjectLayer> layers;
};

class QgisProject
{
public:
  static bool load(const QString &path, std::unique_ptr<QgisProject> *project, Error *error);

  const QVector<ProjectLayer> &layers() const;      // fondo -> frente
  const QStringList &unavailableLayers() const;     // no abribles, se conservan
  int crsEpsg() const;
  bool hasExtent() const;
  Rectangle extent() const;

  /** Aplica la foto y escribe; conserva todo lo no interpretado. */
  bool save(const QString &targetPath, const ProjectSnapshot &snapshot, Error *error);

  /** Firma estable del estado guardable, para saber si hay cambios. */
  static QByteArray signature(const ProjectSnapshot &snapshot);

  static bool isProjectPath(const QString &path); // .qgz / .qgs
};
```

Convenios concretos:

* `XmlNode` no interpreta nombres: `child("maplayer")` devuelve el primero y
  `children` conserva el orden; no hay búsquedas con XPath ni expresiones.
* `QgisProject::save` nunca reordena nodos existentes; inserta/elimina los
  nodos de las capas nuevas/cerradas y actualiza solo los valores conocidos.
* Los conversores de simbología viven en `minigisqgissymbology.*` y no tocan el
  árbol: reciben/devuelven un nodo y los objetos de estilo.
* Todas las funciones públicas llevan comentario Doxygen que explica el por qué
  (por ejemplo, por qué se conserva un renderizador desconocido).

---

## 9. Estrategia de pruebas

### 9.1 Fixture

`data/sample/projects/qgis_ejemplo.qgz` se genera con PyQGIS 3.40 a partir de los
datos de ejemplo y se versiona (es pequeño, ~50 KB):

| Capa (arriba → abajo) | Tipo | Estilo / estado |
|---|---|---|
| `cities` | vector | marcador azul, etiquetas activas con el campo `name`, Noto Sans 10 pt, halo 1,5 mm, colocación `OverPoint`, escala 1:5.000–1:250.000 |
| `parcels_categorizado` | vector | renderizador categorizado por `uso`, **oculta** (prueba de renderizador no soportado y de visibilidad) |
| `parcels` | vector | relleno naranja (200,80,40), contorno (80,20,10) de 0,5 mm, visible |
| `orto` | ráster | `multibandcolor` 1/2/3, opacidad 0,8, sin estiramiento |
| `no_existe` | vector | `datasource` a un archivo inexistente, oculta (se inyecta en el XML) |

El `.qgs` incluye además un `<mapcanvas>` con SRC **EPSG:25830** y extensión en
metros alrededor de Madrid (para probar la aplicación del SRC y del encuadre y
la reproyección al vuelo), y un nodo desconocido `<minigisPrueba>` bajo la raíz
para comprobar que se conserva. El script deja también `qgis_ejemplo.qgs`
(sin comprimir) con el mismo contenido y `qgis_local.qgz`/`.qgs`, una copia con
el SRC del lienzo y del proyecto como WKT local sin código EPSG, para la prueba
de ida y vuelta del encuadre. Como el fixture vive en
`data/sample/projects` (misma unidad que las capas), QGIS escribe los
`datasource` **relativos** (`../shapefiles/cities.shp`), que es lo que ejercita
la comprobación 23.

El fixture debe abrirse en QGIS **antes de usarlo** (el propio script lo
reabre y falla si QGIS no lo lee) y en MiniGIS sin capas de más ni de menos.

### 9.2 Comprobaciones de `--check-project` (numeradas)

Con `qgis_ejemplo.qgz` y una copia en `build/test-output/`:

| # | Comprobación | Qué exige |
|---|---|---|
| 1 | se abren las capas del proyecto | 4 capas (3 vectoriales + 1 ráster), en orden `parcels`, `parcels_categorizado`, `cities` de fondo a frente |
| 2 | la capa rota se informa y no se abre | `no_existe` en `unavailableLayers()`, sin capa en el lienzo |
| 3 | visibilidad del proyecto | `parcels_categorizado` oculta; el resto visible |
| 4 | SRC del proyecto | `destinationEpsg() == 25830`; el estado lo muestra |
| 5 | encuadre del proyecto | extensión del lienzo = la del `<mapcanvas>` (tolerancia 1e-6 relativa) |
| 6 | simbología vectorial | `parcels`: relleno (200,80,40), contorno (80,20,10), grosor 0,5 mm→1,8898 px (tolerancia 0,01) |
| 7 | simbología de punto | `cities`: color de relleno y tamaño 2,5 mm→9,4488 px |
| 8 | etiquetas | `cities`: activas, `name`, 10 pt, colocación `OverPoint`, halo 1,5 mm→5,6693 px, escala mínima/máxima ≈ 66,147/1,3229 mpp (1 %) |
| 9 | estilo ráster | `orto`: bandas 1,2,3, opacidad 80, `Stretch::None` |
| 10 | renderizador no soportado | `parcels_categorizado` abierta con estilo por defecto y `styleRepresentable == false` |
| 11 | guardado sin cambios | guardar una copia sin tocar nada produce un `.qgz` que reabre con el mismo estado (SRC, extensión, capas, orden, visibilidad, estilos) |
| 12 | entradas del ZIP | un archivo ajeno añadido a la copia (`adjuntos/nota.txt`) sigue byte a byte después de guardar |
| 13 | XML desconocido | `<minigisPrueba>` sigue en el `.qgs` guardado |
| 14 | renderizador ajeno intacto | el `<renderer-v2 type="categorizedSymbol">` de la capa oculta es idéntico antes y después |
| 15 | cambio de visibilidad | ocultar `parcels` y guardar: `layer-tree-layer` (y, si el proyecto tiene `<legend>` con entradas, también `legendlayer`/`legendlayerfile`) queda coordinado |
| 16 | cambio de simbología | `setStyle` en `parcels` (relleno (10,20,30), grosor 2 px) y guardar: reabre con esos valores |
| 17 | cambio de etiquetas | dos campos (`name` y `country`) → `isExpression=1` con `concat`, tamaño 12, halo desactivado; reabre igual en MiniGIS y QGIS lo lee |
| 18 | capa añadida | `parcels_utm30.shp` añadida arriba: aparece en `<projectlayers>`, árbol, `<layerorder>` y `<legend>`, con `id` único; reabre y QGIS la abre |
| 19 | capa cerrada | quitar `parcels_categorizado` y guardar: desaparece de los cuatro sitios |
| 20 | capa rota conservada | `no_existe` sigue en el XML después de guardar |
| 21 | extensión cambiada | mover y hacer zoom, guardar: `<mapcanvas><extent>` refleja la vista nueva; `<projectCrs>` sigue intacto |
| 22 | `.qgs` y conversión | guardar como `.qgs`, reabrir; volver a `.qgz`; ambos equivalentes |
| 23 | Guardar como a otra carpeta | los `datasource` relativos se reescriben y el proyecto reabre con las capas |
| 24 | `.bak` y error de escritura | tras guardar dos veces sobre la copia existe `<copia>.bak` con el contenido de la primera; guardar a una carpeta inexistente devuelve 7, no deja archivos a medias y el proyecto original no cambia |
| 25 | firma de estado | proyecto recién abierto limpio; tras cambiar visibilidad, modificado; al volver a la visibilidad original, limpio |
| 26 | título | contiene el nombre del proyecto y `*` solo cuando hay cambios |
| 27 | `--project-info --json` | claves y valores estables del fixture (capas, `unavailable`, SRC, extensión) |

### 9.3 Pruebas de CTest

| Prueba | Qué ejecuta |
|---|---|
| `minigis.proyecto-info` | `--project-info --json` sobre el fixture, compara el contrato JSON |
| `minigis.proyecto` | `--check-project` (comprobaciones 1–27 de §9.2) |
| `minigis.proyecto-cli` | `--save-project` (`.qgz` y `.qgs`), proyecto inexistente → 2, ZIP corrupto → 7, `--info` sobre `.qgz` → 1 |
| `minigis.proyecto-ventana` | `--snapshot-window` con el proyecto: PNG con las 4 filas del panel y el título; revisión visual del archivo |
| `minigis.proyecto-qgis` | opcional, solo si se configura `-DMINIGIS_QGIS_PYTHON=D:/OSGeo4w/bin/python-qgis-ltr.bat`: `tests/qgis/verify_project.py` sobre `build/test-output/proyecto-modificado.qgz` |

`run_cli_test.cmake` gana los modos `project-info`, `check-project`,
`save-project` y `project-error`, con el mismo patrón que los existentes
(`EXPECT_EXIT`, `EXPECT_OK`, comparación de JSON con CMake).

### 9.4 Puerta de QGIS

`tests/qgis/verify_project.py` (PyQGIS 3.40) abre el proyecto indicado y
comprueba: `QgsProject.read()` verdadero, lista de capas y orden, visibilidad,
SRC del proyecto (`authid`), extensión del lienzo, relleno de `parcels`,
etiqueta de `cities` (campo o expresión), opacidad y bandas del ráster, y que la
capa rota sigue declarada. Imprime `OK`/`FALLO` y devuelve 0/1. Se ejecuta
manualmente en este equipo y queda registrada en la verificación; en CTest es
opcional para no atar el proyecto a una instalación de QGIS.

### 9.5 Matriz requisito → prueba

| Requisito | Comprobaciones |
|---|---|
| R1 | 1, 2, 3, 4, 5, 27 |
| R2 | 1, 10, 14, 20 |
| R3 | 11, 12, 22, 23, 24 |
| R4 | 6, 7, 8, 9, 15, 16, 17, 18, 19, 21 |
| R5 | 6, 7, 8, 9, 14, 17 |
| R6 | 1, 2, 18, 23 |
| R7 | 25, 26 |
| R8 | 27 y `minigis.proyecto-cli` |
| R9 | 12, 24 (`-V` reporta tiempos) |
| R10 | las 18 pruebas existentes |
| R11 | revisión del README |

---

## 10. Rendimiento y presupuesto

* Abrir el fixture (5 capas declaradas, 4 abiertas) debe completarse en menos de
  **1 s** en total sin contar la indexación de geometrías, que ya se mide aparte.
* Guardar el fixture no debe pasar de **100 ms** de escritura ZIP+XML, sin
  incluir el copiado de entradas ajenas.
* El guardado no debe aumentar la memoria retenida de forma apreciable: el árbol
  XML del proyecto se libera al cerrar el proyecto y no se guarda por capa.
* La apertura no debe disparar lecturas de atributos ni de geometría nuevas:
  `--check-project` compara los contadores de `--info` antes/después (mismo
  número de elementos, sin lecturas extra).

No se añade una puerta de rendimiento nueva (el proyecto es pequeño); si el
tiempo de guardado se degrada, se verá en `-V` y en la revisión, no en CTest.

---

## 11. Límites

**Siempre**

* Abrir y guardar tiene que dejar el proyecto utilizable en QGIS 3.40: la puerta
  de §9.4 se ejecuta antes de dar la entrega por terminada.
* Nunca se elimina del XML una capa por no poder abrirla, por ser de un
  proveedor no soportado o por tener un renderizador que MiniGIS no entiende.
* Solo se reescribe la simbología/etiquetas de las capas cuyo estado ha
  cambiado; el resto del XML se conserva.
* Escritura atómica y `.bak` antes de sustituir el original.
* Las capas siguen siendo de solo lectura: no se escribe en `.shp`, `.gpkg`,
  `.tif` ni en ninguna fuente.
* Listas de fuentes explícitas; comentarios Doxygen sin tildes.
* Todas las pruebas existentes siguen en verde.

**Preguntar antes**

* Añadir una dependencia que no sea `minizip` (ya instalada por GDAL).
* Cambiar los atajos existentes (`Ctrl+O` = abrir capa) o el orden del menú.
* Empezar a persistir nombres visibles de capa, grupos del panel o reordenación
  manual, o crear proyectos desde cero.
* Tocar el cargador más allá de añadir la variante por capa concreta.
* Cambiar `scripts/env.bat`/`CMakePresets.json` para las rutas actuales
  (necesario para compilar con los scripts, pero es entorno, no producto).

**Nunca**

* Reescribir el proyecto entero desde un modelo propio (perdería lo no
  interpretado).
* Guardar sin temporal ni copia de seguridad.
* Borrar capas rotas o renderizadores desconocidos «por limpieza».
* Modificar datos de las capas.
* Dar por buena la compatibilidad sin la puerta de QGIS o sin reabrir el
  proyecto guardado.
* Debilitar o borrar una prueba existente para que pase un cambio nuevo.

---

## 12. Criterios de éxito

| # | Criterio | Cómo se comprueba |
|---|---|---|
| SC1 | Compila con el preset actual y pasan **18/18** pruebas existentes | §7 |
| SC2 | `--check-project` imprime las 27 comprobaciones en `OK` | §9.2 |
| SC3 | El fixture se abre con 3 capas vectoriales y 1 ráster, en el orden de QGIS y con la capa rota informada | 1, 2 |
| SC4 | SRC, encuadre, visibilidad, simbología, etiquetas y estilo ráster del proyecto se aplican | 3–9 |
| SC5 | Un renderizador no soportado se abre con estilo por defecto y su XML no se toca al guardar | 10, 14 |
| SC6 | Guardar conserva el ZIP ajeno, el XML desconocido y las capas rotas | 12, 13, 20 |
| SC7 | Los cambios de visibilidad, estilo, etiquetas y extensión se guardan y reabren | 15–17, 21 |
| SC8 | Añadir y quitar capas deja el proyecto coherente en los cuatro sitios | 18, 19 |
| SC9 | Guardar como cambia de contenedor y reescribe rutas relativas sin romper capas | 22, 23 |
| SC10 | `.bak` existe tras sobrescribir, y un error de escritura devuelve 7 sin dejar archivos a medias | 24 |
| SC11 | El estado «modificado» y el título son correctos | 25, 26 |
| SC12 | `--project-info --json` es estable y útil para scripts | 27 y `minigis.proyecto-info` |
| SC13 | QGIS 3.40 abre el proyecto guardado con los cambios aplicados | §9.4 |
| SC14 | README actualizado y 18/18 sin regresiones | `ctest` |

Al implementar se añade aquí la fecha y el resultado real de cada comprobación,
igual que en los specs anteriores.

**Verificación del prerrequisito (2026-11-03).** `scripts\env.bat` y
`CMakePresets.json` actualizados a Qt 6.10.3 (`C:\Qtwin\6.10.3\msvc2022_64`),
vcpkg `D:\VCPKG` y SDK `D:\Windows Kits`; configuración limpia del preset
`windows-msvc-release`, compilación y `ctest`: **18/18**. `minigis --version`
informa Qt 6.10.3, GDAL 3.12.4 y PROJ 9.9.0.

**Avance de la implementación (2026-11-03).** T1–T8 hechas; checkpoints A y B
superados y C casi completo: fixture QGIS versionado, `XmlNode`/`ZipArchive` con
ida y vuelta comprobada, lectura y aplicación del proyecto, guardado en
`.qgz`/`.qgs` con rutas relativas, `.bak` y escritura atómica, y guardado de
cambios (visibilidad, simbología, etiquetas con expresión, estilo ráster,
encuadre, capas añadidas y cerradas). El título marca `*` y guardar deja el
proyecto limpio. `ctest`: **24/24**. La puerta de QGIS se ejecutó manualmente
sobre `build\windows-msvc-release\test-output\proyecto-modificado.qgz`:
relleno `#0a141e`, etiquetas `concat("name", ' · ', "country")`, opacidad 0,5,
`parcels_utm30` válida y `parcels_categorizado` eliminada. También se abrió el
proyecto real `TCAR01.qgz` (18 capas, SRC local sin código EPSG). Falta la
prueba manual del diálogo Guardar/Descartar/Cancelar y el cierre de T9–T11.

**Corrección (2026-11-03).** Al guardar un proyecto con `<mapcanvas>` en un SRC
local sin código EPSG, el encuadre se escribía en grados (SRC de dibujo) y al
reabrir se interpretaba como metros: el mapa quedaba en blanco hasta usar «Zoom
a la capa». Ahora se transforma al SRC real del `mapcanvas` y, si la vista no se
movió, no se reescribe (evita deriva en cada guardado). Se añadió el fixture
`qgis_local.qgz` y la prueba de ida y vuelta cubre guardar, mover la vista y
reabrir.

---

## 13. Supuestos (corregir si alguno no es cierto)

1. **Se soporta un subconjunto de QGIS**: se leen proyectos de QGIS 3.20–3.44 y
   se emiten los elementos que QGIS 3.28+ lee; no se pretende editar simbología
   compleja.
2. **El objetivo es el archivo del usuario**, no una exportación: guardar
   sobrescribe el mismo `.qgz` (con `.bak`) o crea otro con «Guardar como».
3. **«Los cambios del usuario»** son capas abiertas/cerradas, visibilidad,
   simbología, etiquetas, estilo ráster y encuadre. No incluye editar geometrías
   ni atributos; el SRC del proyecto se lee y se conserva (no hay interfaz para
   cambiarlo en v1).
4. **Se guardan las capas añadidas** en la sesión y se quitan del XML las que el
   usuario cierre; las que no se pudieron abrir nunca se quitan.
5. **El ráster sigue debajo del vectorial** (limitación actual del motor); el
   orden se conserva en el XML pero no se reproduce en pantalla si el proyecto
   los intercala.
6. **Se admiten `.qgz` y `.qgs`**; el primero es el caso principal.
7. **Un solo proyecto a la vez**; abrir otro sustituye el actual.
8. **La verificación de compatibilidad es con QGIS 3.40.15 LTR**, el instalado
   en este equipo; no se promete con versiones futuras.
9. **El nombre visible** de una capa de proyecto es su `<layername>`; para capas
   abiertas sueltas se mantiene el nombre de la capa de datos (OGR).
10. **`minizip` se usa como dependencia explícita** (ya instalada por GDAL); no
    se añade ninguna otra.
11. **El entorno de compilación actual** es Qt 6.10.3
    (`C:\Qtwin\6.10.3\msvc2022_64`), vcpkg en `D:\VCPKG` y Windows SDK en
    `D:\Windows Kits\10`. `scripts\env.bat`, `CMakePresets.json` y las rutas del
    `README.md` se han actualizado a esas ubicaciones y se ha verificado una
    configuración limpia (`cmake --preset windows-msvc-release`) con compilación
    y **18/18** pruebas el 2026-11-03.
12. **El fixture se versiona** y se regenera con PyQGIS solo cuando cambian los
    datos de ejemplo; QGIS no es dependencia de compilación ni de CTest.
13. **`<layerorder>` coincide con el orden del árbol** en los proyectos de QGIS
    observados (3.20–3.44). Se usa el árbol como fuente del orden; `<layerorder>`
    solo se actualiza para insertar o quitar los `id` de las capas nuevas o
    cerradas. Si un proyecto los tuviera en desacuerdo, manda el árbol.

---

## 14. Preguntas de la revisión (resueltas el 2026-11-03)

1. **Atajo de «Abrir proyecto»** → `Ctrl+Shift+O`; `Ctrl+O` sigue abriendo una
   capa, como hasta ahora.
2. **Guardar capas añadidas y quitar las cerradas** → sí: el proyecto guardado
   refleja las capas que el usuario tiene abiertas.
3. **Copia de seguridad** → sí: `<archivo>.bak` en cada guardado (se sobrescribe
   la copia anterior).
4. **Guardar como a otra carpeta** → se reescriben los `datasource` relativos
   para que sigan apuntando a los mismos archivos.
5. **Nombres de capa** → se muestra el `<layername>` del proyecto; las capas
   abiertas sueltas siguen usando el nombre de datos (OGR).
6. **Proyecto nuevo desde cero** → queda fuera de esta entrega (propuesta por
   defecto; no se discutió en la revisión).
7. **Puerta de QGIS** → obligatoria manualmente en este equipo antes de cerrar la
   entrega; opcional en CTest con `-DMINIGIS_QGIS_PYTHON=...` (propuesta por
   defecto).
8. **Entorno** → sí: `scripts\env.bat` y `CMakePresets.json` actualizados a Qt
   6.10.3 (`C:\Qtwin\6.10.3\msvc2022_64`), vcpkg (`D:\VCPKG`) y SDK
   (`D:\Windows Kits\10\bin\10.0.26100.0\x64`); el `README.md` refleja las mismas
   rutas. Verificado con configuración limpia y 18/18 (ver §12).
9. **Aviso de renderizadores no soportados** → un solo diálogo con detalle al
   abrir (propuesta por defecto).
10. **Etiquetas con escala** → se guarda la escala aproximada (propuesta por
    defecto).

Las respuestas 6, 7, 9 y 10 no se discutieron explícitamente en la revisión: se
mantienen las propuestas por defecto y, si alguna no encaja, se corrige antes de
la tarea del contrato (T9) sin cambiar el alcance.
