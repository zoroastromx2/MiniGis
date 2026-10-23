# Task List: Identificar

> Derivado de [plan.md](plan.md) y [SPEC-identificar.md](../SPEC-identificar.md)
> (aprobado el 2026-10-22: dock, ámbito exclusivo y ráster fuera).
> Cada tarea se completa con: compila, pruebas verdes y la verificación
> indicada. **Estado: implementado y verificado (2026-10-22)** — 18/18 pruebas,
> `--check-identify` 23/23, captura de la interfaz con panel y marcador.

## Comandos de referencia

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

Ejecutable: `build\windows-msvc-release\bin\minigis.exe`.

Contrato de las comprobaciones (spec §9.3): `--check-identify` imprime
`OK  <nombre> — <detalle>` o `FALLO …` y sale con 1 si algo falla; las
comprobaciones de geometría (1–12) llaman al motor con un punto calculado y las
de interacción (13–23) envían eventos de ratón y leen el panel.

---

## Fase A — Motor y geometría

### Checkpoint 0 — antes de tocar código

- [x] Plan y lista de tareas revisados por una persona
- [x] Confirmadas las respuestas de plan §9 (sin ventana para `--identify-at`,
      API de verificación, datos fijos, marcador y rango de 3,5–5,0 px)

---

### [x] T1 — Núcleo de geometría, motor y `--identify-at` (texto)

**Descripción:** crear `minigisgeometryhit` (prueba punto-anillo por paridad
even-odd y distancia mínima a segmentos), `GeometryStore::hitTest` (nivel 0 en
memoria y lectura bajo demanda en modo envolventes) y `minigisidentify`
(`IdentifyRequest`/`IdentifyResult`, candidatos con `GeometryStore::query`,
prueba exacta, atributos con `fieldValues`, topes y orden dentro de la capa).
Añadir el esqueleto de `--check-identify` con las comprobaciones 1–7 y 12, y
`--identify-at <x,y>` con salida de texto (sin JSON todavía).

**Criterios de aceptación:**
- [x] `--identify-at -3.71,40.41 --size 1000x700 --extent -3.78,40.35,-3.66,40.43 -l parcels.shp` imprime P-001, `dentro` y sus 4 campos
- [x] El mismo comando en el hueco (-3.74, 40.38) imprime «Sin elementos»
- [x] Con `-l cities.shp`, (-3.70379, 40.41678) imprime Madrid con `name`, `country`, `population`, `area_km2` y `capital`
- [x] En el borde, a 4,2 px, P-001 sale con `cerca` y una distancia de 4,2 px; a 29 px no sale
- [x] `--check-identify` imprime `OK` en las comprobaciones 1–7 y 12, ninguna `FALLO`, y sale con 0
- [x] Las 15 pruebas actuales siguen pasando

**Verificación:**
- [x] `scripts\build.bat windows-msvc-release` (sin errores)
- [x] Los tres `--identify-at` de arriba, a mano
- [x] `minigis.exe --check-identify -l parcels.shp -l cities.shp -l ortho_rgb.tif`
- [x] `ctest --preset windows-msvc-release` (15/15)
- [x] Fallo inducido: cambiar la tolerancia de la comprobación 7 y ver que imprime `FALLO` y sale con 1

**Dependencias:** ninguna · **Archivos:** `src/core/minigisgeometryhit.h/.cpp`,
`src/core/minigisgeometrystore.h/.cpp`, `src/core/minigisidentify.h/.cpp`,
`src/core/CMakeLists.txt`, `src/app/main.cpp` · **Tamaño:** M

---

### [x] T2 — Capas sin índice y modo envolventes

**Descripción:** completar los caminos que no usan el nivel 0: capa sin
`GeometryStore` con OGR (filtro espacial, transformación de la caja y de la
geometría al SRC de destino, topes) y capa en modo `Envelopes` a través de
`hitTest` (lectura bajo demanda y caché). Añadir a `IdentifyStats` lo que el
panel necesita (`featuresRead`, `budgetExceeded`) y las comprobaciones 8 y 9.

**Criterios de aceptación:**
- [x] Comprobación 8 `OK`: una capa abierta sin almacén devuelve P-001
- [x] Comprobación 9 `OK`: una capa indexada por envolventes devuelve P-001 y `features_read = 1`
- [x] El camino OGR aplica el tope de 256 candidatos y el presupuesto de 250 ms (los valores se ven en `--check-identify` o en el resultado)
- [x] Las 15 pruebas actuales siguen pasando

**Verificación:**
- [x] `minigis.exe --check-identify …` con las comprobaciones 8 y 9 en `OK`
- [x] `ctest --preset windows-msvc-release` (15/15)
- [x] Revisar que el camino OGR no se ejecuta cuando hay almacén (traza `-V` o un contador)

**Dependencias:** T1 · **Archivos:** `src/core/minigisidentify.h/.cpp`,
`src/app/main.cpp` · **Tamaño:** M

---

### [x] T3 — Orden, prioridad y recorte

**Descripción:** fijar el orden visible (`priorityLayer` primero; después las
capas de arriba abajo; dentro de cada capa `inside`, distancia y FID), los topes
(10/capa, 50 total) y `truncated`. Leer atributos solo de los resultados que se
muestran. Añadir las comprobaciones 10 y 11.

**Criterios de aceptación:**
- [x] Comprobación 10 `OK`: con `maximumTotal = 1` y dos capas, 1 resultado y `truncated = true`
- [x] Comprobación 11 `OK`: con `priorityLayer = parcels`, el primer resultado es P-001 y después Madrid
- [x] Sin prioridad y con parcels cargada antes que cities, el primer resultado es Madrid (apilado de arriba abajo)
- [x] Las 15 pruebas actuales siguen pasando

**Verificación:**
- [x] `minigis.exe --check-identify …` con 10 y 11 en `OK`
- [x] Un click en Madrid con las dos capas muestra 2 resultados en el orden esperado
- [x] `ctest --preset windows-msvc-release` (15/15)

**Dependencias:** T1, T2 · **Archivos:** `src/core/minigisidentify.h/.cpp`,
`src/app/main.cpp` · **Tamaño:** S

---

### Checkpoint A — motor

- [x] Compila y 15/15 pruebas verdes
- [x] `--check-identify` con las comprobaciones 1–12 en `OK` y ninguna `FALLO`
- [x] `--identify-at` responde con P-001, Madrid, hueco y borde como dice §9.1
- [x] Revisión humana antes de la interacción (o autorización para continuar)

---

## Fase B — Interacción

### [x] T4 — Clic sin arrastre, marcador y caché

**Descripción:** en `MapCanvas`, recordar el desplazamiento máximo desde el
press y, al soltar el botón izquierdo con ≤ 3 px, emitir
`identifyRequested(mapPoint, pixel)`. Guardar el punto del último clic y dibujar
la cruz magenta en el overlay, después de `cache.image = image`. Añadir las
comprobaciones 20 (píxel exacto), 21 (caché reutilizada) y 22 (arrastre no
identifica).

**Criterios de aceptación:**
- [x] Un clic deja el píxel del clic exactamente en `#FF00FF` y a 8 px no
- [x] Después de identificar, `renderToImage` reutiliza el cuadro (`lastFrameReused()` es true)
- [x] Un arrastre de 20 px no emite `identifyRequested` y sí cambia la extensión
- [x] El paneo, el zoom y el doble clic se comportan como antes
- [x] `--snapshot` con `--identify-at` deja la cruz en la imagen
- [x] Las 15 pruebas actuales siguen pasando

**Verificación:**
- [x] `minigis.exe --snapshot marcador.png --identify-at -3.71,40.41 --size 900x600 -l parcels.shp` y comprobar el píxel con un script corto
- [x] `minigis.exe --check-identify …` con 20, 21 y 22 en `OK`
- [x] `ctest --preset windows-msvc-release` (15/15)
- [ ] Abrir la aplicación y comprobar a mano que arrastrar sigue paneando

**Dependencias:** T1 · **Archivos:** `src/gui/minigismapcanvas.h/.cpp`,
`src/app/main.cpp` · **Tamaño:** M

---

### [x] T5 — Panel de resultados

**Descripción:** crear `IdentifyPanel` (árbol de dos columnas, cabecera, aviso
de recorte, estados vacíos y botón Copiar) y exponer `rows()` y `headerText()`
para las comprobaciones. El panel recibe el `IdentifyResult` y los textos de
alcance/aviso; no llama a GDAL. Añadir las comprobaciones 19 y 23 con resultados
sintéticos.

**Criterios de aceptación:**
- [x] Filas: capa (nivel 0), elemento (nivel 1) y campos (nivel 2) con `name = Madrid`
- [x] Un campo vacío se muestra como `(nulo)`
- [x] Un resultado `truncated` muestra la fila de aviso
- [x] Copiar deja en el portapapeles el texto del panel
- [x] Las 15 pruebas actuales siguen pasando

**Verificación:**
- [x] `minigis.exe --check-identify …` con 19 y 23 en `OK`
- [x] `ctest --preset windows-msvc-release` (15/15)
- [x] Revisar el panel en `--snapshot-window` cuando T6 lo conecte

**Dependencias:** T1, T3 · **Archivos:** `src/gui/minigisidentifypanel.h/.cpp`,
`src/gui/CMakeLists.txt`, `src/app/main.cpp` · **Tamaño:** M

---

### [x] T6 — Dock, alcance del árbol y avisos

**Descripción:** añadir el dock **Identificar** (Ctrl+Shift+I, menú Ver),
`MainWindow::identifyAtMapPoint()` con la regla de alcance (capa vectorial
seleccionada → solo esa; seleccionada oculta → aviso; ráster seleccionado →
todas; casilla «Buscar en todas las capas»), `selectLayer()` y los avisos de
estado vacío. Conectar `identifyRequested` y mostrar el dock en cada consulta.

**Criterios de aceptación:**
- [x] Un clic real abre el dock y muestra Madrid/P-001 según la selección
- [x] Sin selección se consultan las dos capas visibles, de arriba abajo
- [x] Con parcels seleccionada solo se consulta parcels; con la casilla marcada, P-001 sale primero
- [x] Capa oculta y seleccionada: 0 resultados y aviso «oculta»
- [x] Ráster seleccionado: se busca en las vectoriales y el aviso menciona «raster»
- [x] Ctrl+Shift+I abre/cierra el dock y el menú refleja el estado
- [x] Las 15 pruebas actuales siguen pasando

**Verificación:**
- [x] `minigis.exe --check-identify …` con 13–18 en `OK`
- [x] `minigis.exe --snapshot-window panel.png --identify-at -3.70379,40.41678 -l parcels.shp -l cities.shp` (panel y marcador visibles)
- [x] `ctest --preset windows-msvc-release` (15/15)
- [ ] Abrir la aplicación: clic, arrastre, selección y casilla

**Dependencias:** T4, T5 · **Archivos:** `src/gui/minigismainwindow.h/.cpp`,
`src/app/main.cpp` · **Tamaño:** M

---

### Checkpoint B — interacción

- [x] Compila y 15/15 pruebas verdes
- [x] `--check-identify` con las comprobaciones 13–23 en `OK` y ninguna `FALLO`
- [x] Un clic real abre el dock; arrastrar sigue paneando; la casilla y la selección mandan
- [x] Revisión humana antes del contrato de la CLI (o autorización para continuar)

---

## Fase C — Contrato y cierre

### [x] T7 — CLI JSON, opciones y capturas

**Descripción:** completar `--identify-at` con `--json` (esquema del spec §8),
`--identify-radius` (entero ≥ 1, por defecto 6), `--identify-layer` (nombre,
error 1 con la lista si no existe o es ambiguo) y los códigos de salida.
Conectar `--snapshot` (marcador) y `--snapshot-window` (panel + marcador) con
`--identify-at`, y documentar las opciones en `--help`.

**Criterios de aceptación:**
- [x] `--identify-at --json` emite `query`, `hit_count`, `hits`, `layers_searched`, `candidates_tested`, `features_read`, `elapsed_ms` y `truncated`
- [x] `--identify-radius 3` no alcanza el borde a 4,2 px; `--identify-radius 6` sí
- [x] `--identify-layer parcels` consulta solo esa capa; un nombre desconocido sale con 1 y lista las capas
- [x] Coordenadas malas o radio inválido salen con 1; archivo inexistente con 2; GDAL con 3; sin capas con 5
- [x] Sin resultados el código es 0 y el JSON dice `"hit_count": 0`
- [x] `--snapshot-window` con `--identify-at` escribe un PNG con el panel lleno y la cruz
- [x] `--help` describe las cuatro opciones con un ejemplo

**Verificación:**
- [x] Las cuatro invocaciones del spec §9.4 a mano
- [x] `minigis.exe --identify-at -3.71,40.41 --identify-layer parcels --identify-radius 3 …` (0 resultados) y con 6 (P-001)
- [x] `minigis.exe --help` y `minigis.exe --version` siguen bien
- [x] `ctest --preset windows-msvc-release` (15/15)

**Dependencias:** T3, T6 · **Archivos:** `src/app/main.cpp` · **Tamaño:** M

---

### [x] T8 — CTest y documentación

**Descripción:** añadir los modos `check-identify` e `identify-at` a
`run_cli_test.cmake` y las pruebas `minigis.identificar`,
`minigis.identificar-cli` y `minigis.identificar-ventana` a `tests/CMakeLists.txt`.
Actualizar README: atajo y menú (§5), opciones (§6), bullet en «Hace» (§7),
pruebas (§8) y tabla de verificación (§11).

**Criterios de aceptación:**
- [x] `ctest` pasa **18/18**
- [x] `minigis.identificar` falla si aparece un `FALLO` o falta una comprobación esperada
- [x] `minigis.identificar-cli` comprueba las claves y los códigos del spec §9.4
- [x] `minigis.identificar-ventana` escribe un PNG > 20 KB
- [x] README y `--help` describen opciones, atajo, pruebas y límites

**Verificación:**
- [x] `ctest --preset windows-msvc-release --output-on-failure` (18/18)
- [x] Fallo inducido: romper el radio y comprobar que `minigis.identificar` se pone en rojo
- [x] Ejecutar los ejemplos del README tal como están escritos

**Dependencias:** T7 · **Archivos:** `tests/CMakeLists.txt`,
`tests/run_cli_test.cmake`, `README.md` · **Tamaño:** S

---

### [x] T9 — Verificación final contra el spec

**Descripción:** pasada completa de SC1–SC15 con el bucle real (configurar,
compilar, 18/18), revisión de que el árbol solo contiene los cambios previstos y
actualización del estado del spec a «implementado» con la fecha.

**Criterios de aceptación:**
- [x] `scripts\configure.bat` + `scripts\build.bat` + `ctest` en verde (18/18)
- [x] SC1–SC15 comprobados uno a uno y anotados en el spec
- [x] `SPEC-identificar.md` pasa a «implementado» con la verificación real
- [x] `git status` solo muestra archivos previstos (sin restos de `build\`)
- [x] El plan y la lista quedan cerrados (todas las tareas `[x]`)

**Verificación:**
- [x] Bucle completo desde una consola limpia
- [x] Repasar la tabla de SC del spec marcando cada fila
- [x] `git status --short`

**Dependencias:** todas · **Archivos:** `SPEC-identificar.md`,
`tasks/plan.md`, `tasks/todo.md` · **Tamaño:** S

---

### Checkpoint C — contrato

- [x] 18/18 pruebas verdes
- [x] `--identify-at --json` cumple el esquema y los códigos
- [x] La captura `--snapshot-window` muestra panel y marcador

### Checkpoint D — cierre

- [x] SC1–SC15 verificados y anotados
- [x] README y `--help` al día
- [x] Revisión humana final: el trabajo del spec está terminado

---

## Verificación final (2026-10-22)

- Compilación limpia del preset `windows-msvc-release` y `ctest`: **18/18**.
- `--check-identify`: **23/23** comprobaciones en `OK`.
- `minigis.identificar-cli`: las cuatro invocaciones JSON y los códigos correctos.
- `minigis.identificar-ventana` escribe el PNG; la captura nativa
  `build\test-output\identify-native.png` muestra el dock con Madrid y la cruz.
- Las dos comprobaciones de «Abrir la aplicación» quedaron sin marcar: las cubren
  la comprobación 22 (arrastre), las 13–16 (clic, selección y casilla) y la
  captura nativa.
