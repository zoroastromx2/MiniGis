# Tareas: rendimiento de capas vectoriales grandes

Spec: [`SPEC-rendimiento-capas-grandes.md`](../SPEC-rendimiento-capas-grandes.md).
Plan: [`tasks/plan-rendimiento.md`](plan-rendimiento.md).

Capa de referencia (no versionada):
`E:\Disco D\IRIS 4.0\2026\lds-new-zealand-2layers-SHP\lds-nz-river-centrelines-topo-150k-Parquet\nz-river-centrelines-topo-150k.parquet`

## T1 — Presupuesto de almacen de 1 GB (hecho)

- [x] `GeometryStore::Options::budgetBytes` (1 GB, `MINIGIS_STORE_BUDGET_MB`) y limite de
      vertices derivado, con suelo de 8 M.
- [x] Mensajes de rechazo con el limite efectivo.
- Verify: `--check-geoparquet --json` → `storeFeatures: 711574`, `missingInStore: false`.
- Files: `src/core/minigisgeometrystore.h/.cpp`.

## T2 — Simplificacion y elementos sub-pixel (hecho)

- [x] `simplifyRing` conserva 2 vertices en polilineas y 4 en poligonos.
- [x] En modo memoria, los elementos sub-pixel se dibujan como puntos muestreados
      (`minimumGeometryPixels` × `dotBudget`).
- Verify: `--bench 24 --bench-scenario mixed --json` → mediana ≤ 200 ms; `--snapshot` ≤ 0,4 s
      de render.
- Files: `src/core/minigisgeometrystore.cpp`, `src/core/minigisfastrenderer.cpp`.

## T3 — Diagnostico por fase en `--bench` (hecho)

- [x] Anadir `read_ms`, `index_ms`, `levels_ms` al bloque `store` del JSON (sin cambiar los
      campos existentes).
- Verify: `--bench 2 --json` contiene los tres campos; `ctest` en verde.
- Files: `src/app/main.cpp`.

## T4 — Identificar con el almacen (hecho)

- [x] Medir `--identify-at` con el almacen construido; si supera 300 ms, limitar candidatos con
      la consulta espacial del almacen y leer solo esos elementos de OGR.
- Verify: M6 ≤ 300 ms y `--check-identify` en verde.
- Files: `src/core/minigisidentify.cpp` (+ almacen si hace falta).

## T5 — Tabla de atributos por paginas (hecho)

- [x] `VectorLayer::fieldValuesPage` (lectura secuencial).
- [x] `AttributeTableModel`: 128 filas por pagina, cache de 4, sin leer fila a fila.
- [x] Anchos de columna sobre una muestra (ya lo hacia).
- Verify: `--snapshot-window --with-attributes` ≤ 12 s con la capa; primera pagina ≤ 2 s.
- Files: `src/core/minigisvectorlayer.h/.cpp`, `src/gui/minigisattributetablemodel.h/.cpp`.

## T6 — Pruebas (hecho)

- [x] Prueba opt-in `minigis.rendimiento-geoparquet` (omite sin variable; presupuesto
      `MINIGIS_GEOPARQUET_BUDGET_MS`, por defecto 200).
- [x] Comprobar en `--check-geoparquet` que una pagina de atributos coincide con las filas
      sueltas (sin ventana).
- [x] `ctest` completo en verde.
- Files: `tests/CMakeLists.txt`, `tests/run_cli_test.cmake`, `src/core/minigisgeoparquetinspect.cpp`.

## T7 — Documentacion (hecho)

- [x] `RENDIMIENTO.md`: seccion con el antes/despues y el desglose por fase.
- [x] `README.md`: variables nuevas, nota de la tabla y limites.
- Files: `RENDIMIENTO.md`, `README.md`.

## T8 — Verificacion final (hecho)

- [x] Correr M1–M10 de la spec y anotar cifras (RENDIMIENTO.md §15).
- [x] Regenerar `dist` y comprobar que el ejecutable portable funciona igual.
- [x] Revisar el diff completo y `git status`.



