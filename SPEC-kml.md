# Spec: apertura y visualización de KML/KMZ

> Módulo: `kml` · Estado: **aprobada e implementada (2026-11-20)** · Fecha: 2026-11-20
>
> Este documento define *qué* se construye y *cómo se sabe que está bien*. No se
> escribe código hasta que se apruebe. El plan técnico y las tareas van después,
> en `tasks/plan.md` y `tasks/todo.md`.

---

## 1. Objetivo

MiniGIS debe abrir y dibujar archivos **KML y KMZ** con la misma calidad que ya
tiene con shapefile, GeoPackage o GeoJSON: capas en el panel, geometría en el
mapa, atributos en la tabla, identificar, etiquetas y estilos.

Hoy la apertura **funciona por accidente afortunado**: GDAL trae el driver
`LIBKML`, MiniGIS abre el archivo como cualquier vectorial y lo dibuja con la
simbología por defecto. Sobre el archivo de ejemplo real (un KML de 21,6 MB con
las fronteras del mundo) el resultado es correcto. Pero la capa llega con
**geometría «desconocida»** y, al probar variantes, aparecen casos en los que
**elementos del archivo no se dibujan y nadie lo avisa**.

Este cambio convierte ese accidente en un **contrato explícito y probado**:

* abrir `.kml` y `.kmz` (también desde el diálogo del GUI, que hoy no ofrece
  `.kmz`);
* dibujar **todas** las geometrías que GDAL entregue, elemento a elemento,
  incluidas las `MultiGeometry` con partes de distinto tipo;
* no perder elementos en silencio: si algo no se puede dibujar, se cuenta y se
  informa;
* presentar la capa con un tipo de geometría legible (`Polígono`, `Mixta`…), no
  «Desconocido»;
* dejar la simbología y las etiquetas en un estado **decidido**, no accidental;
* verificarlo todo sin ventana, con el mismo estilo de contrato que `--info`,
  `--snapshot` y `--check-*`.

**Usuario:** quien recibe datos de campo, de Google Earth o de un colega y
necesita verlos en MiniGIS sin convertirlos antes con `ogr2ogr`.

**Criterio de éxito del producto:** soltar un `.kml` o `.kmz` en la ventana y ver
exactamente los mismos elementos que ve Google Earth (en planta), con sus
nombres en la tabla de atributos, sin elementos invisibles, sin bloqueos y sin
diferencias entre la CLI y el GUI.

---

## 2. Estado de partida (verificado hoy)

Comprobaciones ejecutadas en este equipo antes de escribir el spec, con
`build\windows-msvc-release\bin\minigis.exe` (MiniGIS 0.3.0, Qt 6.10.3, GDAL
3.12.4, PROJ 9.9.0, MSVC 19.44, Release).

Archivo de ejemplo del usuario:
`D:\F\IRIS 4.0\2026\KML\world-country-boundaries.kml` (21 688 093 bytes,
Google Earth Pro 7.3.6, `Schema` con ~160 `SimpleField`, 258 países).

| Comprobación | Resultado verificado |
|---|---|
| `--list-drivers` | aparecen `KML` y `LIBKML`; para `.kml`/`.kmz` manda `LIBKML` |
| `--info` del KML de ejemplo | exit 0, driver `LIBKML`, 1 capa «Country Bountries», **258 elementos**, `EPSG:4326`, extensión `-180,-90 : 180,83.634` |
| `--info --json` | `"driver": "LIBKML"`, `"features": 258`, campos de LIBKML (`id`, `Name`, `description`, `timestamp`, `altitudeMode`, `extrude`, `visibility`, `icon`…) + los `SimpleField` del `Schema` con su tipo |
| `--snapshot` 1200x800 | PNG correcto: mapa mundial completo, relleno gris, contorno negro, render 30 ms. **La visualización básica ya funciona** |
| Tiempo de apertura del KML de 21,6 MB | **583 ms** estables (3 tandas), `--snapshot` completo 777 ms |
| Arquitectura de la capa | `Geometria : Unknown (any) (Desconocido)` en `--info`, y lo mismo en el modelo del proyecto (`ProjectLayer::geometryName`) |
| KMZ (mismo KML comprimido) | abre igual, mismo driver, mismas capas y elementos |
| KML con dos `<Folder>` | genera **dos capas** con el nombre de cada carpeta (`Puntos`, `Areas`) |
| Línea horizontal, vertical y diagonal | las tres se dibujan (la horizontal con recuadro de altura cero también) |
| Polígonos con Z mezclados con polígonos sin Z | se dibujan ambos; la altitud se ignora y no rompe nada |
| `Placemark` sin geometría | no rompe la lectura |
| `MultiGeometry` (punto + línea + polígono) **sola** en el archivo | se dibujan las tres partes |
| `MultiGeometry` (punto + línea + polígono) **junto a otro polígono** | **la `MultiGeometry` desaparece del mapa**, sin aviso; el otro polígono se dibuja |
| Filtro del diálogo del GUI | incluye `*.kml` pero **no `*.kmz`** |
| Estilos del KML | hoy se **ignoran**: los KML de prueba salen con el gris por defecto de MiniGIS, tengan o no `<Style>` |
| Vía para leer estilos | GDAL expone el estilo resuelto de cada elemento con `OGR_F_GetStyleString`, activando `LIBKML_RESOLVE_STYLE=YES` (documentado en el driver LIBKML de GDAL) |
| Estilo en el motor | `minigisfastrenderer.cpp` fija **un `Style` por capa** y agrupa el dibujado; la caché de cuadro depende de `VectorLayer::revision()` (`RENDIMIENTO.md`) |
| Formato del color que entrega LIBKML | `#RRGGBBAA`, con el **alfa al final** (comprobado: KML rojo `ff0000ff` → `#FF0000FF`, azul `ffff0000` → `#0000FFFF`). Leerlo al revés intercambia rojo y azul en silencio |
| Estilo por defecto de KML | LIBKML devuelve un estilo a **todos** los elementos aunque el archivo no declare `<Style>` (el de por defecto de KML: línea roja de 1 px). El KML del usuario informa `PEN(c:#FF0000FF)` |
| Coste de resolver estilos | con `LIBKML_RESOLVE_STYLE=YES`, el KML de 21,6 MB pasa de 583 ms a ~600 ms en caliente (la primera tanda, 1238 ms, es de caché fría) |
| Carga en la ventana | hoy se **bloquea** con KML de muchas columnas: ajustar el ancho de las columnas de la tabla de atributos pide `data()` de todas las celdas y cada celda es una lectura del origen (LIBKML relee el archivo por elemento). Corregido: la medición se hace sobre una muestra de filas y solo con la tabla a la vista |

**Desviaciones de entorno** (relevantes para ejecutar los comandos de §6):

* El shell de este equipo tenía los permisos del workspace rotos; se repararon
  con `scripts` de diagnóstico de ACL. Comando de vuelta atrás impreso en
  `D:\AAA\MiniGIS\acl-recovery\`.
* vcpkg solo tiene la variante **Release** de GDAL: `windows-msvc-debug` no
  enlaza en este equipo. Todo se compila y prueba con `windows-msvc-release`.

**Conclusión del sondeo:** la lectura y el dibujado base están bien y no hay que
reescribir el motor vectorial. Lo que falta es **clasificar la geometría por
elemento** en capas de tipo desconocido, **no descartar nada en silencio**,
**nombrar bien el tipo en la interfaz** y **fijar el contrato** (simbología,
etiquetas, KMZ, pruebas).

---

## 3. Requisitos funcionales

### R1 — Apertura y detección de tipo

* Un `.kml` o `.kmz` se abre con `GDALOpenEx` en modo lectura, como cualquier
  vectorial. La detección sigue siendo **por contenido, no por extensión**.
* Si el driver `LIBKML` no está disponible pero sí `KML`, se usa `KML` y se
  informa del driver realmente usado en `--info` y en el panel.
* Si GDAL no reconoce el archivo, se devuelve `minigis::Error` con causa y acción
  (código de salida 3), y el texto menciona que un `.kmz` debe ser un ZIP con un
  `.kml` dentro.
* El diálogo **Archivo > Abrir capa** ofrece `*.kml` y `*.kmz`; arrastrar y
  soltar un `.kmz` sobre el lienzo lo abre.
* Un KML sin elementos devuelve error de «sin capas» (código 5), no una capa
  vacía dibujada en blanco.

### R2 — Clasificación de geometría por elemento

Esta es la raíz del defecto observado y la parte central del cambio.

* En una capa cuyo tipo declarado es desconocido (`wkbUnknown`, lo habitual en
  `LIBKML`), el tipo se decide **elemento a elemento**, a partir de la geometría
  real de cada `Placemark`.
* **Ningún elemento se descarta por su tipo de geometría.** Una
  `MultiGeometry`/colección se recorre y se dibujan todas sus partes; si alguna
  parte es de un tipo no soportado (TIN, poliedro, superficie 3D), se ignora
  **esa parte**, se cuenta y se informa (§R11), pero el resto del elemento se
  dibuja.
* El tipo de la capa que se muestra en `--info` y en el panel resume lo que hay:
  `Punto`, `Línea`, `Polígono`, o `Mixta` cuando conviven varios. **Nunca**
  «Desconocido» para una capa con geometría.
* Los elementos sin geometría (solo atributos) se conservan en la tabla de
  atributos y se cuentan aparte; no desplazan la correspondencia entre geometría
  y atributos de los demás.

### R3 — Dibujado de todos los tipos

* Punto, línea, polígono, multipunto, multilínea, multipolígono, colecciones
  (`MultiGeometry`) y sus variantes con Z se dibujan en planta, ignorando la
  altitud.
* Una línea o un polígono con recuadro degenerado (altura o anchura cero) se
  dibuja; no se descarta ni rompe el encuadre.
* El orden de dibujado es el de la capa; una colección se dibuja completa en su
  turno (relleno y contorno antes que puntos y líneas del mismo elemento, para
  que un punto no quede tapado por el relleno de su propio polígono).

### R4 — Carpetas, capas y nombres

* Cada `<Folder>` (también `<Document>` anidado) con geometría produce **una
  capa** cuyo nombre es el de la carpeta. Un KML sin carpetas produce **una
  capa** con el nombre del `<Document>`; si no lo tiene, el nombre del archivo.
* Si dos capas acaban con el mismo nombre, se desambigua con un sufijo
  (`Puntos`, `Puntos (2)`).
* El panel de capas muestra el tipo de geometría de §R2 en su columna
  **Geometría** y el número de elementos en **Elementos**.
* Se conserva el comportamiento actual de capas agrupadas por archivo: al abrir
  un KML con varias carpetas, las capas aparecen agrupadas bajo el nombre del
  archivo.

### R5 — Atributos

* La tabla de atributos muestra los campos que aporta `LIBKML` (`Name`,
  `description`, `timestamp`…) y los `SimpleField` definidos en el `Schema`, con
  su tipo (`entero`, `real`, `texto`).
* Los textos con HTML (típico en `description`) se muestran tal cual en la
  tabla; no se interpretan ni se recortan en el dato.
* Identificar (§ ver `SPEC-identificar.md`) devuelve los mismos atributos que la
  tabla, y funciona sobre puntos, líneas y polígonos de KML.

### R6 — Simbología: estilos del KML aplicados por capa

Decidido en la revisión (Q1): **los estilos del KML sí se importan**, acotados a
color y grosor.

* Se aplican **color de trazo, anchura de trazo, color de relleno y opacidad de
  relleno**. `IconStyle`, `BalloonStyle`, `ListStyle` y mapas de color quedan
  fuera (§5).
* La resolución la hace GDAL, no un lector propio: al abrir un KML se activa la
  configuración `LIBKML_RESOLVE_STYLE=YES`, con la que el driver resuelve
  `styleUrl` (también `StyleMap`, con la clave `normal`) contra la tabla de
  estilos del documento, y el estilo de cada elemento se lee con
  `OGR_F_GetStyleString`. El formato que llega es el *style string* de OGR, no
  el KML: `PEN(c:#RRGGBBAA,w:Npx)`, `BRUSH(fc:#RRGGBBAA,fo:N)`. **El alfa va al
  final**, al contrario de lo que sugiere el KML original (`aaBBGGRR`); tomarlo
  del principio intercambia rojo y azul, así que esa traducción tiene prueba
  propia.
* Traducción al `Style` de MiniGIS:
  * `PEN(c:)` → `strokeColor`; `PEN(w:)` → `strokeWidth` en píxeles, acotada a
    un máximo de **20 px** para que un `width` desmedido no tape el mapa.
  * `BRUSH(fc:)` → `fillColor`; `BRUSH(fo:)` → opacidad del relleno; `fc:none`
    desactiva el relleno. Sin `BRUSH` se conserva el relleno por defecto de
    MiniGIS: KML solo pinta relleno si lo declara, y dejar sin relleno un
    polígono que antes se veía relleno sería un cambio a peor.
  * Sin `PEN`, la capa se dibuja **sin trazo**.
* **El estilo se aplica por elemento** (decidido en la revisión, Q7). Cada
  elemento se dibuja con su color y su grosor: el almacén de geometrías guarda un
  índice de estilo por elemento y una tabla de estilos únicos, y el renderizador
  agrupa el dibujado **por estilo**, no por capa, de modo que sigue habiendo un
  número pequeño de `QPen`/`QBrush` por cuadro en vez de uno por elemento. El
  estado de estilo entra en `revision()`, así que la caché de cuadro sigue siendo
  válida.
* **Tope de estilos por capa: 64.** Si un KML declara más estilos distintos que
  ese tope (habitual en archivos generados con un color por registro), se aplica
  el estilo dominante a toda la capa y `--check-kml` informa
  `styleMode: "dominant"` junto con `distinctStyles`. Es un límite explícito y
  visible, no un recorte silencioso.
* Si la capa tiene varios estilos por debajo del tope, el panel lo indica
  («3 estilos») y `--check-kml` informa `distinctStyles`.
* Valores de `styleMode` en el diagnóstico: `none` (el archivo no trae estilos),
  `single` (un estilo aplicado a toda la capa), `perFeature` (varios estilos,
  cada elemento con el suyo) y `dominant` (el archivo declaraba más estilos que
  el tope y manda el mayoritario).
* Si la capa no trae estilos, se usa la simbología por defecto de MiniGIS, igual
  que en cualquier vectorial, y se informa `appliedStyles: 0`.
* El usuario puede sobrescribir lo importado con el diálogo de estilo; a partir
  de ahí manda su estilo y no se vuelve a importar al recargar la capa.

### R7 — Etiquetas

* El campo `Name` de KML está disponible para etiquetar con
  `--label-field Name` y desde el diálogo de etiquetas.
* Activar etiquetas automáticamente al abrir un KML queda a decisión en §13
  (Q2); por defecto **no** se activan, igual que en cualquier otra capa.

### R8 — Sistema de referencia

* KML se interpreta, como marca su especificación y ya hace GDAL, como
  **`EPSG:4326` (WGS 84, longitud/latitud)**. No se asume ningún otro SRC y no
  se pregunta al usuario.
* La reproyección al SRC del lienzo es la del resto de capas vectoriales; no hay
  ruta especial para KML.
* Longitudes alrededor del antimeridiano se dibujan como vienen (sin recortar a
  `[-180, 180]`), para no partir países ni rutas.

### R9 — KMZ

* Un `.kmz` es un ZIP con un `.kml` dentro. Se abre con el mismo driver y debe
  dar exactamente las mismas capas, nombres, atributos y dibujo que el `.kml`
  equivalente.
* El diálogo del GUI y la ayuda de formatos mencionan `.kmz`.

### R10 — Rendimiento y memoria

* El KML de referencia (21,6 MB, 258 elementos) abre y dibuja por debajo de
  **1,5 s** en Release, sin superar **1,5×** el tiempo actual de apertura
  (583 ms) en más de un 50 %.
* Un KML grande (8-10 MB con decenas de miles de placemarks) no debe congelar la
  interfaz: el progreso que ya existe (`minigis.loading`) sigue funcionando.
* No se materializa el archivo entero en memoria más allá del almacén de
  geometrías que ya usa el motor vectorial, que además tiene presupuesto de
  vértices y modo de respaldo por envolventes (`RENDIMIENTO.md`).

### R11 — Diagnóstico verificable

Nuevo modo sin ventana, coherente con los que ya existen:

```bat
bin\minigis.exe --check-kml data\sample\kml\mixto.kml --json
```

Contrato mínimo de la salida JSON:

```json
{
  "ok": true,
  "driver": "LIBKML",
  "layers": [
    {
      "name": "Areas",
      "features": 5,
      "geometry": "Mixta",
      "kinds": {"polygon": 3, "line": 1, "point": 1},
      "withoutGeometry": 1,
      "unsupportedParts": 0,
      "distinctStyles": 1,
      "extent": [-6.0, 39.0, -1.0, 44.0],
      "crsEpsg": 4326
    }
  ],
  "declaredStyles": 1,
  "appliedStyles": 1,
  "styleMode": "perFeature",
  "style": {"strokeColor": "#ff0000", "strokeWidth": 4, "fillColor": "#ff0000", "fillOpacity": 50}
}
```

* `ok` es `true` solo si **ningún elemento con geometría quedó sin dibujar**
  (`withoutGeometry` cuenta solo los que no traen geometría de origen) y
  `unsupportedParts` es 0 para los datos de ejemplo.
* Códigos de salida: `0` correcto, `1` uso, `2` archivo no encontrado, `3` GDAL
  no abre, `5` sin capas. No se añaden códigos nuevos.
* El mismo diagnóstico funciona sobre `.kmz`.

### R12 — No regresión

* Shapefile, GeoJSON, GeoPackage, GeoJSON comprimido y ráster siguen abriendo y
  dibujando igual; `ctest --preset windows-msvc-release` sigue en verde.
* El motor de render no cambia de comportamiento para capas cuyo tipo es
  conocido: la ruta nueva solo se activa cuando el tipo declarado es
  desconocido.
* Cualquier estado que afecte al dibujo (visibilidad, simbología, almacén de
  geometrías) sigue pasando por `VectorLayer::revision()` / `bumpRevision()`.

---

## 4. Decisiones (y por qué)

**D1 — Clasificar por elemento, no por capa.** El tipo declarado por `LIBKML` es
`wkbUnknown` siempre, así que cualquier decisión basada en el tipo de capa es una
suposición. Clasificar elemento a elemento es lo único que garantiza que lo que
se ve coincide con lo que hay. Coste: una comprobación por elemento, que ya se
hace al leer la geometría para el almacén.

**D2 — No descartar en silencio.** El defecto detectado en §2 no da error: el
mapa sale «bien» con un polígono menos. Un visor que oculta datos sin avisar es
peor que uno que falla. Por eso R11 contabiliza y §R2 prohíbe el descarte por
tipo.

**D3 — Las carpetas son capas.** Es cómo lo entrega `LIBKML` y coincide con lo
que el usuario espera de un KML de Google Earth: cada carpeta que se puede
apagar por separado en Earth es una capa que se puede apagar por separado aquí.

**D4 — KML es 2D en esta versión.** MiniGIS es un visor en planta; la altitud,
`extrude` y `altitudeMode` se ignoran. Añadir un visor 3D es otro producto, no
un requisito de esta spec.

**D5 — El SRC lo pone el formato.** KML es WGS 84 por especificación. Preguntar
el SRC al abrir un KML sería ruido; asumirlo es correcto y es lo que hace
cualquier visor.

**D6 — Un solo driver, el que elija GDAL.** No se implementa un lector de KML
propio ni se usa `QXmlStreamReader` para esto: GDAL ya resuelve espacios de
nombres, `Schema`, `SimpleData`, carpetas y KMZ, y el proyecto tiene la norma de
usar la API C de OGR. El único trabajo nuestro es no perder información después.

**D7 — Un modo `--check-kml` en vez de pruebas que miran PNG.** El proyecto ya
decidió que lo verificable se verifica sin ventana (`AGENTS.md`, `README.md` §7).
Comparar píxeles es frágil; contar capas, elementos y tipos por elemento es
estable.

**D8 — Los estilos los resuelve GDAL, no nosotros.** Implementar un lector de
`<Style>`/`<StyleMap>` propio sería un segundo recorrido del XML (21 MB en el
caso real), duplicaría la resolución de `styleUrl` externos y chocaría con la
norma de usar GDAL para todo lo que GDAL ya sabe hacer. `LIBKML_RESOLVE_STYLE`
hace exactamente ese trabajo y entrega el estilo ya en el *style string* de OGR,
que es una superficie estable y documentada.

**D9 — Estilo por capa en esta versión.** El renderizador está medido y agrupado:
un `Style` por capa y caché por `revision()`. Aplicar el estilo dominante da el
80 % del valor (colores y grosores correctos en el caso normal de un estilo por
capa, que es el habitual en KML de campo) sin tocar la ruta caliente. El estilo
por elemento queda como incremento con medición propia (Q7).

---

## 5. Fuera de alcance (explícito)

* Escribir, editar o exportar KML/KMZ (solo lectura, como el resto de capas).
* `IconStyle` (iconos de punto), `BalloonStyle`, `ListStyle`, `LabelStyle` y
  mapas de color de KML: fuera. Los estilos se aplican solo en color y grosor de
  línea y color y opacidad de relleno (§R6), y por capa, no por elemento.
* `NetworkLink`, `PhotoOverlay`, `ScreenOverlay`, `GroundOverlay`, `Region` y
  `TimeSpan`/`TimeStamp` como comportamiento: se leen como atributos si LIBKML
  los expone, pero no cambian el dibujo.
* Visor 3D, relieve, `tessellate` real, modelos `COLLADA`.
* Vista geodésica o en proyección esférica: se dibuja en planta como el resto.
* Web Map Service, teselas o cualquier cosa de red.

---

## 6. Comandos

### 6.1 Compilar y probar (el bucle del proyecto)

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

Ejecutable: `build\windows-msvc-release\bin\minigis.exe` (o
`scripts\run.bat windows-msvc-release`).

### 6.2 Uso de lo nuevo

```bat
:: abrir e informar
bin\minigis.exe --info "D:\F\IRIS 4.0\2026\KML\world-country-boundaries.kml"
bin\minigis.exe --info --json -l data\sample\kml\mixto.kml

:: dibujar y mirar
bin\minigis.exe --snapshot mapa.png -l data\sample\kml\mixto.kml --size 1200x800

:: verificar sin ventana
bin\minigis.exe --check-kml data\sample\kml\mixto.kml --json

:: KMZ
bin\minigis.exe --check-kml data\sample\kml\mixto.kmz --json

:: GUI
bin\minigis.exe
::   Archivo > Abrir capa (Ctrl+O) > filtro «Capas (*.kml *.kmz ...)»
```

### 6.3 Tiempos de referencia (medidos hoy, Release)

```bat
bin\minigis.exe --info "D:\F\IRIS 4.0\2026\KML\world-country-boundaries.kml"
::   ~583 ms en este equipo (3 tandas)
bin\minigis.exe --snapshot kml.png -l "D:\F\IRIS 4.0\2026\KML\world-country-boundaries.kml" --size 1200x800
::   ~777 ms en total, 30 ms de render
```

---

## 7. Estructura del proyecto

```
src/core/minigislayerkind.cpp        deteccion de tipo (ya existe; sin cambios de contrato)
src/core/minigisvectorlayer.cpp      tipo de geometria de la capa -> resumen legible (R2)
src/core/minigisgeometrystore.cpp    clasificacion por elemento y colecciones (R2, R3)
src/core/minigisstyle.cpp            style string de OGR -> Style de MiniGIS (R6)
src/core/minigiskmlinspect.cpp       NUEVO: recuento por tipos para --check-kml (R11)
src/core/minigiskmlinspect.h         NUEVO
src/gui/minigismainwindow.cpp        filtro del dialogo con *.kmz (R1)
src/app/main.cpp                     opcion --check-kml (R11)
tests/                               pruebas de CTest (R11, R12)
data/sample/kml/                     NUEVO: datos de ejemplo deterministas
   mundo.kml                         poligonos, atributos y Schema
   paises.kml                        el de ejemplo del usuario, recortado o completo
   mixto.kml                         el caso que hoy falla (ver §9.1)
   mixto.kmz                         el mismo, empaquetado
   carpetas.kml                      dos Folder con nombres
   solo_puntos.kml                   puntos con Name
```

Regla del proyecto: al añadir o quitar un archivo fuente hay que editar el
`CMakeLists.txt` de su carpeta. Nunca `file(GLOB ...)`.

---

## 8. Estilo de código

El del proyecto (`AGENTS.md`): C++17, `#pragma once`, namespace `minigis`,
`PascalCase` para tipos, `camelCase` para métodos y variables, prefijo `m` en
miembros privados, `enum class`, archivos `minigis<nombre>.cpp/.h` en minúsculas,
comentarios Doxygen en español sin tildes explicando el **por qué**. GDAL solo
por la API C de OGR (`OGR_L_*`, `OGR_F_*`, `OGR_G_*`).

```cpp
/**
 * Clasifica la geometria de un elemento cuando la capa declara un tipo
 * desconocido (KML, GeoJSON mixto). Devuelve Mixta si conviven varios tipos:
 * el panel debe poder decir que la capa no es homogenea.
 */
GeometryKind classifyFeatureGeometry(const OGRGeometry *geometry)
{
  // Una coleccion no es un tipo de dibujo: es un contenedor. Se recorre para
  // contar sus partes y se resume; nunca se descarta el elemento entero.
  if (wkbFlatten(geometry->getGeometryType()) == wkbGeometryCollection)
    return summarizeCollection(geometry->toGeometryCollection());
  return kindForOgrType(geometry->getGeometryType());
}
```

---

## 9. Estrategia de pruebas

### 9.1 Datos de ejemplo (deterministas, versionados)

`data/sample/kml/mixto.kml` — el caso que hoy falla, mínimo y legible:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2"><Document><name>mixto</name>
  <Placemark><name>coleccion</name>
    <MultiGeometry>
      <Point><coordinates>-6.0,42.0,0</coordinates></Point>
      <LineString><coordinates>-6,42 -4,43</coordinates></LineString>
      <Polygon><outerBoundaryIs><LinearRing><coordinates>
        -6,43 -6,44 -4,44 -4,43 -6,43
      </coordinates></LinearRing></outerBoundaryIs></Polygon>
    </MultiGeometry>
  </Placemark>
  <Placemark><name>poligono</name>
    <Polygon><outerBoundaryIs><LinearRing><coordinates>
      -2,42 -2,43 -1,43 -1,42 -2,42
    </coordinates></LinearRing></outerBoundaryIs></Polygon>
  </Placemark>
</Document></kml>
```

Con este archivo hoy: se dibuja el polígono y **desaparece la colección**.
Después del cambio: se dibujan los dos polígonos, la línea y el punto, y
`--check-kml` informa `kinds: {polygon: 2, line: 1, point: 1}`.

### 9.2 Pruebas de CTest

Con `tests/run_cli_test.cmake`, en plataforma `offscreen`, sin mirar píxeles:

| Prueba | Qué comprueba |
|---|---|
| `kml-info` | `--info --json` sobre `mundo.kml`: driver, 1 capa, elementos, `EPSG:4326`, extensión |
| `kml-mixto` | `--check-kml --json` sobre `mixto.kml`: `ok: true`, `unsupportedParts: 0`, tipos por elemento correctos |
| `kml-kmz` | El mismo `--check-kml` sobre `mixto.kmz` da salida idéntica a la de `mixto.kml` (salvo la ruta) |
| `kml-carpetas` | `carpetas.kml` produce dos capas con los nombres de las carpetas |
| `kml-sin-carpetas` | `mundo.kml` produce una capa con el nombre del `Document` |
| `kml-sin-geometria` | Un placemark sin geometría no rompe la lectura y se cuenta en `withoutGeometry` |
| `kml-error` | Un archivo `.kml` corrupto sale con código 3 y un mensaje con causa y acción |
| `kml-vacio` | Un KML sin elementos sale con código 5 |
| `kml-snapshot` | `--snapshot` genera un PNG no vacío (tamaño mínimo de archivo) |
| `kml-estilos` | `--check-kml --json` sobre un KML con un `<Style>`: `appliedStyles: 1`, `distinctStyles: 1` y el color y la anchura esperados |
| `kml-estilos-varios` | KML con dos estilos y mayoría de uno: `distinctStyles: 2` y se aplica el dominante |
| `kml-estilos-sin` | KML sin `<Style>`: `appliedStyles: 0` y simbología por defecto |
| `kml-estilos-tope` | KML con 70 estilos: `styleMode: "dominant"` y `appliedStyles: 1` |
| `kml-ventana` | Cargar un KML con la tabla de atributos a la vista no bloquea la ventana (límite de 120 s) |

### 9.3 Contrato de `--check-kml`

* `--check-kml <archivo>` acepta `--json`; sin `--json` imprime una tabla legible.
* Sale `0` si todos los elementos con geometría se pueden dibujar; `1` si falta
  el argumento; `2` si no existe el archivo; `3` si GDAL no lo abre; `5` si no
  hay capas.
* Cuenta `unsupportedParts` por elemento, no por archivo, para que el mensaje
  diga qué elemento falla.

### 9.4 Matriz requisito → prueba

| Requisito | Prueba |
|---|---|
| R1, R9 | `kml-info`, `kml-kmz`, `kml-error`, `kml-vacio` |
| R2, R3 | `kml-mixto` (+ revisión visual del `--snapshot`) |
| R4 | `kml-carpetas`, `kml-sin-carpetas` |
| R5 | `kml-info` (campos y tipos), comprobación manual en la tabla |
| R6, R7 | `kml-estilos`, `kml-estilos-varios`, `kml-estilos-sin`; revisión visual del `--snapshot` |
| R8 | `kml-info` (`crsEpsg: 4326`) |
| R10 | Medición manual con el KML de 21,6 MB; sin puerta automática nueva |
| R11 | Todas las pruebas de §9.2 |
| R12 | `ctest` completo + `minigis.rendimiento` |

---

## 10. Límites

* **Ask first / preguntar antes de:** tocar el motor de render (leer antes
  `RENDIMIENTO.md`), cambiar códigos de salida, añadir una dependencia nueva
  (no hace falta ninguna: LIBKML ya está en el GDAL de vcpkg).
* **Always / siempre:** compilar y pasar `ctest` antes de dar algo por hecho;
  reproducir un caso con un archivo de `data/sample/kml/` antes de arreglarlo;
  actualizar `README.md` y esta spec si cambia el contrato.
* **Never / nunca:** descartar elementos en silencio; implementar un lector de
  KML propio; usar las clases C++ de `OGRLayer` en código nuevo; cambiar el
  comportamiento de capas cuyo tipo es conocido.

---

## 11. Criterios de éxito

1. `minigis --info` sobre el KML de ejemplo del usuario informa un tipo de
   geometría legible (`Polígono`), no «Desconocido».
2. `--check-kml data\sample\kml\mixto.kml --json` devuelve `ok: true` y
   `unsupportedParts: 0`, y el `--snapshot` del mismo archivo muestra los dos
   polígonos, la línea y el punto.
3. `mixto.kmz` da exactamente el mismo resultado que `mixto.kml`.
4. Un KML con dos carpetas produce dos capas con sus nombres, y el panel muestra
   el tipo y el número de elementos de cada una.
5. `ctest --preset windows-msvc-release` pasa entero, incluidas las pruebas
   nuevas de §9.2.
6. Los tiempos de §2 no empeoran más de un 50 % con el mismo archivo.
7. Un shapefile, un GeoPackage y un GeoJSON siguen dando la misma salida que
   antes del cambio (comparación de `--info --json` y `--snapshot`).
8. Un KML con `<LineStyle><color>ff0000ff</color><width>4</width></LineStyle>`
   se dibuja con trazo rojo de 4 px, y `--check-kml` informa `appliedStyles: 1`
   con ese color y esa anchura. Un KML con dos estilos dibuja **cada elemento con
   el suyo** (`styleMode: "perFeature"`, `distinctStyles: 2`); uno con más de 64
   estilos se dibuja con el dominante e informa `styleMode: "dominant"`.

---

## 12. Supuestos (corregir si alguno no es cierto)

1. El objetivo es **leer y ver** KML/KMZ, no editarlos ni exportarlos.
2. El usuario trabaja con KML de Google Earth y de terceros, no con KML
   generados por MiniGIS.
3. La altitud no importa: el visor es en planta.
4. Los estilos de KML **no** son imprescindibles para la primera versión; lo
   imprescindible es que no falte ningún elemento.
5. Los archivos de ejemplo se pueden versionar en `data/sample/kml/`; el KML de
   21,6 MB del usuario se usa solo para la medición de rendimiento, no como
   fixture de `ctest` (demasiado grande y con datos de terceros).
6. El GUI y la CLI deben dar el mismo resultado; la CLI es la fuente de verdad
   verificable.

---

## 13. Preguntas abiertas (para la revisión)

* **Q1 — ¿Se importan los estilos del KML?** **Resuelto en la revisión: sí**,
  acotado a color y anchura de trazo y color y opacidad de relleno, aplicado
  **por capa** con el estilo dominante (§R6). El resto de estilos queda fuera
  (§5).
* **Q2 — ¿Se activan las etiquetas con el campo `Name` al abrir un KML?**
  Recomendación: **no por defecto** (como cualquier otra capa), pero dejar el
  campo disponible y documentarlo en `README.md`.
* **Q3 — Un `MultiGeometry` mixto, ¿se dibuja como un solo elemento
  identificable o como elementos separados?** Recomendación: **un solo
  elemento**, porque así está en el archivo y así lo cuenta `--check-kml`; el
  panel de identificar muestra sus atributos una vez.
* **Q4 — ¿Se acepta que un KML de 21,6 MB tarde ~0,6 s en abrir?**
  Recomendación: sí; si se quiere más rápido, se decide en el plan técnico, no
  en esta spec.
* **Q5 — ¿Qué debe pasar con un KML que trae `NetworkLink` a un servidor?**
  Recomendación: se ignora (está en §5) y `--info` no lo menciona; no se hace
  ninguna petición de red.
* **Q6 — ¿El nombre de capa debe ser el de la carpeta o el del archivo seguido
  de la carpeta?** Recomendación: el de la carpeta, y el archivo como grupo en
  el panel (comportamiento actual de capas agrupadas).
* **Q7 — ¿Estilo por elemento (cada polígono con su color)?** **Resuelto en la
  revisión: sí**, con tope de 64 estilos por capa y repliegue al dominante por
  encima de ese tope (§R6). El renderizador agrupa por estilo, no por elemento, y
  la medición contra `minigis.rendimiento` forma parte de la tarea.
