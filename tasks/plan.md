# Implementation Plan: apertura y visualización de KML/KMZ

## Overview

Convertir el soporte KML de MiniGIS, que hoy funciona por delegación afortunada en
GDAL, en un contrato explícito y probado: clasificar la geometría **elemento a
elemento** en capas de tipo desconocido, no perder ningún elemento en silencio,
aplicar los **estilos del KML** (color y grosor) por capa, mostrar un tipo de
geometría legible en el panel, ofrecer `.kmz` en el diálogo y verificar todo con
un modo `--check-kml` sin ventana.

Spec de referencia: [`SPEC-kml.md`](../SPEC-kml.md). Tareas: `tasks/todo.md`.

## Architecture Decisions

* **D1 — Clasificar por elemento, no por capa.** `LIBKML` declara siempre
  `wkbUnknown`; cualquier decisión basada en el tipo de capa es una suposición.
  La clasificación por elemento entra solo en la ruta de capas de tipo
  desconocido: las capas con tipo conocido (shapefile, GeoPackage, GeoJSON
  homogéneo) siguen la ruta actual sin cambios, para no arriesgar la puerta de
  rendimiento.
* **D2 — Prohibido descartar por tipo.** El almacén recorre colecciones
  (`MultiGeometry`) y cuenta las partes no soportadas en vez de tirar el
  elemento entero. El caso que hoy falla (una `MultiGeometry` junto a otro
  polígono) es la prueba de regresión.
* **D3 — Los estilos los resuelve GDAL.** `LIBKML_RESOLVE_STYLE=YES` +
  `OGR_F_GetStyleString` en lugar de un lector de `<Style>` propio: un solo
  recorrido del XML, resolución de `styleUrl` y `StyleMap` incluida, y style
  strings como superficie estable y documentada.
* **D4 — Estilo por elemento, agrupado por estilo.** El almacén guarda un índice
  de estilo por elemento y una tabla de estilos únicos (tope 64); el renderizador
  agrupa el dibujado por estilo, no por elemento, para que el número de
  `QPen`/`QBrush` por cuadro siga siendo pequeño. El estado de estilo entra en
  `revision()`, así que la caché de cuadro sigue siendo válida.
* **D5 — `--check-kml` compara las dos orillas.** Informa lo que entrega OGR y
  lo que el almacén construyó; si no coinciden, `ok: false`. Así la prueba
  detecta la pérdida de elementos sin mirar píxeles.
* **D6 — Sin archivos de terceros en `ctest`.** Fixtures propios, pequeños y
  versionados; el KML de 21,6 MB del usuario solo se usa para medir a mano.

## Dependency graph

```
T1 fixtures ──┬── T2 --check-kml (rojo) ── T3 clasificacion por elemento (verde)
              │                                   │
              │                                   ├── T6 panel (tipo + estilos)
              │                                   │
              ├── T4 estilos desde GDAL ── T5 estilo dominante por capa
              │                                   │
              └── T7 dialogo *.kmz ───────────────┴── T8 documentacion + medicion
```

T3 y T4 son independientes entre sí y pueden ir en paralelo. T5 necesita T3 y
T4. T8 cierra.

## Task List

### Phase 1: Fundación verificable

- [x] T1 — Fixtures KML/KMZ deterministas en `data/sample/kml/`
- [x] T2 — `--check-kml`: recuento por elemento y comparación con el almacén
- [x] T3 — Clasificación por elemento en el almacén de geometrías

### Checkpoint: después de T1–T3

- [ ] `ctest --preset windows-msvc-release` en verde
- [ ] `--check-kml data\sample\kml\mixto.kml --json` devuelve `ok: true` y `unsupportedParts: 0`
- [ ] `--snapshot` de `mixto.kml` muestra los dos polígonos, la línea y el punto
- [ ] Revisión con el humano antes de seguir

### Phase 2: Estilos

- [x] T4 — Resolver estilos del KML con GDAL y convertirlos a `Style`
- [x] T5 — Tabla de estilos e índice por elemento en el almacén
- [x] T6 — Render agrupado por estilo y medición contra la puerta de rendimiento

### Checkpoint: después de T4–T6

- [ ] `kml-estilos`, `kml-estilos-varios` y `kml-estilos-sin` en verde
- [ ] `estilos_varios.kml` dibuja cada elemento con su color
- [ ] `ctest -R minigis.rendimiento` en verde y el KML de 21,6 MB no empeora más de un 20 %

### Phase 3: Interfaz y cierre

- [x] T7 — Panel: tipo de geometría legible y número de estilos
- [x] T8 — Diálogo y arrastrar-soltar con `*.kmz`; errores de KML con causa y acción
- [x] T9 — Documentación, medición final y actualización de la spec

### Checkpoint: completo

- [ ] Los ocho criterios de éxito de `SPEC-kml.md` §11, uno por uno
- [ ] `ctest` completo en verde, incluida `minigis.rendimiento`
- [ ] `--info --json` y `--snapshot` de un shapefile, un GeoPackage y un GeoJSON sin diferencias con `main`

## Verification commands

```bat
scripts\build.bat windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
bin\minigis.exe --check-kml data\sample\kml\mixto.kml --json
bin\minigis.exe --snapshot mixto.png -l data\sample\kml\mixto.kml --size 900x600
bin\minigis.exe --info "D:\F\IRIS 4.0\2026\KML\world-country-boundaries.kml"
```

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| `LIBKML_RESOLVE_STYLE` ralentiza la apertura de KML grandes (resuelve estilo por elemento) | Medio | Medir con el archivo de 21,6 MB en T4; si supera el presupuesto, cachear por `styleUrl` resuelto o activar la resolución solo si el documento tiene tabla de estilos |
| El arreglo de clasificación cambia el comportamiento de capas ya soportadas | Alto | La ruta nueva se activa **solo** cuando el tipo declarado es desconocido; T3 incluye comparar `--info --json` y `--snapshot` de shapefile, GPKG y GeoJSON contra `main` |
| El formato del *style string* de OGR varía (`w:4px` frente a `w:4`, mayúsculas, orden de claves) | Bajo | Parser tolerante y pruebas con las variantes documentadas (`PEN(c:#rrggbb,w:Npx)`, `BRUSH(fc:#rrggbb,fo:N)`) |
| Las pruebas de CTest necesitan un binario ya compilado en el preset | Bajo | Seguir el patrón existente de `tests/run_cli_test.cmake` con `$<TARGET_FILE:minigis>` |
| Fixtures con datos de terceros | Bajo | Solo fixtures propios; el KML del usuario no se versiona |
| El contrato JSON de `--check-kml` evoluciona durante la implementación | Bajo | Campos aditivos; los existentes no cambian de nombre ni de tipo |

## Open Questions

* **Q7 (spec) — estilo por elemento.** Aprobado como incremento posterior. Si en
  la revisión de este plan se quiere dentro del alcance, T5 cambia de tamaño M a
  L y necesita una medición previa contra `minigis.rendimiento`.
* **Tope de anchura de trazo.** Se propone 20 px. Si el usuario prefiere otro
  (o ninguno), se fija en T4.
* **`--check-kml` en la tabla de la CLI del README.** Se añade en T8 junto con el
  resto de la documentación.
