# Tareas: conexión a PostgreSQL/PostGIS

Plan: `tasks/plan-postgis.md` · Spec: `SPEC-postgis.md`

Estado: **completa — T1–T11 hechas y verificadas (39 pruebas CTest: 36 en verde,
3 opt-in omitidas sin variables; 8/8 con el servidor real)**.

| Tarea | Estado | Evidencia |
|---|---|---|
| T1 `PgSource` | hecha | `minigis.pg-catalogo`: 68 comprobaciones, 0 fallos; URI, cadena y saneado |
| T2 catálogo `Connections.xml` | hecha | fixture: 2 `PGIS`, la `GPKG` se ignora; ISO-8859-1 decodificado |
| T3 credenciales | hecha | ida y vuelta DPAPI; el almacén no contiene la contraseña en claro |
| T4 catálogo PostGIS | hecha | `mgn_2025`: 16 tablas, `poligonos_de_manzana` 2 539 214 y SRID 6365 |
| T5 `VectorLayer` | hecha | capa real abierta: URI saneada, grupo `conn/esquema`, driver `PostgreSQL`, 32 elementos |
| T6 CLI básica | hecha | `--info`/`--snapshot`/`--identify-at` con `pg:`; exit 8 en conexión/contraseña; JSON sin `password` |
| T7 `--check-pg` | hecha | 16 tablas, `ok: true`, ~1,3 s; pruebas deterministas (exit 1/2/8) en verde |
| T8 `--bench` | hecha | `municipales`: p90 57 ms, `budget_ok: true`; `--pg-no-index` informa `store.mode: "none"` |
| T9 diálogo | hecha | captura con las 6 conexiones del catálogo real; conexión, contraseña, esquema y tablas |
| T10 integración GUI | hecha | dos tablas agrupadas en `dataware_FDAC/mgn_2025`; la captura de ventana las dibuja; aviso de tablas grandes y de proyecto implementados |
| T11 cierre | hecha | README (CLI, código 8, sección PostGIS), spec en «aprobada e implementada» y los 9 criterios repasados |
| T12 sesión viva | hecha | `PgSession` conserva conexión/contraseña/esquema; el catálogo mantiene la conexión abierta y cachea esquemas/tablas (segunda lectura: **0 ms**); el diálogo se auto-conecta y «Actualizar» refresca; «Recordar» marcado por defecto |

---

## Task 1: `PgSource` — URI, cadena de conexión y saneado

**Description:** Crear el módulo puro que convierte `pg:conexion/esquema/tabla`
en la cadena que entiende el driver PostgreSQL de GDAL, y que borra la
contraseña de cualquier texto antes de que salga del proceso. Es la base de
todo lo demás: no depende de red, ni de Qt Widgets, ni de GDAL.

**Acceptance criteria:**
- [ ] `PgSource::looksLike()` reconoce `pg:`/`PG:` y no confunde una ruta
  `D:\...` ni una URI `file:`.
- [ ] `parse()` acepta 2 segmentos (`pg:conn/esquema`, tabla vacía para el
  catálogo) y 3 (`pg:conn/esquema/tabla`); rechaza 1, 4 o segmentos vacíos con
  `Error` (causa + acción).
- [ ] `uri()` reconstruye exactamente la entrada, conservando mayúsculas del
  nombre de conexión (`dataware_FDAC`).
- [ ] `buildConninfo()` produce
  `PG:host='…' port=… dbname='…' user='…' password='…' connect_timeout=10 sslmode=prefer schemas='…' [tables='…']`;
  sin contraseña omite la clave (libpq usará `PGPASSWORD`).
- [ ] Escapa `\` y `'` en todos los valores; una contraseña con `'` y espacio no
  rompe la cadena (comprobado con unitaria).
- [ ] `scrubSecrets()` convierte `password=x`, `Password = 'x y'` y `PASSWORD="x"`
  en `password=***` sin tocar el resto del texto.
- [ ] Nada del módulo escribe en disco, en logs ni en ajustes.

**Verification:**
- [ ] `ctest --preset windows-msvc-release -R minigis.pg-catalogo` en verde.
- [ ] Usar el ejecutable de pruebas con la URI del ejemplo y comprobar la cadena
  generada (se imprime saneada).

**Dependencies:** None

**Files likely touched:**
- `src/core/minigispgsource.h`, `src/core/minigispgsource.cpp` (nuevos)
- `src/core/CMakeLists.txt`
- `tests/minigispgtest.cpp` (nuevo), `tests/CMakeLists.txt`

**Estimated scope:** M (5 archivos)

---

## Task 2: catálogo `Connections.xml`

**Description:** Leer el archivo de Mapa Digital respetando su codificación y
quedarse solo con lo que MiniGIS entiende: entradas `TYPE="PGIS"` con nombre,
servidor, puerto, base y usuario. El atributo `PASSWORD` no se lee.

**Acceptance criteria:**
- [ ] `PgConnectionCatalog::load(ruta)` lee ISO-8859-1 y devuelve las entradas
  `PGIS` en el orden del archivo; ignora `GPKG`/`SQLite`.
- [ ] `PORT` vacío o ausente → 5432; `NAME` repetido → se conserva el primero y
  se avisa.
- [ ] La estructura `PgConnection` **no tiene** campo de contraseña y su
  serialización (para `--info --json`) no la incluye.
- [ ] `resolvePath()` respeta el orden `--connections` → `MINIGIS_CONNECTIONS` →
  `QSettings("postgis/xmlPath")` → vacío.
- [ ] Archivo ausente, XML inválido o sin `PGIS` → `Error` con causa y acción.
- [ ] Fixture versionado `data/sample/postgis/Connections.xml` con 2 `PGIS`,
  1 `GPKG` y contraseñas de mentira (nunca una credencial real).

**Verification:**
- [ ] Unitaria: el fixture da 2 conexiones, `dataware_FDAC` sería una de ellas,
  la `GPKG` no aparece y no hay rastro de `PASSWORD`.
- [ ] `minigis --connections <fixture> --snapshot-pg-dialog` (cuando exista T7)
  muestra las dos.

**Dependencies:** None

**Files likely touched:**
- `src/core/minigisconnections.h`, `src/core/minigisconnections.cpp` (nuevos)
- `src/core/CMakeLists.txt`
- `data/sample/postgis/Connections.xml` (nuevo)
- `tests/minigispgtest.cpp`

**Estimated scope:** M (5 archivos)

---

## Task 3: credenciales (DPAPI + sesión)

**Description:** Resolver la contraseña sin volver a pedirla en cada tabla y sin
guardarla en claro. Caché en memoria durante la sesión y, si el usuario marca
«Recordar», blob cifrado con DPAPI en el perfil.

**Acceptance criteria:**
- [ ] `PgCredentials::passwordFor(conn)`: caché de sesión → almacén DPAPI →
  vacío (libpq usará `PGPASSWORD`/`.pgpass`).
- [ ] `remember(conn, pass, persistir)` cifra con `CryptProtectData` y escribe
  `%APPDATA%\MiniGIS\postgis-credentials.json`; con `persistir=false` solo
  actualiza la caché.
- [ ] El archivo no contiene la contraseña en claro (comprobado leyéndolo en la
  unitaria); el blob se indexa por `conexión|servidor|puerto|usuario`.
- [ ] `forget(conn)` borra la entrada persistida.
- [ ] Acceso protegido con `QMutex`: el cargador resuelve desde su hilo.
- [ ] En un equipo sin DPAPI (o blob de otro usuario) degrada a vacío sin
  romper; compila fuera de Windows con caché en memoria.

**Verification:**
- [ ] Unitaria: recordar → nuevo objeto → recuperar; borrar el archivo → vacío;
  el texto del archivo no contiene la contraseña.
- [ ] Manual: reiniciar MiniGIS y comprobar que no vuelve a pedirla con
  «Recordar» marcado.

**Dependencies:** Task 2

**Files likely touched:**
- `src/core/minigispgcredentials.h`, `src/core/minigispgcredentials.cpp` (nuevos)
- `src/core/CMakeLists.txt` (enlazar `crypt32` en Windows)
- `tests/minigispgtest.cpp`

**Estimated scope:** M (4 archivos)

---

## Task 4: catálogo PostGIS (esquemas y tablas)

**Description:** Enumerar esquemas y, por esquema, tablas con geometría, número
de elementos y SRID, usando solo la API C de GDAL. Es lo que alimenta el
diálogo y `--check-pg`.

**Acceptance criteria:**
- [ ] `schemas(conn)` deduplica los prefijos de las capas (`public` cuando no
  hay punto) y tarda menos de 2 s en la red del usuario (medidos 70 ms).
- [ ] `tables(conn, "mgn_2025")` devuelve 16 filas con nombre, geometría legible
  (`MultiPolygon` → «Polígono»), elementos y SRID 6365.
- [ ] `poligonos_de_manzana` informa 2 539 214 elementos y
  `areas_geoestadisticas_estatales` 32.
- [ ] `tableLayers(conn, esquema, tabla)` abre exactamente 1 capa; un nombre
  inexistente devuelve `Error` con las tablas disponibles en el `hint`.
- [ ] Todos los mensajes pasan por `scrubSecrets`; la cadena con contraseña no
  se guarda en ninguna estructura.
- [ ] Si GDAL no tiene el driver `PostgreSQL`, el error lo dice y sugiere la
  feature de vcpkg.

**Verification:**
- [ ] `--check-pg` manual contra `dataware_FDAC` (tras T7) coincide con lo
  medido en la spec §2.
- [ ] Inspección con `-V` de que no aparece `password=`.

**Dependencies:** Task 2, Task 3

**Files likely touched:**
- `src/core/minigispgcatalog.h`, `src/core/minigispgcatalog.cpp` (nuevos)
- `src/core/CMakeLists.txt`

**Estimated scope:** M (3 archivos)

---

## Task 5: `VectorLayer` acepta `pg:`

**Description:** Hacer que la capa vectorial existente abra una tabla PostGIS
igual que abre un GeoPackage, conservando la URI como identidad y exponiendo los
datos de conexión para `--info`.

**Acceptance criteria:**
- [ ] `VectorLayer::open("pg:conn/esquema/tabla")` devuelve una capa con
  `sourcePath()` = la URI, `containerName()` = `conn/esquema`, `layerCount()` 1
  y nombre = tabla.
- [ ] `layerNames("pg:conn/esquema/tabla")` devuelve `{tabla}` sin exigir que la
  ruta exista en disco.
- [ ] `detectLayerKind("pg:…")` devuelve `Vector` sin abrir la red (el error de
  conexión se informa al cargar, no al clasificar).
- [ ] `openVectorDataset()` ya no aplica `QFileInfo::exists` a las fuentes `pg:`
  y construye la cadena con `PgConnection` + `PgCredentials`.
- [ ] Una conexión inexistente o sin contraseña devuelve `Error` con causa y
  acción, sin volcar la cadena.
- [ ] Shapefile, GeoPackage, GeoJSON, KML, raster y proyectos siguen igual.

**Verification:**
- [ ] `--info` de `pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_estatales`
  (tras T6) informa 32 elementos, driver `PostgreSQL`, EPSG:6365.
- [ ] `ctest --preset windows-msvc-release` sin regresiones.

**Dependencies:** Task 1, Task 3

**Files likely touched:**
- `src/core/minigisvectorlayer.h`, `src/core/minigisvectorlayer.cpp`
- `src/core/minigislayerkind.cpp`

**Estimated scope:** M (3 archivos)

---

## Task 6: CLI básica con `pg:`

**Description:** Que `--info`, `--snapshot` y `--identify-at` acepten la URI
`pg:` y que `--connections` fije el catálogo. Nada de lógica nueva de dibujo:
es abrir la puerta y sanear lo que sale.

**Acceptance criteria:**
- [ ] `--connections <ruta>` alimenta al catálogo (y a `MINIGIS_CONNECTIONS` si
  no se pasa).
- [ ] Los `QFileInfo::exists` de la CLI se sustituyen por `sourceExists()`:
  una fuente `pg:` no se comprueba contra el disco.
- [ ] `--info` de una capa PostGIS imprime `Conexión` y `Tabla`; `--json` añade
  `connection {name, server, port, database, user}`, `schema` y `table`, sin
  contraseña.
- [ ] `--snapshot` y `--identify-at` funcionan con `pg:`.
- [ ] Error de conexión/autenticación → exit **8**; conexión inexistente → 8 con
  la lista de nombres; esquema/tabla inexistente → 3.
- [ ] `--help` documenta `--connections`, `--check-pg`, `--snapshot-pg-dialog`
  y `--pg-no-index`, y la tabla de códigos incluye el 8.

**Verification:**
- [ ] `--info --json` de una tabla real: exit 0 y JSON sin `password`.
- [ ] `--snapshot` produce un PNG no vacío con la extensión de México.
- [ ] `PGPASSWORD` incorrecta: exit 8 y el mensaje no repite la contraseña.

**Dependencies:** Task 5

**Files likely touched:**
- `src/app/main.cpp`

**Estimated scope:** M (1 archivo grande)

---

## Task 7: `--check-pg` y `--snapshot-pg-dialog`

**Description:** El modo de diagnóstico sin ventana que hace verificable todo lo
anterior: lista las tablas de un esquema, comprueba que cada una abre y cuenta
elementos, en texto y en JSON. Y la captura del diálogo sin servidor.

**Acceptance criteria:**
- [ ] `--check-pg -l "pg:conn/esquema"` sin `--json` imprime una tabla legible;
  con `--json`, el contrato de `SPEC-postgis.md` §9.3.
- [ ] Códigos: 0 correcto, 1 sin `-l`, 2 catálogo no encontrado, 3 GDAL no abre,
  5 esquema sin capas, 8 conexión/autenticación.
- [ ] `ok: false` si alguna tabla no abre, con `error.message`/`error.hint`
  saneados.
- [ ] `--snapshot-pg-dialog <png> --connections <fixture>` escribe un PNG no
  vacío con las conexiones listadas y sin tocar la red.
- [ ] Con el fixture y sin servidor, los casos de error son deterministas (sin
  esperas de red).

**Verification:**
- [ ] `ctest -R minigis.pg-` (sin catálogo, conexión inexistente, uso,
  diálogo) en verde.
- [ ] `--check-pg` real contra `mgn_2025`: 16 tablas, `ok: true`, < 2 s.

**Dependencies:** Task 4, Task 6

**Files likely touched:**
- `src/core/minigispgcheck.h`, `src/core/minigispgcheck.cpp` (nuevos)
- `src/core/CMakeLists.txt`
- `src/app/main.cpp`
- `tests/run_cli_test.cmake` (modos nuevos), `tests/CMakeLists.txt`

**Estimated scope:** M (5 archivos)

---

## Task 8: `--bench` con `pg:` y `--pg-no-index`

**Description:** Medir el dibujo de una capa PostGIS con el mismo contrato del
banco de pruebas, y poder medir tablas que no caben en el almacén sin
construirlo.

**Acceptance criteria:**
- [ ] `--bench <n> -l "pg:…"` mide y emite el JSON de siempre.
- [ ] `--pg-no-index` no construye almacén para fuentes `pg:` y el JSON informa
  `store.mode: "none"`.
- [ ] `--bench-budget` sigue devolviendo 6 cuando se supera.
- [ ] Las capas de archivo conservan su comportamiento (`store.mode`
  `memory`/`envelopes`).

**Verification:**
- [ ] `--bench 24` de `areas_geoestadisticas_municipales` con presupuesto
  150 ms: exit 0, `budget_ok: true` (p90 medido 48 ms). `...urbanas` (64 808)
  tarda ~879 ms y solo se anota como referencia.
- [ ] `--bench 12 --pg-no-index` de `frentes_de_manzana`: no bloquea el
  arranque y `store.mode` es `none`.

**Dependencies:** Task 5

**Files likely touched:**
- `src/app/main.cpp`

**Estimated scope:** S (1 archivo)

---

## Task 9: diálogo «Abrir conexión PostGIS»

**Description:** La puerta de entrada del GUI: elegir conexión, escribir la
contraseña una vez, elegir esquema y marcar una o varias tablas. No sabe GDAL:
pide catálogo al core y devuelve selecciones.

**Acceptance criteria:**
- [ ] Acción **Archivo > Abrir conexión PostGIS…** (Ctrl+Shift+O) y botón en la
  barra.
- [ ] Combo con las conexiones `PGIS`, botón **Examinar…**, resumen
  servidor/puerto/base/usuario.
- [ ] Contraseña enmascarada, «Recordar», **Conectar**; tras conectar, combo de
  esquema, filtro y árbol de tablas (tabla, geometría, elementos) con selección
  múltiple.
- [ ] Errores dentro del diálogo con causa y acción; cursor de espera y
  Cancelar mientras conecta.
- [ ] Aceptar devuelve la lista de tablas seleccionadas con su esquema y
  conexión; Cancelar no toca nada.
- [ ] Ni la contraseña ni la cadena de conexión acaban en ajustes, logs o
  títulos.

**Verification:**
- [ ] `--snapshot-pg-dialog dialogo.png --connections <fixture>` para la
  captura, con revisión visual.
- [ ] Prueba manual completa con `dataware_FDAC`.

**Dependencies:** Task 4

**Files likely touched:**
- `src/gui/minigispostgisdialog.h`, `src/gui/minigispostgisdialog.cpp` (nuevos)
- `src/gui/CMakeLists.txt`
- `src/gui/minigismainwindow.cpp` (acción y conexión)

**Estimated scope:** L (4 archivos)

---

## Task 10: integración en la ventana

**Description:** Que las tablas elegidas se carguen como cualquier capa, con
grupo por conexión, confirmación para tablas enormes y un aviso claro al
guardar un proyecto que contiene capas PostGIS.

**Acceptance criteria:**
- [ ] Las capas se cargan en segundo plano con progreso y cancelación; el panel
  las agrupa por `conexión/esquema` y el encuadre inicial las cubre.
- [ ] Tabla con más de 1 000 000 de elementos: confirmación con recuento y tres
  salidas (índice / bajo demanda / cancelar); «bajo demanda» deja
  `LayerRequest::buildStore = false`.
- [ ] Guardar un `.qgz` con capas PostGIS avisa «N capas PostGIS no se guardan»
  y el archivo no contiene credenciales; las capas siguen en la sesión.
- [ ] «Recientes» y arrastrar/soltar no tratan `pg:` como ruta de disco.
- [ ] Tabla de atributos, identificar, etiquetas y simbología funcionan sobre
  una capa PostGIS.

**Verification:**
- [ ] Prueba manual con dos tablas de `mgn_2025` (una pequeña y otra grande).
- [ ] `--snapshot-window ventana.png -l "pg:…"` muestra la capa dibujada y el
  grupo en el panel.

**Dependencies:** Task 5, Task 9

**Files likely touched:**
- `src/gui/minigismainwindow.h`, `src/gui/minigismainwindow.cpp`
- `src/core/minigisvectorloader.h`, `src/core/minigisvectorloader.cpp`
- `src/core/minigisqgisproject.cpp` (excluir capas `pg:` al guardar)

**Estimated scope:** M (4 archivos)

---

## Task 11: documentación, mediciones y cierre

**Description:** Dejar el contrato escrito y las evidencias medidas: README,
tabla de pruebas, estado de la spec y del plan.

**Acceptance criteria:**
- [ ] `README.md`: opciones nuevas en la tabla de la CLI, código 8, sección
  «PostGIS» con el flujo y una advertencia sobre `Connections.xml` y la
  contraseña.
- [ ] `SPEC-postgis.md`: estado **aprobada e implementada** y tabla de tiempos
  reales (catálogo, apertura, `--check-pg`, `--bench`).
- [ ] `ctest` completo en verde, con las pruebas `pg-*` y las opt-in omitidas
  (77) sin variables.
- [ ] Los nueve criterios de `SPEC-postgis.md` §11 comprobados uno a uno.

**Verification:**
- [ ] `ctest --preset windows-msvc-release --output-on-failure`.
- [ ] Repaso de los nueve criterios con la salida de los comandos a mano.

**Dependencies:** Tasks 6, 7, 8, 10

**Files likely touched:**
- `README.md`, `SPEC-postgis.md`, `tasks/plan-postgis.md`, `tasks/todo-postgis.md`

**Estimated scope:** S (4 archivos)
