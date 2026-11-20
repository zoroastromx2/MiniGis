# Spec: conexión a PostgreSQL/PostGIS

> Módulo: `postgis` · Estado: **aprobada e implementada (2026-11-20)** · Fecha: 2026-11-20
>
> Implementación: `src/core/minigispg{s,connections,credentials,catalog,check}.*`,
> `src/gui/minigispostgisdialog.*` y la integración en `VectorLayer`,
> `MainWindow` y la CLI. Verificado con `dataware_FDAC` (PostgreSQL 13.16 /
> PostGIS 3.2.1) y con 39 pruebas de CTest.
>
> Este documento define *qué* se construye y *cómo se sabe que está bien*. No se
> escribe código hasta que se apruebe. El plan técnico y las tareas van después,
> en `tasks/plan-postgis.md` y `tasks/todo-postgis.md`.
>
> Una sola capacidad (`postgis`): no aplica el mapa de capacidades de la fase 0.

---

## 1. Objetivo

MiniGIS debe abrir y dibujar capas vectoriales guardadas en **PostgreSQL con
PostGIS**, eligiendo la conexión por su **nombre** en el `Connections.xml` de
Mapa Digital (el ejemplo del usuario es `dataware_FDAC`), sin escribir cadenas
de conexión a mano y sin que la contraseña acabe en un proyecto, un log o una
salida JSON.

Hoy MiniGIS solo abre archivos: toda fuente se valida con `QFileInfo::exists`,
así que una conexión `PG:...` muere con «No existe el archivo» (verificado: exit
2). Este cambio añade una **fuente nueva** al mismo motor —no un visor aparte—
con el mismo contrato que ya tienen shapefile, GeoPackage, KML y GeoJSON:

* elegir conexión del `Connections.xml` y autenticarse;
* explorar **esquemas y tablas** con su geometría y su número de elementos;
* abrir una o varias tablas como capas normales: panel, simbología, etiquetas,
  tabla de atributos, identificar, `--info`, `--snapshot`;
* todo **de solo lectura**;
* verificarlo sin ventana, como el resto del proyecto.

**Usuario:** quien ya tiene sus datos en el almacén corporativo (`bdginegi` u
otro) y hoy tiene que exportarlos a GeoPackage con `ogr2ogr` para verlos en
MiniGIS.

**Criterio de éxito del producto:** abrir el diálogo, elegir `dataware_FDAC`,
escribir la contraseña una vez, elegir `mgn_2025` y `poligonos_de_manzana`, y ver
la capa dibujada sobre México con sus 2,5 millones de elementos, su tabla de
atributos y su identificar, sin que la contraseña aparezca en ningún archivo.

---

## 2. Estado de partida (verificado hoy)

Comprobaciones ejecutadas en este equipo el 2026-11-20 con
`build\windows-msvc-release\bin\minigis.exe` (MiniGIS 0.3.0, Qt 6.10.3, GDAL
3.12.4, PROJ 9.9.0, MSVC 19.44, Release) y con la API C de GDAL 3.12.4.

| Comprobación | Resultado verificado |
|---|---|
| `Connections.xml` del usuario | `D:\D\Ftes Mapa Digital\mapadigital_2027\bin\Connections.xml`, 1 683 bytes, codificación **ISO-8859-1** |
| Entradas | 8 `ConStr`: **6 `TYPE="PGIS"`** (`dataware_FDAC`, `MDM_POSTGIS_ADM`, `MDM_POSTGIS_ADM_ADO`, `Beto_Virtual_2_1_ADO`, `Beto_Virtual_ADO`, `dataware_ADO`) y **2 `TYPE="GPKG"`** (`nute`, `RNC`) |
| `dataware_FDAC` | `SERVER="10.153.3.32" PORT="5433" DATABASE="bdginegi" USER="datawarehouse"`, `DRIVER="FireDac"`, `PASSWORD` **cifrada** |
| Cifrado de las contraseñas | Esquema **propietario de Mapa Digital** (blob base64 de 32 bytes); no hay clave ni algoritmo documentados. MiniGIS **no puede descifrarlas** |
| Contraseña en claro | Los `.pmd` de Mapa Digital (p. ej. `D:\F\IRIS 4.0\postgres.pmd`) sí guardan `Password=` en claro. Es un dato del usuario, **no** una fuente que MiniGIS deba leer |
| Red | `10.153.3.32:5433` responde a TCP desde este equipo |
| Conexión real | **OK** con `libpq`: PostgreSQL **13.16** (x86_64-pc-linux-gnu) y **PostGIS 3.2.1** |
| `minigis --list-drivers` | incluye **`PostgreSQL`**; junto a `minigis.exe` están `libpq.dll`, `libssl-3-x64.dll`, `libcrypto-3-x64.dll` e `iconv-2.dll` |
| `minigis --info "PG:host=... user=..."` | **hoy falla**: exit 2, «No existe el archivo»; `openVectorDataset()` exige `QFileInfo::exists` antes de llamar a GDAL |
| Catálogo sin `schemas=` | `GDALOpenEx` devuelve **5 121 capas en 70 ms**, con nombre `esquema.tabla` (`mgn_2022.areas_geoestadisticas_basicas_rurales`) |
| Catálogo por esquema | con `schemas=mgn_2025`: **16 capas**, nombre **sin** prefijo (`poligonos_de_manzana`) |
| Apertura de una tabla | con `schemas=mgn_2025 tables=poligonos_de_manzana`: **1 capa en 55 ms** |
| Recuento de elementos | `OGR_L_GetFeatureCount(force=0)` devuelve cifra de `pg_class` (estimada), suficiente para avisar antes de cargar |
| Tamaños reales (`mgn_2025`) | `areas_geoestadisticas_estatales` **32**; `areas_geoestadisticas_basicas_urbanas` 64 808; `poligonos_de_manzana` **2 539 214**; `ejes_de_vialidad` 7 224 753; `frentes_de_manzana` **13 167 925** |
| Tipos de geometría | `POINT`, `MULTIPOINT`, `MULTILINESTRING`, `MULTIPOLYGON`, todos **SRID 6365** (Mexico ITRF2008 / LCC) |
| Reproyección | GDAL entrega el SRC y el motor ya reproyecta a EPSG:4326 como con cualquier capa |
| Proyectos QGIS | `minigisqgisproject.cpp` solo acepta `provider` `ogr`/`gdal` con ruta de archivo; una capa PostGIS quedaría «no disponible» al guardarse/reabrirse |
| Contraseña en errores de GDAL | libpq/GDAL la enmascaran en el mensaje (`password=XXXX...`); **no se depende de eso**: se enmascara también por nuestra cuenta |

**Conclusión del sondeo:** el motor vectorial no hay que reescribirlo. Falta
**una fuente nueva** (`pg:`), un **catálogo de conexiones** que sepa leer
`Connections.xml` sin tocar las contraseñas, una **política de credenciales**
(preguntar y recordar con DPAPI) y un **contrato verificable** sin ventana.

---

## 3. Requisitos funcionales

### R1 — Catálogo de conexiones desde `Connections.xml`

* Se lee con `QXmlStreamReader` respetando la codificación declarada
  (`ISO-8859-1`), sin depender de la extensión del archivo.
* Solo se listan las entradas `TYPE="PGIS"` (comparación sin distinguir
  mayúsculas). Las de tipo `GPKG`/`SQLite` se ignoran y no aparecen.
* De cada entrada se conservan `NAME`, `SERVER`, `PORT` (5432 por defecto si
  falta), `DATABASE` y `USER`. El atributo **`PASSWORD` no se lee**: no se
  guarda en memoria ni se descifra.
* Orden de resolución de la ruta del archivo, el primero que exista:
  1. `--connections <ruta>` (CLI);
  2. variable de entorno `MINIGIS_CONNECTIONS`;
  3. ajuste guardado por el GUI (`QSettings`, grupo `postgis`);
  4. si no hay ninguna, el diálogo pide el archivo y recuerda la elección.
* Un archivo ausente, ilegible o sin ninguna entrada `PGIS` produce
  `minigis::Error` con causa y acción; el GUI lo muestra en el propio diálogo,
  no como error fatal de arranque.

### R2 — Credenciales

* **GUI:** campo de contraseña enmascarado y casilla **«Recordar»** (marcada por
  defecto: quien conecta quiere no volver a escribirla). Al recordar, la
  contraseña se cifra con **DPAPI** de Windows (`CryptProtectData`, ámbito del
  usuario) y se guarda en `%APPDATA%\MiniGIS\postgis-credentials.json`, indexada
  por `conexión + servidor + usuario`. No se guarda nunca en claro. Desmarcarla
  borra la copia persistida y la mantiene solo en la sesión.
* **Sesión:** una contraseña introducida se recuerda en memoria mientras la
  aplicación vive, para no pedirla por cada tabla.
* **CLI:** nunca pregunta. Orden de resolución: credencial DPAPI del usuario (si
  existe) → mecanismos estándar de libpq (`PGPASSWORD`, `PGPASSFILE`,
  `%APPDATA%\postgresql\pgpass.conf`).
* La contraseña **no aparece** en `--info`, `--json`, `stderr`, mensajes de
  error, ajustes en claro, archivos de proyecto, lista de recientes ni nombre de
  capa.

### R3 — Conexión y catálogo de esquemas y tablas

* La conexión se abre con la API C de GDAL:
  `GDALOpenEx("PG:...", GDAL_OF_VECTOR | GDAL_OF_READONLY, ...)`.
  La cadena de conexión incluye `connect_timeout=10`, `sslmode=prefer` y los
  parámetros `schemas=`/`tables=` cuando tocan; **nunca** `GDAL_OF_UPDATE`.
* **Esquemas:** una apertura sin `schemas=` lista todas las capas
  (`esquema.tabla`) y de ahí se deducen los esquemas. `public` es el esquema de
  las capas sin punto. Presupuesto: **< 2 s** en la red del usuario (medidos
  70 ms).
* **Tablas de un esquema:** se abre `schemas=<esquema>` y se informa, por tabla:
  nombre, tipo de geometría legible (`Polígono`, `Línea`, `Punto`, `Multipunto`…),
  número de elementos (estimado, `force=0`) y SRID.
* El diálogo ofrece filtro de texto sobre las tablas y selección múltiple.
* Un esquema sin capas vectoriales produce lista vacía con aviso, no error.

### R4 — Abrir capas PostGIS

* Cada tabla se abre como una capa normal (`VectorLayer`) con
  `schemas=<esquema> tables=<tabla>`; todo lo que hoy funciona con un archivo
  debe funcionar aquí: panel y grupos, simbología, etiquetas, tabla de
  atributos, identificar, `--info`, `--snapshot`.
* La carga sigue siendo **en segundo plano** y cancelable, con el mismo
  cargador (`VectorLayerLoader`); una tabla que falla no cancela las demás.
* El nombre visible es el de la tabla; el grupo del panel es
  `<conexión>/<esquema>` y la línea de posición de `--info` informa «capa 1 de 1
  de «dataware_FDAC/mgn_2025»».

### R5 — Identidad de la fuente: URI `pg:`

* Una capa PostGIS se identifica con la URI **`pg:<conexión>/<esquema>/<tabla>`**
  (p. ej. `pg:dataware_FDAC/mgn_2025/poligonos_de_manzana`), sin credenciales,
  en ASCII y estable entre sesiones.
* `VectorLayer::sourcePath()` devuelve esa URI; `--info` imprime
  `Conexión : dataware_FDAC (10.153.3.32:5433/bdginegi, datawarehouse)` y
  `Tabla : mgn_2025.poligonos_de_manzana` en lugar de las líneas de archivo.
* `--info --json` añade un objeto `connection` (`name`, `server`, `port`,
  `database`, `user`) y los campos `schema` y `table`. Nunca `password`.
* `detectLayerKind`, `VectorLayer::layerNames` y las comprobaciones de
  existencia de la CLI reconocen el prefijo `pg:` y **no** lo tratan como ruta
  de disco.

### R6 — Confidencialidad

* Toda cadena que venga de GDAL/libpq o que compongamos pasa por una única
  función `scrubSecrets()` que sustituye `password=<lo que sea>` por
  `password=***`, sin distinguir mayúsculas ni comillas.
* `qDebug`/`qWarning` y `-V` usan siempre la forma saneada.
* Guardar un proyecto con capas PostGIS **no** serializa la cadena de conexión
  (ver R12 y §5).

### R7 — Errores accionables

| Situación | Mensaje (resumen) | Acción sugerida | Exit |
|---|---|---|---|
| `Connections.xml` no existe o no tiene `PGIS` | «No se encontró el catálogo de conexiones» | «Indica el archivo con `--connections` o elige otro en el diálogo» | 2 |
| Conexión rechazada / DNS / puerto | «No se pudo conectar con 10.153.3.32:5433» | «Comprueba la red/VPN; el servidor responde en el puerto 5433» | 8 |
| Autenticación fallida | «La contraseña de «dataware_FDAC» no es válida» | «Vuelve a escribirla; la de Connections.xml está cifrada y MiniGIS no la usa» | 8 |
| Nombre de conexión inexistente | «No existe la conexión «X»» | «Disponibles: dataware_FDAC, MDM_POSTGIS_ADM, …» | 8 |
| Esquema o tabla inexistente | «No existe la tabla «mgn_2025.foo»» | «Tablas del esquema: …» | 3 |
| GDAL sin driver `PostgreSQL` | «Este GDAL no puede abrir PostGIS» | «Recompila con la feature `postgresql` de vcpkg» | 3 |
| Esquema sin capas | «El esquema «x» no tiene capas vectoriales» | «Elige otro esquema» | 5 |

El código **8** es nuevo y aditivo; no se renumera ninguno de los existentes
(0–7).

### R8 — Interfaz

* Entrada **Archivo > Abrir conexión PostGIS…** (Ctrl+Shift+O) y botón en la
  barra, junto a «Abrir capa».
* Diálogo `PostgisDialog`:
  * combo **Conexión** con las entradas `PGIS` del catálogo y botón
    **«Examinar…»** para elegir otro `Connections.xml`;
  * resumen de solo lectura: servidor, puerto, base de datos, usuario;
  * contraseña + «Recordar» + botón **Conectar**;
  * tras conectar: combo **Esquema**, filtro de texto, árbol de tablas
    (tabla, geometría, elementos) con selección múltiple y botón
    **«Añadir capas»**.
* Mientras conecta o lista, cursor de espera y botón Cancelar; los errores se
  muestran **dentro** del diálogo con causa y acción, sin cerrarlo.
* Al aceptar, las capas entran por el cargador normal: progreso, cancelación,
  agrupación por conexión y encuadre inicial igual que un archivo.

### R9 — Línea de comandos

* `-l, --layer` y los argumentos posicionales aceptan la URI `pg:`.
* `--connections <ruta>` fija el catálogo (también para el GUI si se pasa).
* `--check-pg` diagnostica un esquema: `minigis --check-pg -l "pg:conn/esquema"
  [--json]` lista las tablas, comprueba que GDAL las expone como capas y cuenta
  elementos. Sin `-l` sale con 1; `--json` sigue el contrato de §9.3.
* Soportado en v1: `--info`, `--snapshot`, `--identify-at`, `--bench`,
  `--check-pg` y `--snapshot-pg-dialog`. `--check-layers` con `pg:` queda para
  después (§5).
* `--bench` con `pg:` conserva su contrato (JSON estable, `--bench-budget` como
  puerta, `store.mode`); acepta `--pg-no-index` para medir el dibujo bajo
  demanda sin construir el almacén.
* `--snapshot-pg-dialog <png> --connections <xml>` captura el diálogo sin
  servidor (solo catálogo y estado inicial), para la prueba de interfaz.

### R10 — Tablas grandes

* El diálogo muestra el número de elementos **antes** de abrir.
* Al abrir una tabla con más de **1 000 000** de elementos se pide confirmación:
  ««frentes_de_manzana» tiene 13,2 millones de elementos. Preparar el índice de
  dibujo puede tardar varios minutos.» con tres salidas: **Preparar índice**,
  **Dibujar bajo demanda** (sin almacén, lectura directa por cuadro) y
  **Cancelar**. La elección se recuerda durante la sesión.
* El progreso y la cancelación existentes deben funcionar; la cancelación deja
  la aplicación utilizable y no añade la capa a medias.
* En la CLI, `--pg-no-index` fuerza «dibujar bajo demanda» y evita el diálogo de
  confirmación, de modo que `--bench` y `--snapshot` de tablas enormes se puedan
  ejecutar sin ventana.

### R11 — Diagnóstico verificable sin ventana

* Pruebas unitarias (ejecutable propio, como `minigisproyectoiotest`) para lo
  que no necesita servidor: parseo de `Connections.xml`, URI `pg:`,
  construcción de la cadena de conexión, escapado de comillas y `scrubSecrets`.
* Prueba de integración **opt-in**: si existen las variables `MINIGIS_PG_TEST_*`
  (`CONNECTIONS`, `URI`, `LAYER`), ejecuta `--check-pg`/`--bench` reales y
  compara el contrato; si no, la prueba se marca como omitida
  (`SKIP_REGULAR_EXPRESSION`), nunca como fallo.
* `--snapshot-pg-dialog` verifica la interfaz sin servidor.

### R12 — No regresión

* Ninguna capa de archivo cambia de comportamiento: `--info --json` y
  `--snapshot` de shapefile, GeoPackage, GeoJSON, KML y raster deben coincidir
  con los de `main` (salvo los campos nuevos, que son aditivos).
* `ctest --preset windows-msvc-release` entero en verde, incluida
  `minigis.rendimiento`.
* Guardar un proyecto con capas PostGIS: se guardan las capas de archivo y se
  avisa «N capas PostGIS no se guardan en el proyecto»; las capas PostGIS siguen
  en la sesión. El `.qgz` no contiene credenciales.

### R13 — Sesión persistente y auto-conexión

* **Sesión viva:** mientras la aplicación está abierta, `PgSession` conserva la
  conexión, la contraseña y el último esquema. El catálogo mantiene además
  abiertas sus conexiones (la de todos los esquemas y la del esquema actual) y
  cachea en memoria los esquemas y las tablas: volver a abrir el diálogo no pide
  nada y muestra las tablas al instante, sin volver a conectar.
* **Actualizar:** el botón «Actualizar» cierra la conexión de catálogo, vacía la
  caché y relee esquemas y tablas, para ver cambios hechos en el servidor.
* **Auto-conexión:** al abrir el diálogo, si hay sesión activa o una contraseña
  recordada para la última conexión, se conecta solo y lista esquemas/tablas. Si
  no hay contraseña recordada, preselecciona la última conexión y deja el foco en
  el campo de contraseña.
* **Persistencia:** se guardan `postgis/lastConnection`, `postgis/lastSchema` y
  `postgis/xmlPath` en los ajustes; la contraseña, cifrada con DPAPI. Nada de eso
  acaba en el proyecto ni en los logs.
* La captura `--snapshot-pg-dialog` desactiva la auto-conexión a propósito: una
  prueba sin ventana no debe tocar la red ni depender del perfil del usuario.

---

## 4. Decisiones (y por qué)

* **D1 — GDAL por la API C, sin libpq directo.** `GDALOpenEx` con la cadena
  `PG:...` y las opciones `schemas=`/`tables=` ya resuelve conexión, catálogo,
  reproyección y lectura. Meter libpq a mano duplicaría el manejo de errores y
  rompería la regla del proyecto (GDAL solo por API C). La contraseña la
  gestiona libpq, que es quien entiende `PGPASSWORD`/`.pgpass`.
* **D2 — URI `pg:` como identidad, cadena de conexión solo en memoria.** La
  cadena lleva la contraseña; no puede ser la ruta de la capa. La URI es
  estable, legible, sin secretos y encaja en `-l`, recientes y mensajes.
* **D3 — No intentar descifrar `Connections.xml`.** El cifrado es propietario y
  no hay clave. Pedir la contraseña una vez (y recordarla con DPAPI) es honesto,
  seguro y no depende de un formato privado que puede cambiar.
* **D4 — Catálogo por esquema, no «todas las capas».** Abrir el `PG:` entero
  produce **5 121 capas**: imposible de presentar y de cargar. Se usa una
  apertura global solo para enumerar esquemas (70 ms) y una por esquema para
  las tablas (16 capas, 55 ms).
* **D5 — Solo lectura, en toda la ruta.** El visor no tiene modelo de edición;
  `GDAL_OF_UPDATE` no se usa nunca. Es también la garantía para un almacén
  corporativo compartido.
* **D6 — Proyectos: fuera de alcance en v1, con aviso.** Escribir el
  `datasource` del provider `postgres` de QGIS es otro contrato (formato
  `dbname=… table="s"."t" (geom) sql=`, credenciales al reabrir) y merece su
  propia spec. Se avisa y no se guarda a medias.
* **D7 — Pruebas sin servidor obligatorio.** El `bdginegi` no está en CI ni debe
  estarlo. Lo determinista (parseo, URI, saneado) se prueba siempre; lo que
  necesita red es opt-in y se documenta como evidencia manual.

---

## 5. Fuera de alcance (explícito)

* Escribir, crear o borrar tablas, filas o esquemas; transacciones; `COPY`.
* PostGIS **raster**, topología, `geography` como tipo propio, `raster_columns`.
* Filtros SQL, `WHERE`, `ORDER BY`, joins, vistas parametrizadas y límites por
  extensión visible.
* Guardar/reabrir capas PostGIS en proyectos `.qgz` (D6).
* WFS, WMS, servicios OGC en general.
* Autenticación distinta de usuario/contraseña (certificados, GSSAPI, IAM).
* Gestión de VPN, túneles o `sslmode` configurable desde la interfaz.
* `--check-layers` sobre fuentes `pg:` (el motor ya lo soportaría, pero su
  contrato de agrupación se cierra después).
* Varios esquemas en una sola apertura.

---

## 6. Comandos

### 6.1 Compilar y probar (el bucle del proyecto)

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

### 6.2 Uso de lo nuevo

```bat
:: catálogo y tablas del esquema (necesita contraseña: PGPASSWORD o DPAPI)
set PGPASSWORD=<tu contraseña>
bin\minigis.exe --connections "D:\D\Ftes Mapa Digital\mapadigital_2027\bin\Connections.xml" ^
  --check-pg -l "pg:dataware_FDAC/mgn_2025" --json

:: una capa concreta, sin ventana
bin\minigis.exe --connections "...\Connections.xml" ^
  --info -l "pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_estatales" --json

:: dibujo a PNG
bin\minigis.exe --connections "...\Connections.xml" ^
  -l "pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_estatales" ^
  --snapshot salida.png --size 1200x800

:: captura del diálogo (sin servidor)
bin\minigis.exe --connections "...\Connections.xml" --snapshot-pg-dialog dialogo.png
```

```bat
:: integración real (opt-in), el mismo comando que ejecuta CTest
set MINIGIS_PG_TEST_CONNECTIONS=D:\D\Ftes Mapa Digital\mapadigital_2027\bin\Connections.xml
set MINIGIS_PG_TEST_URI=pg:dataware_FDAC/mgn_2025
ctest --preset windows-msvc-release -R minigis.pg
```

### 6.3 Tiempos de referencia (medidos, Release, LAN)

| Operación | Tiempo |
|---|---|
| Enumerar 5 121 capas (todos los esquemas) | 70 ms |
| Abrir `schemas=mgn_2025` (16 capas) | ~55 ms |
| Abrir `schemas=mgn_2025 tables=poligonos_de_manzana` (1 capa) | ~55 ms |
| `--check-pg` completo (proceso nuevo, 16 tablas, sin releerlas) | ~1,2 s |
| Reabrir el diálogo con la sesión viva y la caché | ~0 ms (sin red) |
| `--bench` de `areas_geoestadisticas_municipales` (p90 por cuadro) | 55 ms |
| `--bench` de `areas_geoestadisticas_basicas_urbanas` (64 808) | ~879 ms (referencia, no puerta) |
| Presupuesto de R3 para el catálogo de esquemas | < 2 s |

Mediciones finales de la implementación (2026-11-20): `--check-pg` con 16 tablas
en **1,2 s**; `--info` de `areas_geoestadisticas_estatales` (32 elementos) en
**~60 ms**; `--bench` de `municipales` con p90 **55 ms** y `budget_ok: true`.

---

## 7. Estructura del proyecto

```
src/core/minigisconnections.h/.cpp      NUEVO  catalogo: leer Connections.xml
src/core/minigispgsource.h/.cpp         NUEVO  URI pg:, cadena de conexion, saneado
src/core/minigispgcatalog.h/.cpp        NUEVO  esquemas/tablas: listar y medir (API C de GDAL)
src/core/minigispgcheck.h/.cpp          NUEVO  informe de --check-pg (texto y JSON)
src/core/minigispgcredentials.h/.cpp    NUEVO  DPAPI (Windows) + cache de sesion
src/core/minigisvectorlayer.h/.cpp      fuente pg: y metadatos de conexion
src/core/minigislayerkind.cpp           reconoce pg: como vectorial
src/gui/minigispostgisdialog.h/.cpp     NUEVO  dialogo (R8)
src/gui/minigismainwindow.h/.cpp        accion de menu, boton, carga y agrupacion
src/app/main.cpp                        --connections, --check-pg, --snapshot-pg-dialog, exit 8
tests/                                  unitarias + integracion opt-in + captura del dialogo
README.md                               tabla de la CLI, codigos de salida, seccion PostGIS
```

Regla del proyecto: al añadir o quitar un archivo fuente hay que editar el
`CMakeLists.txt` de su carpeta. Nunca `file(GLOB ...)`.

---

## 8. Estilo de código

El del proyecto (`AGENTS.md`): C++17, `#pragma once`, namespace `minigis`,
`PascalCase` para tipos, `camelCase` para métodos y variables, prefijo `m` en
miembros privados, `enum class`, archivos `minigis<nombre>.cpp/.h` en
minúsculas, comentarios Doxygen en español sin tildes explicando el **por qué**.
Errores por retorno (`nullptr` + `Error*`), nunca excepciones.

```cpp
/**
 * Traduce una URI pg:conexion/esquema/tabla a la cadena que entiende el driver
 * PostgreSQL de GDAL. La contrasena va aqui y solo aqui: el resultado no se
 * guarda en ajustes, proyectos ni mensajes (ver scrubSecrets).
 */
QString buildConnectionString(const PgSource &source, const PgConnection &connection,
                              const QString &password)
{
  // Las comillas simples de libpq protegen espacios y caracteres raros; se
  // escapan la barra invertida y la comilla, que es lo unico que libpq admite.
  return QStringLiteral("PG:host='%1' port=%2 dbname='%3' user='%4' password='%5' "
                        "connect_timeout=10 sslmode=prefer schemas='%6' tables='%7'")
    .arg(escapeConninfo(connection.server), QString::number(connection.port),
         escapeConninfo(connection.database), escapeConninfo(connection.user),
         escapeConninfo(password), escapeConninfo(source.schema), escapeConninfo(source.table));
}
```

---

## 9. Estrategia de pruebas

### 9.1 Piezas unitarias (sin servidor, siempre en CTest)

* `Connections.xml`: fixtures versionados en `data/sample/postgis/` con 2
  entradas `PGIS`, 1 `GPKG` y contraseñas que parecen cifradas; se comprueba
  que se leen 2, con sus campos, que la `GPKG` se ignora y que `PASSWORD` no
  aparece en ninguna estructura serializada.
* URI: ida y vuelta de `pg:`; rechazo de segmentos vacíos, de más de tres
  segmentos y de `..`; nombres con espacios y con `%`.
* Cadena de conexión: valores con espacio, comilla simple y barra invertida
  quedan escapados; la contraseña aparece una vez y solo en el resultado.
* `scrubSecrets`: `password=x`, `Password='x y'`, mayúsculas y comillas quedan
  en `password=***`; no toca el resto del texto.

### 9.2 Pruebas de CTest

| Prueba | Qué comprueba |
|---|---|
| `minigis.pg-catalogo` | Unitarias de §9.1 (sin red) |
| `minigis.pg-dialogo` | `--snapshot-pg-dialog` con el fixture: PNG no vacío y diálogo con las 2 conexiones |
| `minigis.pg-sin-catalogo` | `--connections` apuntando a un archivo inexistente: exit 2 con mensaje accionable |
| `minigis.pg-conexion-inexistente` | `-l pg:no_existe/s/t` con catálogo válido: exit 8 y nombres disponibles |
| `minigis.pg-cli-uso` | `--check-pg` sin `-l`: exit 1 |
| `minigis.pg` (opt-in) | Con `MINIGIS_PG_TEST_URI`: `--check-pg --json` devuelve `ok: true`, una entrada por tabla de `mgn_2025` y `elapsedMs > 0`; omitida (77) si no hay variables |
| `minigis.pg-bench` (opt-in) | Con `MINIGIS_PG_TEST_LAYER`: `--bench 12 --bench-budget 150 --json` sale con 0, `budget_ok: true` y `store.mode` informado; omitida (77) si no hay variable |

### 9.3 Contrato de `--check-pg`

* `--check-pg -l "pg:<conexion>/<esquema>" [--json]`; sin `-l`, código 1.
* Códigos: `0` correcto, `1` uso, `2` catálogo no encontrado, `3` GDAL no abre
  el esquema/una tabla, `5` esquema sin capas, `8` conexión o autenticación.
* `--json` (aditivo, estable):

```json
{
  "ok": true,
  "connection": { "name": "dataware_FDAC", "server": "10.153.3.32", "port": 5433,
                  "database": "bdginegi", "user": "datawarehouse" },
  "schema": "mgn_2025",
  "layers": [
    { "name": "poligonos_de_manzana", "geometry": "MultiPolygon",
      "features": 2539214, "srid": 6365, "openOk": true }
  ],
  "elapsedMs": 120
}
```

* `ok` es `false` si alguna tabla no abre; el error va en `error.message` y
  `error.hint`, ya saneados.

### 9.4 Matriz requisito → prueba

| Requisito | Prueba |
|---|---|
| R1 | `pg-catalogo`, `pg-sin-catalogo` |
| R2 | `pg-catalogo` (sin contraseña serializada) + revisión manual del GUI |
| R3 | `pg` (opt-in) con el esquema real |
| R4 | `pg` + `--snapshot` manual sobre `mgn_2025` |
| R5 | `pg-catalogo` (URI) y `--info --json` sin `password` |
| R6 | `pg-catalogo` (`scrubSecrets`) |
| R7 | `pg-conexion-inexistente`, `pg-cli-uso`, `pg-sin-catalogo` |
| R8 | `pg-dialogo` + revisión visual |
| R9 | `pg-cli-uso`, `pg` |
| R9, R10 | `pg-bench` |
| R10 | Revisión manual con `frentes_de_manzana` (13,2 M) y `--pg-no-index` |
| R11 | Todas las anteriores |
| R12 | `ctest` completo + comparación con `main` |

---

## 10. Límites

* **Always / siempre:** compilar y pasar `ctest` antes de dar algo por hecho;
  reproducir con fixture antes de arreglar; sanear cualquier mensaje que pueda
  llevar la cadena de conexión; actualizar `README.md` y esta spec si cambia el
  contrato.
* **Ask first / preguntar antes de:** añadir una dependencia (no hace falta:
  GDAL ya trae PostgreSQL y Windows ya trae DPAPI), cambiar códigos de salida,
  tocar el motor de render, cambiar el formato del proyecto.
* **Never / nunca:** descifrar o guardar en claro la contraseña de
  `Connections.xml`; abrir con `GDAL_OF_UPDATE`; poner la cadena de conexión en
  `sourcePath()`; dejar que la contraseña llegue a `stdout`, `stderr`, JSON,
  ajustes o proyecto; tocar libpq directamente.

---

## 11. Criterios de éxito

1. En el GUI: **Archivo > Abrir conexión PostGIS…**, elegir `dataware_FDAC`,
   escribir la contraseña, elegir `mgn_2025` y `areas_geoestadisticas_estatales`,
   y la capa aparece dibujada sobre México con sus 32 elementos, su tabla de
   atributos y su identificar.
2. El mismo resultado por CLI, sin ventana:
   `minigis --info -l "pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_estatales" --json`
   sale con 0, informa driver `PostgreSQL`, 32 elementos y EPSG:6365.
3. `--snapshot` de esa capa produce un PNG no vacío y con la extensión de México.
4. `--check-pg -l "pg:dataware_FDAC/mgn_2025" --json` lista las 16 tablas con su
   geometría y sus elementos, y `ok: true`; tarda menos de 2 s.
5. Con una contraseña incorrecta: exit 8, mensaje con causa y acción, y la
   contraseña **no** aparece en la salida (ni la correcta ni la incorrecta).
6. El catálogo lista las 6 conexiones `PGIS` y no las 2 `GPKG`; sin
   `Connections.xml`, el mensaje dice exactamente qué hacer.
7. Guardar un proyecto con una capa PostGIS avisa de que no se guarda y el
   `.qgz` resultante no contiene credenciales.
8. `ctest --preset windows-msvc-release` entero en verde, y `--info --json` /
   `--snapshot` de shapefile, GeoPackage, GeoJSON, KML y raster no cambian
   respecto de `main`.
9. `--bench 24 -l "pg:dataware_FDAC/mgn_2025/areas_geoestadisticas_municipales"
   --bench-budget 150 --json` sale con 0 y `budget_ok: true` (2 478 polígonos,
   p90 medido 48 ms). `areas_geoestadisticas_basicas_urbanas` (64 808) tarda
   ~879 ms por cuadro: se documenta como referencia, no como puerta. Con
   `--pg-no-index` el JSON informa `store.mode: "none"`.
10. Reabrir **Archivo > Abrir conexión PostGIS…** en la misma sesión no pide
    contraseña ni pulsar Conectar: aparece conectado y con las tablas. Tras
    reiniciar MiniGIS, con «Recordar» marcado (por defecto), se conecta solo.

---

## 12. Supuestos (corregir si alguno no es cierto)

1. El objetivo es **leer y ver** datos PostGIS, no editarlos.
2. La contraseña la conoce el usuario; no hay que descifrarla del
   `Connections.xml` ni de otro archivo de Mapa Digital.
3. El equipo es Windows con DPAPI y MSVC Release (el preset verificado).
4. `Connections.xml` mantiene su formato (`servers/ConStr` con atributos en
   mayúsculas) y su codificación ISO-8859-1.
5. El servidor está en la red local/VPN del usuario; MiniGIS no gestiona la VPN.
6. Solo interesan tablas con columna de geometría registrada en
   `geometry_columns` (lo que lista GDAL por defecto).
7. El servidor de pruebas no está disponible en CI: las pruebas de red son
   opt-in.
8. Las capas PostGIS se usan en la sesión; guardarlas en el proyecto queda para
   una spec posterior.

---

## 13. Preguntas de la revisión (cerradas el 2026-11-20)

* **Q1 — Contraseña.** Resuelto: se pide en el diálogo y se recuerda con DPAPI
  si el usuario marca «Recordar»; la CLI usa la credencial DPAPI y, si no
  existe, `PGPASSWORD`/`.pgpass` (§R2, D3).
* **Q2 — Proyecto `.qgz`.** Resuelto: las capas PostGIS **no** se guardan en v1;
  se avisa y siguen en la sesión. El provider `postgres` de QGIS queda para una
  spec posterior (§D6, §5).
* **Q3 — Tablas de millones.** Resuelto: aviso con recuento y elección entre
  preparar índice o dibujar bajo demanda; en CLI, `--pg-no-index` (§R10).
* **Q4 — Varias tablas por vez.** Resuelto: selección múltiple dentro de un
  esquema; para otro esquema se vuelve a abrir el diálogo (§R3, §R8).
* **Q5 — `--bench` sobre PostGIS.** Resuelto: **sí, en v1**, con `--bench-budget`
  como puerta y `--pg-no-index` para tablas que no caben en el almacén (§R9,
  §11.9).
