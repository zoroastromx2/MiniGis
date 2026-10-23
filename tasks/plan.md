# Implementation Plan: Selección del objeto identificado

> Plan técnico derivado de [SPEC-seleccion.md](../SPEC-seleccion.md), aprobado el
> 2026-10-22: solo el **primer** resultado, la tabla **cambia de capa** sin tocar
> el árbol, resalte con **halo oscuro + amarillo**, la tabla oculta **no se abre**,
> sin selección desde el panel, sin `Esc` y el panel marca **(seleccionado)**.
> Tareas concretas en [todo.md](todo.md).
> **Estado: implementado y verificado (2026-10-22)** — ver el cierre de
> `todo.md`.

## 1. Resumen

Identificar ya devuelve los resultados ordenados. Este cambio convierte el
primero en una **selección**: el elemento se resalta en el mapa (contorno real,
sin relleno) y, si la tabla de atributos está visible, se cambia a su capa y se
salta a su fila. Un clic sin resultados limpia la selección.

El trabajo se corta en **rebanadas verticales**: primero el resalte visible
(núcleo + lienzo), después la integración con el flujo de Identificar y la tabla,
luego el contrato JSON/CTest y, al final, la documentación y la verificación.
Cada corte deja el programa compilando y las 18 pruebas actuales verdes.

## 2. Arquitectura

```
                 ┌───────────────────────────────────────────────┐
                 │ src/gui/MainWindow::identifyAtMapPoint        │
                 │  · result.hits.front() -> selección            │
                 │  · si la tabla está visible -> capa + fila     │
                 └───────────────┬───────────────────────────────┘
                                 │
              ┌──────────────────┴───────────────────┐
              ▼                                      ▼
┌──────────────────────────────┐        ┌──────────────────────────────────┐
│ src/gui/MapCanvas            │        │ src/gui/AttributeTableModel      │
│  · setSelectedFeature()      │        │  · setLayer(capa del objeto)     │
│  · QPainterPath en coords    │        │  · fila = FID                    │
│    de mapa (una vez)         │        │  QTableView: selectRow + scrollTo│
│  · drawSelection() tras la   │        └──────────────────────────────────┘
│    caché, antes del marcador │
└──────────────┬───────────────┘
               │ featureRings(fid)
               ▼
┌──────────────────────────────┐
│ src/core/GeometryStore       │
│  · nivel 0 (memoria)         │
│  · caché de envolventes      │
│  · sin almacén -> envolvente │
└──────────────────────────────┘
```

Flujo de un clic con resultados:

1. `identifyAtMapPoint` obtiene el `IdentifyResult` como hoy.
2. Si está vacío: `clearSelectedFeature()` y la tabla no se toca.
3. Si hay resultados: `canvas->setSelectedFeature(hits.front().layer, fid)`.
4. El lienzo construye el trazado del elemento **una vez** y lo dibuja como
   overlay en cada cuadro (sin invalidar la caché).
5. Si el dock de atributos está visible: se pone el modelo en la capa del objeto,
   se selecciona la fila `fid` y se centra. El foco se queda en el mapa.
6. El panel de Identificar marca la fila del elemento con «(seleccionado)».

## 3. Decisiones de implementación

| Decisión | Motivo |
|---|---|
| `GeometryStore::featureRings(index, ...)` copia el nivel 0 (o la caché de envolventes) | El lienzo no debe conocer la disposición interna de los buffers ni el modo del almacén; una copia de **un** elemento al seleccionar es despreciable |
| Trazado en coordenadas de mapa, dibujado con `QTransform` y lápices cosméticos | Se construye una vez por selección; el grosor (1/3 px) no depende del zoom y no se recorren vértices por cuadro |
| Puntos: anillo de 6 px en coordenadas de píxel | Un punto no tiene forma que escalar; el anillo debe verse igual a cualquier zoom, como el marcador |
| Orden de overlays: etiquetas → selección → marcador | El marcador del clic sigue siendo visible y el resalte no tapa las etiquetas |
| El estado de selección vive en `MapCanvas`; `MainWindow` lo consulta | Una sola fuente de verdad; sirve igual para el clic, `--identify-at` y las capturas |
| El resalte no sube revisiones ni invalida la caché | Mismo patrón que etiquetas y marcador; la comprobación 30 lo exige |
| La tabla se re-apunta con un ayudante (`setAttributeLayer`) y luego `selectRow`/`scrollTo` | `replaceAttributeModel()` sigue mandando cuando el usuario cambia de capa en el árbol; la selección solo fuerza la capa del objeto cuando la tabla está visible |
| Sin `setFocus` en la vista de la tabla | El foco se queda en el mapa para seguir identificando |
| El panel recibe `selectedLayer`/`selectedFid` en su `View` | El sufijo «(seleccionado)» lo pinta el panel sin conocer el estado del lienzo |
| `--snapshot` comparte el ayudante de `--identify-at` para aplicar la selección | Evita duplicar la lógica de alcance (`--identify-layer`, todas las visibles) en dos sitios |
| Al cambiar el SRC de destino se limpia la selección | El trazado guardado estaría en otro sistema de referencia |
| Quitar la capa del objeto limpia la selección; ocultarla no | Quitar deja un puntero muerto; ocultar solo debe dejar de dibujar |

## 4. Orden de implementación

```
T1 núcleo + lienzo + resalte (checks 25, 30)
        │
        ▼
T2 MainWindow + tabla + panel (checks 24, 26-29, 31)
        │
        ▼
T3 CLI JSON + capturas + CTest (checks 24-31 completos)
        │
        ▼
T4 README + verificación final (SC1-SC13)
```

### Fase A — Resalte visible (T1)

`featureRings`, estado de selección en el lienzo, trazado cacheado, overlay,
limpieza por ciclo de vida y las comprobaciones de píxel y caché.

### Fase B — Integración (T2)

La selección se aplica desde Identificar; la tabla cambia de capa y salta a la
fila; el panel marca la fila. Comprobaciones de selección, limpieza, tabla y
panel.

### Fase C — Contrato y cierre (T3–T4)

`selection` en el JSON, resalte en las capturas, etiquetas de CTest, README y
pasada final de los criterios.

## 5. Índice de tareas

Detalle completo (criterios, verificación, archivos y tamaño) en
[todo.md](todo.md).

| # | Tarea | Fase | Dep. | Tamaño |
|---|---|---|---|---|
| T1 | Núcleo y lienzo: anillos, selección y resalte | A | — | M |
| T2 | Identificar selecciona; tabla y panel | B | T1 | M |
| T3 | CLI, JSON, capturas y CTest | C | T2 | M |
| T4 | README y verificación final | C | T3 | S |

## 6. Riesgos y mitigaciones

| Riesgo | Impacto | Mitigación |
|---|---|---|
| El `QTransform` del trazado invierte el eje Y o escala el lápiz | Alto | Se construye desde `MapToPixel` (origen, escala y signo) y se usan lápices cosméticos; la comprobación 25 mira el píxel del borde |
| El suavizado impide comparar el píxel exacto | Medio | La comprobación acepta `#FFD400` en un entorno de ±1 px y compara también con el cuadro previo sin selección |
| `selectRow` se pierde al reconstruir el modelo | Medio | Primero se fija la capa y después se selecciona/desplaza; no se reconstruye el modelo entre ambas operaciones |
| La tabla vuelve a la capa actual por un `replaceAttributeModel` inesperado | Medio | El ciclo de vida está definido: solo cambia con la selección del árbol o la lista de capas; la comprobación 29 fija el caso oculto |
| El sufijo del panel rompe las comprobaciones existentes | Bajo | Las comprobaciones usan `contains`, no igualdad de la etiqueta del elemento |
| El resalte invalida la caché | Alto | Se dibuja después de `cache.image = image`; la comprobación 30 exige `lastFrameReused()` |
| `--snapshot` duplica la lógica de alcance y se desvía de `--identify-at` | Medio | Un único ayudante resuelve capas, capa limitada y tolerancia; ambos modos lo llaman |
| Una capa sin almacén no tiene anillos | Bajo | Se resalta la envolvente; el caso normal siempre tiene almacén |
| El FID supera las 100 000 filas de la tabla | Bajo | Se avisa en la barra de estado y no se mueve la fila; la comprobación no lo fuerza |
| La selección de una capa oculta se dibuja | Bajo | `drawSelection` comprueba `isVisible()`; la comprobación no lo fuerza |

## 7. Paralelización

* **Secuencial:** T1 → T2 → T3 (comparten lienzo, ventana y `main.cpp`).
* **En paralelo:** T4 (README) se puede redactar mientras T3 estabiliza el JSON,
  pero se valida después.
* **Coordinación:** el contrato de `featureRings` y de `setSelectedFeature` no
  cambia después de T1; si cambia, se actualizan lienzo y ventana en la misma
  tarea.

## 8. Puntos de control

| Checkpoint | Después de | Exige |
|---|---|---|
| A — Resalte | T1 | Compila, 18/18, comprobaciones 25 y 30 en `OK`; la 26 puede quedar para T2 |
| B — Integración | T2 | Comprobaciones 24, 26–29 y 31 en `OK`; la tabla salta a la fila y el panel marca el elemento |
| C — Contrato | T3 | 24–31 en `OK`, `selection` en el JSON, captura con resalte y 18/18 |
| D — Cierre | T4 | SC1–SC13 verificados; README actualizado |

Cada checkpoint implica **revisión humana** antes de seguir (salvo autorización
expresa para continuar).

## 9. Preguntas abiertas para esta revisión

Ninguna pendiente: las siete decisiones de `SPEC-seleccion.md` §12 están
cerradas. Se mantienen las dos propuestas no preguntadas: si la capa no tiene
almacén se resalta la envolvente, y la fila de la tabla no recibe el foco.

La única decisión técnica que conviene revisar es el **orden de los overlays**
(etiquetas → selección → marcador) y que el resalte use **lápices cosméticos**
para que el grosor no dependa del zoom; ambas están en §3.
