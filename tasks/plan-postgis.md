# Implementation Plan: conexión a PostgreSQL/PostGIS

## Overview

Convertir PostGIS en una **fuente más** del motor vectorial existente: el
`pg:conexion/esquema/tabla` se abre con el mismo `VectorLayer`, se dibuja con el
mismo renderizador y se diagnostica con los mismos modos sin ventana. La
novedad está en el **catálogo de conexiones** (`Connections.xml`), en la
**política de credenciales** (DPAPI + `PGPASSWORD`) y en un **diálogo** que
convierte una base de datos de miles de tablas en una selección concreta.

Spec de referencia: [`SPEC-postgis.md`](../SPEC-postgis.md). Tareas:
`tasks/todo-postgis.md`.

## Architecture Decisions

* **D1 — Un solo embudo.** `openVectorDataset()` reconoce el prefijo `pg:` y
  construye la cadena `PG:...` con la contraseña; el resto del motor (atributos,
  identificar, etiquetas, render, almacén) no se entera de que la fuente no es un
  archivo. No hay una segunda clase de capa.
* **D2 — Identidad separada de secreto.** `PgSource` (URI `pg:conexion/esquema/
  tabla`) es lo que se guarda, se muestra y se compara; `PgConnection` +
  contraseña solo viven en memoria durante la apertura. `VectorLayer::sourcePath()`
  devuelve la URI, nunca la cadena con contraseña.
* **D3 — Credenciales perezosas y thread-safe.** `PgCredentials` resuelve la
  contraseña en el hilo que abre la capa (el del cargador): caché de sesión,
  luego DPAPI, luego libpq (`PGPASSWORD`/`.pgpass`). Un `QMutex` protege la
  caché; no se pide contraseña desde un hilo de trabajo.
* **D4 — Catálogo por esquema.** Una apertura global solo enumera esquemas
  (5 121 capas, 70 ms medidos); cada esquema se abre aparte (16 capas, 55 ms).
  El diálogo nunca abre la base entera como capas.
* **D5 — «Dibujar bajo demanda» explícito.** `LayerRequest` gana
  `buildStore`; el diálogo y `--pg-no-index` lo ponen a `false` para tablas
  enormes. El renderizador ya sabe dibujar sin almacén (camino clásico).
* **D6 — Un solo punto de verdad para «existe».** `sourceExists()` sustituye a
  los `QFileInfo::exists` de la CLI y el GUI cuando la fuente es `pg:`; el resto
  de modos (`--info`, `--snapshot`, `--identify-at`, `--bench`) entra por el
  mismo `collectLayerPaths` sin ramas nuevas.
* **D7 — Pruebas sin servidor obligatorio.** Unitarias propias para parseo,
  URI, cadena y saneado; integración opt-in por variables de entorno con
  `SKIP_RETURN_CODE 77`. El `bdginegi` no entra en CI.
* **D8 — El diálogo no sabe GDAL.** `PostgisDialog` consume
  `PgConnectionCatalog` y `PgCatalog` (core), y devuelve una lista de
  selecciones; `MainWindow` las mete en el cargador como cualquier archivo.

## Dependency graph

```
T1 PgSource ──┐
T2 Connections┴── T3 cadena + credenciales ── T4 catálogo ── T7 --check-pg
                                                   │
                                                   ├── T5 VectorLayer ── T6 CLI informe/captura
                                                   │                         │
                                                   │                         └── T8 --bench
                                                   └── T9 diálogo ── T10 integración GUI
                                                                             │
T11 documentación y cierre ◄─────────────────────────────────────────────────┘
```

T1 y T2 son independientes. T5 necesita T1 y T3. T9 necesita T4. T6, T7 y T8
pueden ir en paralelo tras T5/T4. T10 necesita T5 y T9. T11 cierra.

## Task List

### Phase 1: Fundación (sin red)

- [x] T1 — `PgSource`: URI `pg:`, cadena de conexión y `scrubSecrets`, con unitarias
- [x] T2 — Catálogo `Connections.xml` (ISO-8859-1, solo `PGIS`) y resolución de ruta
- [x] T3 — Credenciales: DPAPI + caché de sesión + orden de resolución
- [x] T4 — Catálogo PostGIS: esquemas, tablas, geometría, elementos y SRID

### Checkpoint: después de T1–T4

- [x] `ctest -R minigis.pg` en verde (unitarias y casos sin servidor)
- [x] El fixture `Connections.xml` lista 2 conexiones y descarta la `GPKG`
- [ ] Revisión con el humano antes de tocar `VectorLayer`

### Phase 2: Motor y CLI

- [x] T5 — `VectorLayer` y `detectLayerKind` aceptan `pg:`; metadatos de conexión
- [x] T6 — `--connections`, `--info`/`--snapshot`/`--identify-at` con `pg:`, exit 8
- [x] T7 — `--check-pg` (texto y JSON) y `--snapshot-pg-dialog`
- [x] T8 — `--bench` con `pg:` y `--pg-no-index`

### Checkpoint: después de T5–T8

- [x] `--check-pg -l "pg:dataware_FDAC/mgn_2025" --json` devuelve las 16 tablas
- [x] `--info` y `--snapshot` de `areas_geoestadisticas_estatales` (32 elementos)
- [x] `--bench` de `areas_geoestadisticas_municipales` con presupuesto
- [x] Ninguna salida contiene `password=`
- [ ] Revisión con el humano antes del GUI

### Phase 3: Interfaz

- [x] T9 — `PostgisDialog` (conexión, contraseña, esquema, tablas) y acción de menú
- [x] T10 — Carga por lotes, grupo por conexión, tablas grandes, aviso de proyecto y recientes

### Checkpoint: después de T9–T10

- [x] `--snapshot-pg-dialog` genera un PNG con las conexiones del fixture
- [x] Prueba manual: `dataware_FDAC` → `mgn_2025` → dos tablas → dibujadas y agrupadas
- [ ] `frentes_de_manzana` (13,2 M) avisa y permite cancelar

### Phase 4: Cierre

- [x] T11 — README, matriz requisito → prueba, mediciones y estado de la spec
- [x] T12 — Sesión PostGIS viva y auto-conexión (seguimiento del 2026-11-20)

### Checkpoint: completo

- [x] Los nueve criterios de éxito de `SPEC-postgis.md` §11, uno por uno
- [x] `ctest --preset windows-msvc-release` entero en verde
- [x] `--info --json` y `--snapshot` de shapefile, GeoPackage, GeoJSON, KML y raster sin diferencias con `main`

## Verification commands

```bat
scripts\build.bat windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure

set PGPASSWORD=<tu contraseña>
set XML=D:\D\Ftes Mapa Digital\mapadigital_2027\bin\Connections.xml

bin\minigis.exe --connections "%XML%" --check-pg -l "pg:dataware_FDAC/mgn_2025" --json
bin\minigis.exe --connections "%XML%" --info -l "pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_estatales" --json
bin\minigis.exe --connections "%XML%" --snapshot pg.png -l "pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_estatales"
bin\minigis.exe --connections "%XML%" --bench 24 -l "pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_municipales" --bench-budget 150 --json
bin\minigis.exe --connections "%XML%" --snapshot-pg-dialog dialogo.png
```

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| La contraseña viaja en la cadena de conexión y se filtra a un mensaje o a un proyecto | Alto | `scrubSecrets` en el único punto de salida; `sourcePath()` es la URI; prueba que recorre `--info --json`, `-V` y el `.qgz` |
| Resolución de credenciales desde el hilo del cargador | Medio | `PgCredentials` con `QMutex` y carga perezosa; la contraseña se siembra en la caché de sesión desde el diálogo antes de arrancar la carga |
| `Connections.xml` cambia de formato o de codificación | Medio | Parser tolerante, fixture versionado y mensaje con causa/acción; no se lee `PASSWORD` |
| 5 121 capas en memoria al enumerar esquemas | Bajo | Solo se guardan nombres; la apertura global se descarta enseguida (70 ms medidos) |
| Tablas de 13 M de elementos bloquean minutos | Alto | Aviso con recuento, «dibujar bajo demanda», `--pg-no-index` y cancelación ya existentes |
| El comillado de libpq no cubre un valor raro | Medio | Escapado de `\` y `'` con unitarias; valores verificados en servidor real en T4 |
| DPAPI no disponible (perfil móvil, política) | Bajo | Se degrada a no recordar; aviso y contraseña por sesión |
| `--bench` construye el almacén de una tabla enorme | Medio | `--pg-no-index` y presupuesto; tabla media para la puerta |
| El código 8 rompe consumidores existentes | Bajo | Es aditivo (0–7 no cambian); se documenta en README y en las pruebas |
| Nombres de tabla con punto o barra | Bajo | Se documenta la limitación de la URI; el catálogo abre por `schemas=`/`tables=`, sin reconstruir nombres |
| GUI se queda esperando la conexión (LAN lenta) | Medio | Cursor de espera, botón Cancelar y tiempo de conexión acotado (`connect_timeout=10`) |

## Open Questions

Ninguna abierta: Q1–Q5 cerradas en la revisión del 2026-11-20. El umbral de
«tabla grande» (1 000 000) se fija en T10 y queda como constante documentada.





