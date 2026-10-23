# Spec: Selección del objeto identificado (resalte y tabla)

> Módulo: `seleccion` · Estado: **implementado y verificado** (2026-10-22) · Fecha: 2026-10-22
>
> Depende de [SPEC-identificar.md](SPEC-identificar.md), ya implementado y
> verificado. Este documento define *qué* se construye y *cómo se sabe que está
> bien*. El plan y las tareas están en `tasks/plan.md` y `tasks/todo.md`.
>
> Aprobado con estas decisiones: solo el **primer** resultado; la tabla **cambia
> de capa** sin tocar el árbol; resalte con **halo oscuro + amarillo**; la tabla
> oculta **no se abre**; sin selección desde el panel; sin `Esc`; el panel marca
> **(seleccionado)**.
>
> Verificado con compilación limpia de `windows-msvc-release`: **18/18** pruebas
> y las comprobaciones **24–31** de `--check-identify` en `OK`, con la captura
> `build\test-output\selection-native.png`.

---

## 1. Objetivo

Identificar hoy responde «¿qué es esto?» en un panel, pero el mapa no cambia: el
elemento consultado sigue igual que los demás y, si la tabla de atributos está
abierta, el usuario tiene que buscar la fila a mano. La consulta no deja rastro
visual.

Este cambio convierte el resultado de Identificar en una **selección**:

* el primer elemento identificado queda **seleccionado** y su **geometría se
  resalta** en el mapa (contorno, sin relleno);
* si la **tabla de atributos está visible**, se cambia a la capa del elemento y
  se **mueve al registro** seleccionado (fila y desplazamiento), sin abrirla si
  estaba cerrada;
* un clic nuevo sustituye la selección; un clic sin resultados la **limpia**.

**Usuario:** quien revisa datos y, tras identificar una parcela o una ciudad,
necesita verla señalada en el mapa y saltar a su fila para comparar valores con
lo que tiene delante.

**Criterio de éxito del producto:** tras un clic sobre un polígono, su contorno
se ve resaltado de forma inequívoca sobre cualquier simbología, la tabla (si está
abierta) muestra esa capa con la fila del elemento seleccionada y centrada, y todo
ello se puede comprobar sin ventana con el contrato extendido de
`--check-identify`.

---

## 2. Estado de partida (verificado leyendo el código)

| Comprobación | Resultado |
|---|---|
| ¿Existe selección de elementos? | **No.** Solo hay «capa actual» (`MapCanvas::currentLayer`) y la selección de fila de la tabla; no hay ningún elemento vectorial seleccionado ni resaltado |
| ¿Existe resalte? | **No.** El lienzo dibuja geometría, etiquetas y el marcador del clic; nada más |
| Resultado de Identificar | `IdentifyResult::hits` con `layer`, `fid`, `inside`, `distance` y valores; el primero es el de mayor prioridad / el de arriba ([minigisidentify.h](../src/core/minigisidentify.h)) |
| Tabla de atributos | `AttributeTableModel` se ata a `currentLayer()` en cada reconstrucción ([minigismainwindow.cpp:1319](../src/gui/minigismainwindow.cpp)); la fila es el FID y hay un tope de 100 000 filas |
| Vista de la tabla | `QTableView` con `SelectRows`, sin `scrollTo` ni selección programática ([minigismainwindow.cpp:309-317](../src/gui/minigismainwindow.cpp)) |
| Geometría por elemento | El `GeometryStore` tiene el nivel 0 y la caché de envolventes, pero solo expone `hitTest`; no hay forma pública de obtener los anillos de un elemento para dibujarlo aparte |
| Overlays existentes | Etiquetas y marcador del clic se dibujan **después** de guardar la caché del cuadro; el marcador demuestra que un overlay no invalida la caché ([minigismapcanvas.cpp](../src/gui/minigismapcanvas.cpp)) |
| Pruebas actuales | 18/18, con `--check-identify` (23 comprobaciones) ya en verde |

---

## 3. Requisitos funcionales

### R1 — Selección al identificar

* Tras una consulta de Identificar con **al menos un resultado**, se selecciona
  **el primer resultado** (`hits.front()`): su capa y su FID. Es el mismo orden
  que ya define R4 del spec de Identificar (capa prioritaria, apilado, dentro y
  distancia).
* Hay **un solo objeto seleccionado** a la vez. Un clic nuevo con resultados lo
  sustituye.
* Un clic **sin resultados** limpia la selección (y el resalte). Un panel vacío
  con un resalte antiguo sería una contradicción.
* Arrastrar (paneo), hacer zoom o cambiar la vista **no** cambia la selección.
* La selección está disponible para la interfaz y para el contrato sin ventana:
  capa, FID y un booleano «hay selección».
* `--identify-at` también deja la selección aplicada: el JSON añade
  `"selection": {"layer": "...", "fid": n}` o `null` si no hay resultados. Es el
  mismo primer resultado que muestra el panel.

### R2 — Resalte de la geometría

* El elemento seleccionado se dibuja como **contorno** (sin relleno) sobre el
  cuadro: todos los anillos de un polígono (exterior y huecos), el trazado de una
  línea y un anillo alrededor de un punto (radio en píxeles, independiente del
  zoom). Para un punto el anillo tiene **6 px de radio** (o el del símbolo + 3 px
  si el estilo usa un punto mayor), de modo que el resalte se vea aunque el
  marcador del clic caiga en el mismo sitio.
* Estilo propuesto, pensado para verse sobre cualquier simbología:
  * halo exterior de **1 px** por cada lado en gris muy oscuro (`#202020`),
  * trazo interior de **3 px** en amarillo (`#FFD400`), opaco.
  El color interior exacto (`#FFD400`) es lo que comprueba la prueba de píxeles.
* Se dibuja **después de las etiquetas y antes del marcador del clic**, sobre el
  cuadro ya renderizado, igual que el marcador: **no invalida la caché** ni
  obliga a redibujar la geometría.
* La geometría se toma del **nivel 0** (sin simplificar) para que el contorno
  coincida con la forma real. El trazado se construye **una vez por selección** y
  se dibuja con un lápiz cosmético; no se recorre la geometría en cada cuadro ni
  se toca la capa.
* Con paneo y zoom el resalte sigue a la geometría. Aparece también en
  `--snapshot` y `--snapshot-window` cuando hay una selección (por ejemplo, con
  `--identify-at`).
* Si la capa del elemento está **oculta**, no se dibuja el resalte (la selección
  se conserva y reaparece al mostrarla).
* Si la capa **no tiene almacén** (caso raro: no cupo ni en el índice de
  envolventes), se resalta su **envolvente** y se documenta el compromiso; el
  caso normal (almacén en memoria o por envolventes) resalta la forma real.

### R3 — Tabla de atributos

* Si la tabla está **visible** al seleccionar:
  * el modelo pasa a la **capa del elemento** si mostraba otra (sin cambiar la
    selección del árbol, ver R4);
  * se selecciona la **fila `fid`** y se desplaza para dejarla centrada
    (`QAbstractItemView::PositionAtCenter`); el índice actual queda en esa fila.
* Si la tabla está **oculta**, no se abre ni se cambia de capa: la selección del
  mapa no altera el layout que el usuario eligió. Al abrirla después se comporta
  como hoy (capa actual).
* Si el FID queda fuera de las filas mostradas (tope de 100 000), no se puede
  mover: se avisa en la barra de estado («El registro N no está en la tabla; se
  muestran los primeros 100 000») y no se cambia la fila actual.
* El título del dock sigue indicando la capa mostrada (comportamiento actual).
* Seleccionar una fila en la tabla **no** cambia la selección del mapa (fuera de
  alcance).

### R4 — Ciclo de vida y relación con el árbol

* La **capa seleccionada en el árbol no cambia**: la selección del objeto no
  toca `MapCanvas::currentLayer`, así que el alcance de Identificar aprobado
  (solo esa capa cuando hay selección) sigue siendo el mismo.
* La tabla puede mostrar la capa del objeto aunque no sea la del árbol; esa
  desviación dura hasta que el usuario cambie la selección del árbol o cambie la
  lista de capas, momento en que la tabla vuelve a seguir la capa actual (como
  hoy).
* Se limpia la selección cuando: el clic no encuentra nada, se quita la capa del
  elemento, o se cambia el SRC de destino (el trazado guardado estaría en otro
  sistema). Ocultar la capa no la limpia (solo deja de dibujarse).
* `Ctrl+Shift+I` sigue mostrando/ocultando el panel de Identificar; no afecta a
  la selección.

### R5 — Integración con el flujo actual

* La selección se aplica en `MainWindow::identifyAtMapPoint`, después de obtener
  el `IdentifyResult` y antes de mostrar el panel; el orden es: resaltar el
  objeto, mover la tabla y mostrar el panel.
* El panel de Identificar no cambia de estructura, pero la fila del elemento
  seleccionado se marca con el sufijo **«(seleccionado)»** para que panel y mapa
  coincidan.
* Identificar sin resultados deja el panel con su aviso y el mapa sin resalte.
* Las consultas internas de `--check-identify` que llaman al motor directamente
  no cambian la selección; solo las que pasan por la ventana.

### R6 — Verificación sin ventana

Se amplía `--check-identify` (mismo comando, mismas capas) con las
comprobaciones 24–31 de §8.2. No hay una opción nueva de la CLI salvo la clave
`selection` en el JSON de `--identify-at`.

### R7 — Rendimiento

* Aplicar la selección no recorre capas ni lee atributos (ya están en el
  resultado): solo construye el trazado de **un** elemento.
* El repintado posterior sigue **reutilizando** el cuadro anterior
  (`lastFrameReused()`), como el marcador.
* La consulta de Identificar mantiene su presupuesto (≤ 50 ms en los datos de
  ejemplo); el resalte no añade trabajo por vértice en cada cuadro.

### R8 — No regresión

* Las **18 pruebas actuales** siguen pasando y el contrato de Identificar (23
  comprobaciones) no cambia: solo se añaden comprobaciones.
* Paneo, zoom, etiquetas, tabla, diálogos y caché se comportan igual.
* Sin dependencias nuevas; listas de fuentes explícitas.

### R9 — Documentación

* `README.md`: qué hace la selección, el resalte, el salto de la tabla y el
  `selection` del JSON; §7 «Hace», §8 pruebas y §11 verificación.
* El estado de este spec pasa a «implementado» al cerrar la entrega.

### R10 — Fuera de alcance (explícito)

* Selección múltiple, por rectángulo, lazo o por consulta de atributos.
* Editar la geometría o los atributos del objeto seleccionado.
* Seleccionar desde la tabla y que el mapa se resalte (solo el sentido
  identificar → tabla).
* Estadísticas de la selección, exportarla o guardarla entre sesiones.
* Cambiar el color o el grosor del resalte desde la interfaz.
* Resaltar los demás resultados de Identificar (solo el primero).

---

## 4. Decisiones (y por qué)

1. **Solo el primer resultado.** El requisito habla de «el objeto»; el motor ya
   ordena por prioridad y cercanía, así que el primero es la respuesta que el
   usuario ve arriba. Resaltar los 50 resultados convertiría el mapa en un
   semáforo y no aporta.
2. **No cambiar la capa del árbol.** Es la decisión que mantiene intacto el
   alcance de Identificar aprobado. Si identificar cambiase la capa activa, el
   siguiente clic quedaría limitado a esa capa sin que el usuario lo hubiera
   pedido.
3. **La tabla se re-apunta a la capa del objeto, sin tocar el árbol.** Da la
   funcionalidad pedida (llegar al registro) sin efectos colaterales en el
   alcance. El título del dock dice qué capa se está viendo.
4. **Resalte como overlay con trazado cacheado.** Mismo patrón que etiquetas y
   marcador: cero invalidaciones de caché, cero recorridos de la capa por cuadro.
   Un resalte que obligue a redibujar la geometría sería un retroceso de
   rendimiento desproporcionado para señalar un elemento.
5. **Nivel 0, no el nivel simplificado del cuadro.** El contorno debe ceñirse a
   la forma real; usar el nivel de dibujo haría que el resalte «flote» fuera del
   polígono al alejarse.
6. **Halo oscuro + trazo amarillo.** El amarillo solo desaparece sobre rellenos
   amarillos o claros; el halo de 1 px garantiza contraste sobre cualquier
   simbología sin engordar el resalte.
7. **Limpiar al no encontrar nada.** Un resalte que sobrevive a un clic en vacío
   sugiere una selección que ya no corresponde con el panel.
8. **La tabla oculta no se abre.** Abrir un dock por un clic cambia el layout y
   tapa el mapa; el requisito dice «si se muestran».
9. **Fila = FID.** Es el convenio que ya usan la tabla, el índice de envolventes e
   Identificar; no hace falta buscar por atributos.
10. **`selection` en el JSON de `--identify-at`.** Es la forma de comprobar la
    selección sin ventana y de que un script sepa qué elemento se seleccionó.

---

## 5. Estructura del proyecto

```
src/core/
  minigisgeometrystore.*       + featureRings(index, ...) para el resalte
src/gui/
  minigismapcanvas.*           selección, trazado cacheado y overlay del resalte
  minigismainwindow.*          aplica la selección, mueve la tabla, accesores de prueba
src/app/main.cpp               selection en el JSON de --identify-at; checks 24-30;
                               resalte en --snapshot/--snapshot-window
tests/
  run_cli_test.cmake           etiquetas nuevas de --check-identify; JSON con selection
  CMakeLists.txt               (sin pruebas nuevas: se amplía minigis.identificar)
README.md                      comportamiento, tabla, pruebas y verificación
```

Regla de dependencias: la selección vive en `MapCanvas` (estado visual) y en
`MainWindow` (tabla); el núcleo solo aporta los anillos del elemento.

---

## 6. Comandos

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

```bat
:: contrato completo (incluye las comprobaciones de selección y tabla)
build\windows-msvc-release\bin\minigis.exe --check-identify ^
        -l data\sample\shapefiles\parcels.shp ^
        -l data\sample\shapefiles\cities.shp ^
        -l data\sample\rasters\ortho_rgb.tif

:: la selección tambien sale en el JSON
build\windows-msvc-release\bin\minigis.exe --identify-at -3.70379,40.41678 --json ^
        -l data\sample\shapefiles\parcels.shp -l data\sample\shapefiles\cities.shp

:: captura de la interfaz con el resalte y el registro en la tabla
build\windows-msvc-release\bin\minigis.exe --snapshot-window seleccion.png ^
        --identify-at -3.70379,40.41678 --with-attributes ^
        -l data\sample\shapefiles\parcels.shp -l data\sample\shapefiles\cities.shp
```

---

## 7. Estilo de código

Se mantiene el del proyecto: C++17, `namespace minigis`, miembros `m`, comentarios
en español sin tildes, `#pragma once`, y sin dependencias nuevas.

Interfaz propuesta (núcleo):

```cpp
/**
 * Copia los anillos del elemento `index` en el SRC del almacen (nivel 0).
 * Devuelve false si no hay geometria disponible.
 */
bool featureRings(uint32_t index, std::vector<double> &coords,
                  std::vector<uint32_t> &ringFirst, std::vector<uint32_t> &ringCount,
                  GeometryKind &kind) const;
```

Interfaz propuesta (lienzo):

```cpp
/** Selecciona un elemento y prepara su resalte; no invalida la cache. */
void setSelectedFeature(const std::shared_ptr<VectorLayer> &layer, qint64 fid);
void clearSelectedFeature();
bool hasSelectedFeature() const;
qint64 selectedFeatureId() const;
std::shared_ptr<VectorLayer> selectedFeatureLayer() const;
```

Cada función pública con su comentario Doxygen; el resalte nunca modifica la
revisión de la capa ni la caché del cuadro.

---

## 8. Estrategia de pruebas

### 8.1 Datos

Los mismos de Identificar: `parcels.shp` (P-001 fid 0, P-003 fid 2, P-005 con
hueco), `cities.shp` (Madrid fid 0), `ortho_rgb.tif`. Escala de la prueba:
0,00012 u.m./px.

### 8.2 Comprobaciones nuevas de `--check-identify`

| # | Comprobación | Qué exige |
|---|---|---|
| 24 | `identificar selecciona el primer resultado` | tras el clic en P-001, la selección es `parcels`/fid 0 y `hasSelectedFeature()` es true |
| 25 | `el objeto seleccionado se resalta en el mapa` | en `renderToImage`, el color `#FFD400` aparece a ±1 px del punto medio del borde superior de P-001, y no aparecía antes de seleccionar |
| 26 | `un clic sin resultados limpia la seleccion` | clic en el hueco de P-005: sin selección y sin `#FFD400` en el cuadro |
| 27 | `la tabla se mueve al registro seleccionado` | con la tabla visible en parcels, clic en P-003: el modelo muestra parcels y la fila actual es 2 |
| 28 | `la tabla cambia a la capa del objeto` | con la tabla visible en parcels, clic en Madrid: el modelo muestra cities y la fila actual es 0 |
| 29 | `la tabla oculta no se mueve` | con la tabla oculta y parcels seleccionada, clic en P-001: el modelo sigue en parcels y la fila no cambia |
| 30 | `la seleccion no invalida el cuadro cacheado` | tras seleccionar, `renderToImage` reutiliza el cuadro (`lastFrameReused()`) |
| 31 | `el panel marca el elemento seleccionado` | la primera fila de elemento del panel contiene «(seleccionado)» |

La comprobación 21 existente (marcador y caché) sigue igual.

### 8.3 Accesores de verificación

`MainWindow` expone, como ya hace con el panel de capas:
`selectedLayerName()`, `selectedFeatureId()`, `hasSelectedFeature()`,
`attributeTableLayerName()` y `attributeTableCurrentRow()`.

### 8.4 CTest

`minigis.identificar` exige las etiquetas 24–31 además de las 23 actuales.
`minigis.identificar-cli` comprueba `"selection"` en el JSON de Madrid y que sea
`null` en el clic del hueco. No se añaden pruebas nuevas: el contrato crece.

### 8.5 Matriz requisito → prueba

| Requisito | Prueba |
|---|---|
| R1 | 24, 26, `minigis.identificar-cli` |
| R2 | 25, 26, 30, captura |
| R3 | 27, 28, 29 |
| R4 | 26, 28, 29 |
| R5 | 24, 27, 31 |
| R7 | 30 |
| R8 | las 18 pruebas existentes |

---

## 9. Límites

**Siempre**

* Compilar el preset `windows-msvc-release` y ejecutar **todas** las pruebas
  (18 actuales + las comprobaciones nuevas) antes de dar una tarea por terminada.
* El resalte **no** puede subir revisiones de capa ni invalidar la caché del
  cuadro; se comprueba con `lastFrameReused()`.
* El trazado se construye una vez por selección; nunca se recorre la capa por
  cuadro.
* La capa del árbol no cambia al seleccionar un objeto.
* Documentar el comportamiento en `README.md` y anotarlo en la verificación.

**Preguntar antes**

* Cambiar el estilo o el color del resalte más allá de R2.
* Añadir selección múltiple, por rectángulo o desde la tabla.
* Cambiar el alcance de Identificar (la capa seleccionada limita) para que la
  selección del objeto active su capa.
* Tocar el motor de render más allá del overlay del resalte.
* Añadir dependencias (no se prevé ninguna).

**Nunca**

* Redibujar la geometría de la capa para pintar un resalte.
* Abrir la tabla de atributos sin que el usuario lo pida.
* Dejar un resalte de un elemento que ya no está seleccionado.
* Silenciar que la fila no se pudo mostrar (tope de 100 000).
* Debilitar o borrar una prueba existente para que pase un cambio nuevo.

---

## 10. Criterios de éxito

| # | Criterio | Cómo se comprueba |
|---|---|---|
| SC1 | Compila en limpio y pasan **18/18** pruebas, con las comprobaciones 24–31 en `OK` | §6 |
| SC2 | Tras identificar P-001, la selección es parcels/fid 0 | comprobación 24 |
| SC3 | El contorno de P-001 se ve en amarillo `#FFD400` sobre el mapa | comprobación 25 |
| SC4 | Un clic en el hueco limpia la selección y el resalte | comprobación 26 |
| SC5 | Con la tabla visible, identificar una parcela mueve a su fila | comprobación 27 |
| SC6 | Con la tabla visible en otra capa, se cambia a la capa del objeto | comprobación 28 |
| SC7 | Con la tabla oculta, no se abre ni se mueve | comprobación 29 |
| SC8 | Seleccionar no invalida la caché del cuadro | comprobación 30 |
| SC9 | La capa seleccionada en el árbol no cambia al seleccionar un objeto | 24, 28 |
| SC10 | El panel marca el elemento seleccionado con «(seleccionado)» | comprobación 31 |
| SC11 | `--identify-at --json` incluye `selection` (o `null`) | `minigis.identificar-cli` |
| SC12 | `--snapshot-window --with-attributes --identify-at` muestra el resalte y la fila | captura + revisión |
| SC13 | README actualizado y 18/18 sin regresiones | `ctest` |

**Verificación (2026-10-22).** Compilación limpia con `windows-msvc-release` y
`ctest --preset windows-msvc-release`: **18/18** (15 existentes + 3 de
Identificar). `--check-identify` imprime las **31** comprobaciones en `OK`;
`minigis.identificar-cli` valida `selection` (y `null` en el hueco);
`minigis.identificar-ventana` escribe el PNG con el panel, el resalte y la tabla.
La captura nativa `build\test-output\selection-native.png` muestra Madrid
seleccionado, el anillo amarillo, «#0 · Punto · dentro (seleccionado)» y la fila
0 de la tabla resaltada.

---

## 11. Supuestos (corregir si alguno no es cierto)

1. «El objeto» es **el primer resultado** de Identificar, no todos.
2. Solo hay **un** elemento seleccionado a la vez.
3. La **capa del árbol no cambia**; solo cambia la capa que muestra la tabla
   cuando hace falta.
4. El resalte es un **contorno** (sin relleno) con el estilo de R2.
5. La tabla solo se mueve si está **visible**; no se abre sola.
6. La fila de la tabla es el **FID** del elemento.
7. Un clic sin resultados **limpia** la selección.
8. Ocultar la capa **no** limpia la selección; quitarla sí.
9. La selección no se guarda entre sesiones.
10. El resalte se dibuja en el lienzo y también en las capturas
    (`--snapshot`/`--snapshot-window` con `--identify-at`).
11. La tabla conserva el tope de 100 000 filas; un FID por encima no se puede
    centrar y se avisa.
12. Toolchain: Qt 6.10.3, GDAL 3.12.4, MSVC 2022, preset `windows-msvc-release`.

---

## 12. Decisiones de la revisión

Resueltas el 2026-10-22:

1. **Solo el primer resultado.** Los demás no se resaltan.
2. **La tabla cambia de capa** cuando el objeto es de otra, **sin tocar la
   selección del árbol**.
3. **Resalte con halo oscuro (`#202020`, 1 px por lado) + trazo amarillo
   (`#FFD400`, 3 px).**
4. **La tabla oculta no se abre** ni cambia de capa.
5. **Un clic en el panel de resultados no selecciona** otro resultado: solo el
   primero.
6. **`Esc` no limpia** la selección; el clic sin resultados sí.
7. **El panel marca «(seleccionado)»** en la fila del elemento seleccionado.

Se mantienen las propuestas de §3 para los dos puntos no preguntados: si la capa
no tiene almacén se resalta la envolvente, y la fila de la tabla no recibe el
foco.
