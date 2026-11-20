# Task List: Selección del objeto identificado

> Derivado de [plan.md](plan.md) y [SPEC-seleccion.md](../SPEC-seleccion.md)
> (aprobado el 2026-10-22: primer resultado, tabla cambia de capa sin tocar el
> árbol, halo + amarillo, tabla oculta no se abre, sin selección desde el panel,
> sin `Esc`, el panel marca «(seleccionado)»).
> Cada tarea se completa con: compila, 18/18 pruebas verdes y la verificación
> indicada. **Estado: implementado y verificado (2026-10-22)** — 18/18 pruebas,
> comprobaciones 24–31 de `--check-identify` en `OK`, `selection` en el JSON y
> captura nativa con resalte y tabla.

## Comandos de referencia

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

Ejecutable: `build\windows-msvc-release\bin\minigis.exe`.

Contrato: `--check-identify` mantiene las 23 comprobaciones actuales y añade las
24–31 de §8.2 del spec. Las comprobaciones de geometría/CLI llaman al motor; las
de interacción envían eventos y leen el panel y la tabla.

---

## Fase A — Resalte visible

### Checkpoint 0 — antes de tocar código

- [x] Plan y lista de tareas revisados por una persona
- [x] Confirmado el orden de overlays (etiquetas → selección → marcador) y los
      lápices cosméticos

---

### [x] T1 — Núcleo y lienzo: anillos, selección y resalte

**Descripción:** añadir `GeometryStore::featureRings()` (copia del nivel 0 y, en
modo envolventes, de la caché) y el estado de selección en `MapCanvas`:
`setSelectedFeature`, `clearSelectedFeature`, accesores, trazado cacheado en
coordenadas de mapa y overlay con halo oscuro (1 px) + amarillo `#FFD400` (3 px),
dibujado después de la caché y antes del marcador. Puntos con anillo de 6 px. Se
limpia al quitar la capa, al vaciar capas y al cambiar el SRC de destino; una capa
oculta no se dibuja. Añadir las comprobaciones 25 y 30.

**Criterios de aceptación:**
- [x] `featureRings` devuelve los anillos del elemento en memoria y en modo
      envolventes, y `false` si no hay geometría
- [x] Con P-001 seleccionado se ve `#FFD400` a ±1 px del borde superior y no se
      veía antes de seleccionar
- [x] `renderToImage` posterior a la selección reutiliza el cuadro
      (`lastFrameReused()`)
- [x] Al quitar la capa seleccionada, `hasSelectedFeature()` pasa a false
- [x] Una capa oculta no dibuja el resalte
- [x] Las 18 pruebas actuales siguen pasando

**Verificación:**
- [x] `scripts\build.bat windows-msvc-release`
- [x] `minigis.exe --check-identify -l parcels -l cities -l ortho` con 25 y 30 en `OK`
- [x] `ctest --preset windows-msvc-release` (18/18)
- [x] Revisar una captura con P-001 seleccionado

**Dependencias:** ninguna · **Archivos:** `src/core/minigisgeometrystore.h/.cpp`,
`src/gui/minigismapcanvas.h/.cpp`, `src/app/main.cpp` · **Tamaño:** M

---

### Checkpoint A — resalte

- [x] Compila y 18/18 pruebas verdes
- [x] Comprobaciones 25 y 30 en `OK`; el resalte se ve con el color y el grosor
      previstos
- [x] Revisión humana antes de la integración (o autorización para continuar)

---

## Fase B — Integración

### [x] T2 — Identificar selecciona; tabla y panel

**Descripción:** en `identifyAtMapPoint`, seleccionar `hits.front()` y limpiar en
un clic sin resultados. Si el dock de atributos está visible, poner el modelo en
la capa del objeto (sin tocar el árbol), seleccionar la fila `fid` y centrarla;
sin foco en la tabla. Añadir los accesores de verificación y el sufijo
«(seleccionado)» en el panel. Comprobar el aviso del tope de 100 000 filas.

**Criterios de aceptación:**
- [x] Tras el clic en P-001, la selección es `parcels`/fid 0 y la capa del árbol
      no cambia
- [x] Un clic en el hueco limpia la selección y el resalte
- [x] Con la tabla visible en parcels, un clic en P-003 deja la fila actual en 2
- [x] Con la tabla visible en parcels, un clic en Madrid cambia a cities y fila 0
- [x] Con la tabla oculta, un clic en Madrid no cambia su capa ni su fila
- [x] El panel marca la fila del elemento con «(seleccionado)»
- [x] Las 18 pruebas actuales siguen pasando

**Verificación:**
- [x] `minigis.exe --check-identify …` con 24, 26–29 y 31 en `OK`
- [x] `ctest --preset windows-msvc-release` (18/18)
- [x] `minigis.exe --snapshot-window s.png --identify-at …` (resalte, panel y fila)

**Dependencias:** T1 · **Archivos:** `src/gui/minigismainwindow.h/.cpp`,
`src/gui/minigisidentifypanel.h/.cpp`, `src/app/main.cpp` · **Tamaño:** M

---

### Checkpoint B — integración

- [x] Compila y 18/18 pruebas verdes
- [x] Comprobaciones 24, 26–29 y 31 en `OK`
- [x] La tabla salta a la fila y el panel marca el elemento seleccionado
- [x] Revisión humana antes del contrato (o autorización para continuar)

---

## Fase C — Contrato y cierre

### [x] T3 — CLI, JSON, capturas y CTest

**Descripción:** añadir `"selection"` al JSON de `--identify-at` (o `null` sin
resultados), aplicar la selección en `--snapshot` y `--snapshot-window` con
`--identify-at` mediante un ayudante compartido, y ampliar
`tests/run_cli_test.cmake` con las etiquetas 24–31 y las comprobaciones del JSON.
No se añaden pruebas nuevas: `minigis.identificar` y `minigis.identificar-cli`
crecen.

**Criterios de aceptación:**
- [x] `--identify-at --json` incluye `"selection"` con la capa y el FID, y `null`
      cuando no hay resultados
- [x] `--snapshot` y `--snapshot-window` con `--identify-at` muestran el resalte
      (y el panel en la ventana)
- [x] `minigis.identificar` exige las etiquetas 24–31 y falla con cualquier
      `FALLO`
- [x] `minigis.identificar-cli` comprueba `selection` y su `null`
- [x] `ctest` 18/18

**Verificación:**
- [x] Las cuatro invocaciones de `--identify-at` del spec
- [x] `ctest --preset windows-msvc-release -R "identificar" --output-on-failure`
- [x] `ctest --preset windows-msvc-release` (18/18)

**Dependencias:** T2 · **Archivos:** `src/app/main.cpp`,
`tests/run_cli_test.cmake` · **Tamaño:** M

---

### [x] T4 — README y verificación final

**Descripción:** documentar la selección, el resalte, el salto de la tabla y la
clave `selection`; actualizar §5, §7, §8 y §11 del README. Pasar
`SPEC-seleccion.md` a «implementado» con la verificación real y cerrar las tareas.

**Criterios de aceptación:**
- [x] README describe el comportamiento y los comandos tal como funcionan
- [x] `SPEC-seleccion.md` pasa a «implementado» con 18/18 y las comprobaciones
      24–31
- [x] SC1–SC13 comprobados uno a uno
- [x] `git status` solo muestra archivos previstos (sin restos de `build\`)

**Verificación:**
- [x] Bucle completo (configurar, compilar, `ctest`)
- [x] Ejecutar los ejemplos del README
- [x] `git status --short`

**Dependencias:** T3 · **Archivos:** `README.md`, `SPEC-seleccion.md`,
`tasks/todo.md` · **Tamaño:** S

---

### Checkpoint C — contrato

- [x] 24–31 en `OK`, `selection` en el JSON y 18/18
- [x] Captura con resalte, panel y fila de la tabla

### Checkpoint D — cierre

- [x] SC1–SC13 verificados y anotados
- [x] README y spec al día
- [x] Revisión humana final: el trabajo del spec está terminado

---

## Verificación final (2026-10-22)

- Compilación limpia del preset `windows-msvc-release` y `ctest`: **18/18**.
- `--check-identify`: **31/31** comprobaciones en `OK` (24–31 de selección,
  resalte, limpieza, tabla y panel).
- `minigis.identificar-cli`: `selection` con capa y FID, y `null` en el hueco.
- `minigis.identificar-ventana` y la captura nativa
  `build\test-output\selection-native.png`: Madrid seleccionado con anillo
  amarillo, «(seleccionado)» en el panel y fila 0 de la tabla resaltada.
- La tabla oculta no se abre ni se mueve (comprobación 29).
