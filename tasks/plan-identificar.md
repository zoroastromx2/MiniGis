# Implementation Plan: Identificar en MiniGIS

> Plan técnico derivado de [SPEC-identificar.md](../SPEC-identificar.md), aprobado
> el 2026-10-22 con estas decisiones: **dock** no modal, la capa seleccionada
> **limita** el ámbito y los valores de píxel del ráster quedan **fuera**.
> Tareas concretas en [todo.md](todo.md).
> **Estado: implementado y verificado (2026-10-22)** — ver el cierre de
> `todo.md`.

## 1. Resumen

Añadir **Identificar**: un clic sin arrastre sobre el lienzo muestra, en un dock,
la capa, el FID y todos los campos del elemento —o de los elementos— que hay bajo
el clic. Con una capa seleccionada en el árbol la consulta se limita a ella; sin
selección se consultan todas las visibles, ordenadas por apilado y distancia. La
prueba es exacta (polígonos con huecos incluidos) y todo se verifica sin ventana
con un `--check-identify` de 23 comprobaciones, un `--identify-at --json` y una
captura de la interfaz.

El trabajo se corta en **rebanadas verticales**: primero un camino completo que
prueba la geometría y responde por CLI con texto, después los casos difíciles del
almacén (sin índice y modo envolventes), luego el orden y el recorte, después la
interacción (clic, marcador, panel, alcance) y, al final, el contrato JSON, las
pruebas de CTest y la documentación. Cada corte deja el programa usable y las 15
pruebas actuales verdes.

## 2. Arquitectura

```
                    ┌──────────────────────────────────────────┐
                    │ src/app/main.cpp                         │
                    │  --identify-at / --identify-radius       │
                    │  --identify-layer / --check-identify     │
                    │  --snapshot(-window) + marcador          │
                    └───────────────┬──────────────────────────┘
                                    │
              ┌─────────────────────┴─────────────────────┐
              ▼                                           ▼
┌──────────────────────────────┐            ┌──────────────────────────────┐
│ src/gui                      │            │ src/core                     │
│  MainWindow                  │  petición  │  identifyAtPoint()           │
│   · alcance (árbol + casilla)│ ─────────▶ │   · capas y prioridad        │
│   · identifyAtMapPoint()     │            │   · candidatos (R-tree)      │
│   · selectLayer()            │  resultado │   · orden, topes, atributos  │
│  IdentifyPanel (dock)        │ ◀───────── │  IdentifyRequest/Result      │
│  MapCanvas                   │            │                              │
│   · clic sin arrastre        │            │  GeometryStore::hitTest()    │
│   · marcador en el overlay   │            │   · nivel 0 / caché de       │
│                              │            │     envolventes              │
└──────────────────────────────┘            │  minigisgeometryhit          │
                                            │   · paridad + distancia      │
                                            └──────────────────────────────┘
```

Flujo de un clic:

1. `MapCanvas` detecta press + release con desplazamiento máximo ≤ 3 px y emite
   `identifyRequested(mapPoint, pixel)`. El punto se calcula con el mismo
   `MapToPixel` del dibujado.
2. `MainWindow` construye la petición: capas visibles en orden de apilado, capa
   prioritaria (la del árbol) y tolerancia = `radioPx × mapUnitsPerPixel()`.
3. `identifyAtPoint()` consulta el R-tree por capa, prueba la geometría exacta
   (`hitTest`), ordena, recorta y lee los atributos solo de lo que se va a mostrar.
4. `IdentifyPanel` pinta cabecera, grupos por capa, elementos y campos; el lienzo
   guarda el punto y dibuja la cruz **después** de cachear el cuadro.

## 3. Decisiones de implementación

| Decisión | Motivo |
|---|---|
| `GeometryStore::hitTest(index, x, y, tol, inside, distance)` | El almacén conoce sus dos modos y la disposición de los anillos; una interfaz estrecha evita copiar geometrías y el problema de que `acquireSubset` no devuelve una entrada por candidato en modo envolventes |
| `minigisgeometryhit` separado de `minigisidentify` | La prueba punto-anillo la usan el almacén y el camino OGR; separarla evita que `GeometryStore` dependa del motor de consulta |
| `IdentifyRequest.layers` en orden de apilado (fondo → techo) + `priorityLayer` | Es el orden que ya tiene el lienzo; el motor decide el orden visible (prioritaria primero, luego de arriba abajo) |
| La tolerancia llega al núcleo en unidades de mapa | El núcleo no conoce píxeles ni widgets; la GUI convierte con `mapUnitsPerPixel()` |
| `--identify-at` usa un `MapCanvas` (sin ventana), no `MainWindow` | Es una consulta de datos: no necesita docks ni carga en segundo plano, y así sirve en scripts y en `--snapshot` |
| `MainWindow::selectLayer()` para `--check-identify`, no solo para pruebas | Es la misma operación que hace el árbol; el chequeo la usa para simular la selección sin depender de la interfaz de usuario |
| `IdentifyPanel::rows()` y `headerText()` públicos | Permiten verificar el contenido del panel sin leer píxeles, igual que `layerTreeRows()` con el panel de capas |
| El marcador se dibuja después de `cache.image = image` | La caché se queda con la geometría limpia y el marcador no obliga a redibujar: es el patrón de las etiquetas |
| Umbral de clic de 3 px, recordando el desplazamiento **máximo** | Distingue clic de paneo aunque el botón se suelte en el punto de partida; el botón central y el doble clic no cambian |
| Atributos después de ordenar y recortar | Leer 256 candidatos para descartar 250 es I/O inútil |
| Topes y presupuesto visibles en `IdentifyResult` | Un clic en una capa enorme no puede devolver «nada» en silencio |
| Las comprobaciones 1–12 llaman al motor; 13–23 usan eventos reales | Permite construir y verificar la geometría antes del panel, sin perder la cobertura del clic y del alcance (spec §9.3) |
| Sin dependencias nuevas | Qt y GDAL ya están; el núcleo solo usa `QPointF`, `QElapsedTimer` y contenedores Qt |

## 4. Orden de implementación

Dependencias (flechas = «necesita antes»):

```
T1 motor + --identify-at texto (checks 1-7, 12)
 ├──▶ T2 capas sin índice / envolventes (8-9)
 │         │
 │         ▼
 ├──▶ T3 prioridad, apilado, recorte (10-11)
 │         │
 │         ├──────────────┐
 │         ▼              ▼
 └──▶ T4 clic, marcador, caché (20-22)   T5 panel (19, 23)
                     │                    │
                     └────────┬───────────┘
                              ▼
                    T6 dock, alcance y avisos (13-18)
                              │
                              ▼
                    T7 CLI JSON, opciones y capturas
                              │
                              ▼
                    T8 CTest y documentación
                              │
                              ▼
                    T9 verificación final (SC1-SC15)
```

### Fase A — Motor y geometría (T1–T3)

La consulta completa por CLI con texto: prueba exacta, huecos, radio, capas sin
índice, modo envolventes, orden, prioridad y recorte. Checkpoint A al cerrar T3.

### Fase B — Interacción (T4–T6)

Clic sin arrastre, marcador, panel, dock, alcance del árbol y avisos de capa
oculta y ráster. Checkpoint B al cerrar T6.

### Fase C — Contrato y cierre (T7–T9)

JSON, opciones de la CLI, capturas, pruebas de CTest, README y pasada final.
Checkpoints C y D.

## 5. Índice de tareas

Detalle completo (criterios, verificación, archivos y tamaño) en
[todo.md](todo.md).

| # | Tarea | Fase | Dep. | Tamaño |
|---|---|---|---|---|
| T1 | Núcleo de geometría, motor y `--identify-at` (texto) | A | — | M |
| T2 | Capas sin índice y modo envolventes | A | T1 | M |
| T3 | Orden, prioridad y recorte | A | T1, T2 | S |
| T4 | Clic sin arrastre, marcador y caché | B | T1 | M |
| T5 | Panel de resultados | B | T1, T3 | M |
| T6 | Dock, alcance del árbol y avisos | B | T4, T5 | M |
| T7 | CLI JSON, opciones y capturas | C | T3, T6 | M |
| T8 | CTest y documentación | C | T7 | S |
| T9 | Verificación final contra el spec | C | todas | S |

## 6. Riesgos y mitigaciones

| Riesgo | Impacto | Mitigación |
|---|---|---|
| El umbral de clic rompe el paneo o identifica tras un arrastre | Alto | Se recuerda el desplazamiento máximo en `mouseMoveEvent`; la comprobación 22 arrastra 20 px y exige que las filas del panel no cambien y que la extensión sí |
| El marcador queda horneado en la caché y se duplica o no se mueve | Alto | Se dibuja después de `cache.image = image` (como las etiquetas); las comprobaciones 20 y 21 miran el píxel y la reutilización del cuadro |
| En modo envolventes `acquireSubset` no es 1:1 con los candidatos | Alto | Se usa `hitTest`, que resuelve la lectura bajo demanda por índice; la comprobación 9 mide `features_read = 1` |
| Un polígono con huecos mal probado identifica el hueco | Alto | Paridad even-odd sobre todos los anillos; la comprobación 3 usa el hueco real de P-005 |
| La prueba por envolvente deja pasar elementos lejanos | Medio | Prueba exacta de distancia a segmentos; las comprobaciones 5 (4 px) y 6 (29 px) acotan el radio |
| El camino OGR sin almacén recorre una capa enorme | Medio | Filtro espacial (transformando la caja al SRC de la capa), tope de 256 candidatos y presupuesto de 250 ms con aviso |
| `syncLayerPanel()` reconstruye el árbol y pierde la selección | Medio | El panel ya recibe la entrada actual; las comprobaciones 13–16 hacen clic después de un cambio de visibilidad para forzar la reconstrucción |
| Simular ratón en offscreen da coordenadas distintas según el tamaño | Medio | Las expectativas se calculan con el `MapToPixel` real del lienzo; el tamaño se estabiliza procesando eventos, como en `--snapshot-window` |
| `fieldValues(row)` asume FID = fila | Bajo | Es el convenio ya usado por la tabla y el índice de envolventes; los datos de ejemplo lo cumplen (Madrid y P-001 son fid 0) y el camino OGR usa el FID de OGR |
| El JSON o los códigos de salida se desvían del contrato | Medio | `minigis.identificar-cli` compara claves y códigos; el JSON se documenta en el spec §8 |
| Se olvida añadir un archivo a `CMakeLists.txt` (listas explícitas) | Bajo | Cada tarea que crea un `.h/.cpp` actualiza la lista de su carpeta en la misma tarea |

## 7. Paralelización

* **En paralelo:** T4 (lienzo) y T5 (panel) tras T3, porque tocan archivos
  distintos; T8 (pruebas + README) se puede preparar mientras T7 estabiliza el
  JSON, pero se valida después.
* **Secuencial:** T1 → T2 → T3 (comparten `minigisidentify.cpp`); T4 + T5 → T6
  (comparten `MainWindow`/`MapCanvas`); T7 → T8 → T9.
* **Coordinación:** el contrato de `IdentifyResult` (T1) no cambia después de T3;
  si cambia, se actualizan panel y CLI en la misma tarea.

## 8. Puntos de control

| Checkpoint | Después de | Exige |
|---|---|---|
| A — Motor | T3 | Compila, 15/15 pruebas, `--check-identify` con las comprobaciones 1–12 en `OK` y ninguna `FALLO` |
| B — Interacción | T6 | Comprobaciones 13–23 en `OK`; un clic real abre el dock; arrastrar sigue paneando; 15/15 |
| C — Contrato | T8 | 18/18 en CTest; `--identify-at --json` cumple el esquema; la captura de la interfaz muestra panel y marcador |
| D — Cierre | T9 | SC1–SC15 verificados uno a uno; README y `--help` al día |

Cada checkpoint implica **revisión humana** antes de seguir con la fase siguiente
(salvo autorización expresa para continuar).

## 9. Preguntas abiertas para esta revisión

1. **`--identify-at` sin ventana:** propuesto construir un `MapCanvas` (no un
   `MainWindow`) para que la consulta sea rápida y headless; `--snapshot-window`
   sí usa la ventana cuando hay que rellenar el panel.
2. **API pública para las comprobaciones:** `IdentifyPanel::rows()`,
   `headerText()` y `MainWindow::selectLayer()` se exponen como parte del contrato
   de verificación (mismo patrón que `layerTreeRows()` y `waitForLoading()`), no
   como API de usuario.
3. **Datos de `--check-identify`:** se exigen las tres capas de ejemplo por `-l`
   (parcels, cities, ortho) y el chequeo falla con un mensaje accionable si los
   nombres no son los esperados; no busca archivos por su cuenta.
4. **Marcador:** cruz de 1 px en magenta puro dibujada en el píxel redondeado del
   clic; es lo que hace determinista la comprobación 20 en la plataforma
   `offscreen`.
5. **Tolerancia en píxeles de la comprobación 5:** se acepta el rango 3,5–5,0 px
   porque el clic se redondea al píxel entero antes de convertirlo a mapa (±0,5 px
   sobre los 4,2 px teóricos).
