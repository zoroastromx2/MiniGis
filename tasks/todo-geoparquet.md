# Tareas: apertura y visualización de GeoParquet

Plan: `tasks/plan-geoparquet.md` · Spec: `SPEC-geoparquet.md`

Estado: **completa — T0–T7 hechas y verificadas (2026-11-20)**. 51 pruebas
CTest: 48 en verde y las 3 de PostGIS omitidas sin servidor; 12 son de
GeoParquet.

| Tarea | Estado | Evidencia |
|---|---|---|
| T0 motor | hecha | `gdal[parquet]` instalado en 16 min (Arrow 26.0.0); `--list-drivers` muestra `Parquet` y `Arrow`; `09Man` abre en 109 ms |
| T1 fixtures | hecha | 8 `.parquet` + `sin_geometria.geojson` en `data/sample/geoparquet`; `scripts\make_geoparquet_samples.bat` los regenera y se verificaron con `ogrinfo` |
| T2 apertura y errores | hecha | tabla sin geometría → exit 5; carpeta → exit 3 con «carpeta con archivos .parquet»; `parcels.parquet` → exit 0; sin regresiones |
| T3 `--check-geoparquet` | hecha | `parcels`: `ok: true`, 5/5; `09Man`: 67 375/67 375, `ok: true`, 1,13 s; `sin_crs`: EPSG 0 sin asumir 4326 |
| T4 CLI | hecha | opción y ayuda; códigos 0/1/2/3/4/5; 12 pruebas de CTest |
| T5 interfaz | hecha | filtro `*.parquet *.geoparquet`; `--snapshot-window` con 09Man y tabla: 2,2 s, 67 375 elementos y 10 campos; Identificar devuelve una manzana |
| T6 CTest | hecha | `ctest --preset windows-msvc-release`: **51 pruebas, 0 fallos, 3 omitidas** |
| T7 documentación | hecha | README (formatos, CLI, vcpkg, problemas), `RENDIMIENTO.md` §14 con las medidas, spec en «aprobada e implementada» |

---

## Task 0: compilar GDAL con Parquet y medir la línea base

**Description:** Instalar la dependencia que falta y comprobar que el motor
vectorial actual ya abre los archivos del usuario. No se escribe código de
MiniGIS en esta tarea; sirve para separar el problema del driver del contrato que
se va a añadir.

**Acceptance criteria:**
- [ ] `D:\VCPKG\vcpkg.exe list` muestra `gdal` con la feature `parquet` y
      `arrow`/`parquet` instalados.
- [ ] `bin\minigis.exe --list-drivers` incluye `Parquet`.
- [ ] `--info --json 09Mun.parquet` abre con driver `Parquet`, 16 elementos,
      `MEXICO_ITRF_2008_LCC`, `crs.epsg: 0`.
- [ ] `--info --json 09Man.parquet` abre con 67 375 elementos y 10 campos, sin
      columnas `geometry_bbox.*`.
- [ ] `--snapshot` de `09Mun` (1600×900) no sale vacío y tarda menos de 2 s.
- [ ] Tiempos de apertura, `--info` y snapshot anotados en el plan/spec.

**Verification:**
- [ ] `D:\VCPKG\vcpkg.exe list | findstr /i "gdal arrow parquet snappy thrift"`
- [ ] `bin\minigis.exe --info --json "D:\F\IRIS 4.0\2026\GeoParquet\09Mun.parquet"`
- [ ] `bin\minigis.exe --info --json "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet"`
- [ ] `ctest --preset windows-msvc-release --output-on-failure` sigue en verde.

**Dependencies:** ninguna.

**Files likely touched:**
- `vcpkg/vcpkg.json` (feature ya declarada)
- `README.md` (el comando de instalación se documenta en T7)

**Estimated scope:** S de código; L de espera de compilación.

---

## Task 1: fixtures GeoParquet y script de generación

**Description:** Crear los archivos pequeños y deterministas que usarán las
pruebas, y el script que los regenera. Los fixtures se versionan; el script no se
ejecuta en `ctest`.

**Acceptance criteria:**
- [ ] Existen en `data/sample/geoparquet/`:
      `parcels.parquet` (5 polígonos, EPSG:4326, 4 campos, sin FID nombrado),
      `parcels_utm30.parquet` (5 polígonos, EPSG:25830),
      `mixto.parquet` (punto + línea + polígono),
      `sin_geometria.parquet` (2 filas, 1 con geometría nula),
      `sin_crs.parquet` (5 polígonos, CRS nulo),
      `tabla.parquet` (2 filas, sin geometría),
      `vacio.parquet` (0 elementos),
      `parcels_fid.parquet` (FID nombrado `parcel_id`).
- [ ] Cada uno abre con OSGeo4W GDAL y coincide con lo esperado (elementos,
      tipo de geometría, SRC, FID).
- [ ] `scripts/make_geoparquet_samples.bat` regenera todos los archivos y
      documenta la versión de GDAL/Python usada.
- [ ] `mixto.parquet` conserva una `GEOMETRYCOLLECTION` con punto, línea y
      polígono (mismo caso que el KML mixto).
- [ ] Ningún fixture ocupa más de 100 KB (salvo decisión contraria).

**Verification:**
- [ ] `D:\OSGeo4w\bin\ogrinfo.exe -al -so data\sample\geoparquet\parcels.parquet`
- [ ] `... sin_crs.parquet` informa `(unknown)`.
- [ ] `... tabla.parquet` informa `Geometry: None`.
- [ ] `... mixto.parquet` informa `Unknown (any)`.

**Dependencies:** ninguna (OSGeo4W tiene driver de escritura). Idealmente en
paralelo con T0.

**Files likely touched:**
- `data/sample/geoparquet/*.parquet` (nuevos)
- `scripts/make_geoparquet_samples.bat` y, si hace falta,
  `scripts/make_geoparquet_samples.py` (nuevos)

**Estimated scope:** M (2–3 archivos + 8 fixtures).

---

## Task 2: apertura, rechazos y errores accionables

**Description:** Enseñar a `VectorLayer` a distinguir un GeoParquet de una tabla
Parquet, de una carpeta y de un binario sin driver, sin tocar el resto de
formatos.

**Acceptance criteria:**
- [ ] Si el archivo termina en `.parquet`/`.geoparquet` y
      `GDALGetDriverByName("Parquet")` es nulo, el error dice que este GDAL no
      incluye el driver y sugiere `vcpkg install "gdal[parquet]" --triplet
      x64-windows`.
- [ ] Una carpeta devuelve error con la acción «abre el archivo .parquet, no la
      carpeta» y explica que las colecciones particionadas no están soportadas.
- [ ] Una capa con `OGR_L_GetGeomType() == wkbNone` y 0 campos de geometría se
      rechaza con «es un archivo Parquet, pero no contiene geometría» y una
      acción concreta; el código de salida de la CLI es **5**.
- [ ] La señal de «sin geometría» no se confunde con un fallo de GDAL (código
      3); la CLI la propaga como **5**.
- [ ] `parcels.parquet`, `mixto.parquet` y `sin_geometria.parquet` siguen
      abriendo; un GeoParquet vacío abre con 0 elementos.
- [ ] El resto de formatos no cambia de comportamiento.

**Verification:**
- [ ] `bin\minigis.exe --info data\sample\geoparquet\tabla.parquet` → exit 5 y
      mensaje esperado.
- [ ] `bin\minigis.exe --info data\sample\geoparquet` → mensaje de carpeta.
- [ ] `bin\minigis.exe --info data\sample\geoparquet\parcels.parquet` → exit 0.
- [ ] `ctest --preset windows-msvc-release --output-on-failure`.

**Dependencies:** T0 (driver).

**Files likely touched:**
- `src/core/minigisvectorlayer.h`, `.cpp`
- `src/app/main.cpp` (mapeo de la señal a exit 5)
- `tests/run_cli_test.cmake` (casos de error, en T6)

**Estimated scope:** M (3 archivos).

---

## Task 3: `--check-geoparquet` en core

**Description:** Crear el diagnóstico que abre el archivo, cuenta la geometría
por elemento, construye el almacén y compara las dos orillas, como
`--check-kml` pero sin estilos.

**Acceptance criteria:**
- [ ] `inspectGeoParquet(path, &report, &error)` rellena por capa: `name`,
      `features`, `withoutGeometry`, `unsupportedParts`, `points`, `lines`,
      `polygons`, `storeFeatures`, `missingInStore`, `crsEpsg`, `crsName`,
      `crsDescription`, `extent`, `extent4326`, `fields`, `fidColumn`,
      `attributeCache`.
- [ ] `toJson()` produce el contrato del spec §R7; `toText()` es legible.
- [ ] `ok()` es `true` solo si el almacén conserva todos los elementos
      dibujables y no hay partes no soportadas; con el almacén no construido
      (`storeFeatures == -1`) no se afirma que falten.
- [ ] `parcels.parquet`: 5, `ok: true`, `crsEpsg: 4326`.
- [ ] `09Man.parquet`: 67 375, `ok: true`, `storeFeatures: 67375`.
- [ ] `sin_crs.parquet`: `crsEpsg: 0`, `crsName` vacío, `ok: true` (abre, no se
      asume 4326).
- [ ] `sin_geometria.parquet`: `withoutGeometry: 1`, `ok: true`.
- [ ] `mixto.parquet`: `points`, `lines` y `polygons` suman las partes de la
      `GEOMETRYCOLLECTION` y del polígono suelto, sin perder ninguna.

**Verification:**
- [ ] `bin\minigis.exe --check-geoparquet --json data\sample\geoparquet\parcels.parquet`
- [ ] `bin\minigis.exe --check-geoparquet --json "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet"`
- [ ] `bin\minigis.exe --check-geoparquet --json data\sample\geoparquet\sin_crs.parquet`

**Dependencies:** T0, T1, T2.

**Files likely touched:**
- `src/core/minigisgeoparquetinspect.h`, `.cpp` (nuevos)
- `src/core/CMakeLists.txt`
- `src/app/main.cpp` (llamada, en T4)

**Estimated scope:** L (2 archivos nuevos + CMake).

---

## Task 4: integración en la CLI

**Description:** Exponer el diagnóstico como `--check-geoparquet`, con `--json`,
ayuda y códigos de salida estables, y hacer que `--info` devuelva 5 para una
tabla sin geometría.

**Acceptance criteria:**
- [ ] `--help` documenta `--check-geoparquet` con un ejemplo.
- [ ] `--check-geoparquet --json archivo.parquet` escribe solo JSON en stdout;
      `stderr` queda para el diagnóstico.
- [ ] Sin capa: exit 1. Archivo inexistente: exit 2. GDAL no abre: exit 3.
      Informe no `ok`: exit 4. Sin capas/sin geometría: exit 5.
- [ ] `--info` de `tabla.parquet` devuelve 5, no 3.
- [ ] `--info`, `--snapshot` y `--bench` de un GeoParquet siguen funcionando.

**Verification:**
- [ ] `bin\minigis.exe --check-geoparquet --json data\sample\geoparquet\parcels.parquet`
- [ ] `bin\minigis.exe --check-geoparquet` (sin capa) → 1.
- [ ] `bin\minigis.exe --check-geoparquet data\sample\geoparquet\no-existe.parquet` → 2.
- [ ] `bin\minigis.exe --info data\sample\geoparquet\tabla.parquet` → 5.

**Dependencies:** T3.

**Files likely touched:**
- `src/app/main.cpp`
- `README.md` (tabla de la CLI, en T7)

**Estimated scope:** M (1 archivo grande).

---

## Task 5: diálogo, arrastrar y soltar y ventana

**Description:** Que un `.parquet` se pueda elegir y soltar como cualquier otra
capa, y comprobar que la carga de `09Man` no bloquea la interfaz.

**Acceptance criteria:**
- [ ] El filtro de **Abrir capa** incluye `*.parquet` y `*.geoparquet` en
      «Capas», «Vectoriales» y una entrada propia.
- [ ] Arrastrar `09Mun.parquet` al lienzo lo abre y lo encuadra en CDMX.
- [ ] El panel muestra `09Man.parquet` con 67 375 elementos y el tipo
      `Multi Polygon`.
- [ ] La tabla de atributos muestra los 10 campos y se desplaza sin congelar la
      ventana.
- [ ] Identificar sobre una manzana devuelve sus atributos.
- [ ] Captura `--snapshot-window` con la capa y la tabla.

**Verification:**
- [ ] `bin\minigis.exe --snapshot-window ventana.png -l "D:\F\IRIS 4.0\2026\GeoParquet\09Man.parquet" --size 1600x900`
- [ ] Prueba manual de arrastrar y soltar y del diálogo.

**Dependencies:** T2, T4.

**Files likely touched:**
- `src/gui/minigismainwindow.cpp`
- `data/sample/geoparquet` (fixtures de T1)

**Estimated scope:** S (1 archivo).

---

## Task 6: pruebas CTest

**Description:** Convertir el contrato en pruebas automáticas sin ventana.

**Acceptance criteria:**
- [ ] `minigis.geoparquet-info`: `--info --json` con driver `Parquet`, 5
      elementos y `crs.epsg` 4326.
- [ ] `minigis.geoparquet-snapshot`: PNG 1200×800 válido y no vacío.
- [ ] `minigis.geoparquet-check`: `ok: true`, `storeFeatures == features`,
      `unsupportedParts == 0`.
- [ ] `minigis.geoparquet-mixto`: punto + línea + polígono conservados.
- [ ] `minigis.geoparquet-sin-geometria`: `withoutGeometry: 1`.
- [ ] `minigis.geoparquet-sin-crs`: `crsEpsg: 0`, sin asumir 4326.
- [ ] `minigis.geoparquet-tabla`: exit 5 y mensaje «no es GeoParquet».
- [ ] `minigis.geoparquet-carpeta`: mensaje que pide el archivo.
- [ ] `minigis.geoparquet-vacio`: 0 elementos, `ok: true`.
- [ ] `minigis.geoparquet-fid`: `fidColumn` correcto.
- [ ] `minigis.rendimiento` y el resto siguen en verde.

**Verification:**
- [ ] `ctest --preset windows-msvc-release --output-on-failure`
- [ ] `ctest --preset windows-msvc-release -R geoparquet --output-on-failure`

**Dependencies:** T1, T3, T4.

**Files likely touched:**
- `tests/CMakeLists.txt`
- `tests/run_cli_test.cmake` (modos nuevos o script propio)

**Estimated scope:** M (2 archivos).

---

## Task 7: documentación, medición y cierre

**Description:** Dejar el cambio comprensible para quien llegue después y
actualizar la spec con lo aprendido.

**Acceptance criteria:**
- [ ] README: GeoParquet en la lista de formatos, en la tabla de la CLI, en los
      ejemplos y en solución de problemas (feature `parquet`, síntoma «driver
      no disponible»).
- [ ] README: comando `vcpkg install "gdal[parquet]" --triplet x64-windows` y
      nota de que el preset usa el árbol instalado en modo clásico.
- [ ] `RENDIMIENTO.md`: nota con la medición de `09Man` (apertura, almacén,
      snapshot) si aporta una cifra nueva.
- [ ] `SPEC-geoparquet.md`: estado «aprobada e implementada», tiempos finales,
      modo de instalación real y aclaración del CRS de GeoPandas.
- [ ] `tasks/todo-geoparquet.md`: todas las tareas marcadas con su evidencia.
- [ ] Los 11 criterios de éxito del spec comprobados uno por uno.

**Verification:**
- [ ] `ctest --preset windows-msvc-release --output-on-failure`
- [ ] Revisión manual de README y spec.

**Dependencies:** T0–T6.

**Files likely touched:**
- `README.md`
- `RENDIMIENTO.md`
- `SPEC-geoparquet.md`
- `tasks/todo-geoparquet.md`

**Estimated scope:** S (4 documentos).
