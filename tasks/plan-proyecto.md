# Implementation Plan: Proyectos QGIS (`.qgz`/`.qgs`)

> Plan técnico derivado de [SPEC-proyecto.md](../SPEC-proyecto.md), aprobado el
> 2026-11-03 con estas decisiones: `Ctrl+Shift+O` para abrir proyecto y `Ctrl+O`
> sigue abriendo capas; se guardan las capas añadidas y se quitan las cerradas;
> copia `.bak` en cada guardado; nombre de capa OGR; y rutas del entorno
> actualizadas (Qt 6.10.3, vcpkg `D:\VCPKG`).
> Tareas concretas y criterios de aceptación en
> [todo-proyecto.md](todo-proyecto.md).
> **Estado: T0–T8 implementadas; checkpoints A y B superados y C casi completo
> (2026-11-03)** — 24/24 pruebas; guardado de cambios y capas nuevas/cerradas
> verificados también en QGIS. Falta la prueba manual del diálogo de cierre.

## 1. Resumen

MiniGIS sabe abrir capas y dibujarlas, pero no conserva nada de una sesión. Este
trabajo añade el concepto de **proyecto QGIS**: leer el `.qgz`/`.qgs`, reconstruir
en el mapa lo que muestra (capas, orden, visibilidad, SRC, encuadre, simbología,
etiquetas, estilo ráster) y volver a escribir el archivo con los cambios del
usuario, conservando todo lo que MiniGIS no interpreta.

El trabajo se corta en **rebanadas verticales** que dejan el programa compilando
y las 18 pruebas actuales verdes:

1. infraestructura de lectura (XML y ZIP) y `--project-info`;
2. abrir el proyecto en la ventana (cargador por capa y aplicación de estado);
3. guardar sin cambios (ida y vuelta fiel del archivo);
4. guardar cambios (visibilidad, simbología, etiquetas, ráster, encuadre);
5. añadir y quitar capas, estado «modificado» y contrato completo;
6. puerta de compatibilidad con QGIS y documentación.

## 2. Arquitectura

```
┌────────────────────────────── src/gui ──────────────────────────────────┐
│ MainWindow: acciones, título, estado modificado, diálogos, recientes    │
│   ProjectSnapshot <- MapCanvas (capas, orden, visibilidad, estilos,     │
│                       etiquetas, RasterStyle, extent)                   │
└───────────────┬─────────────────────────────────────────┬───────────────┘
                │ QgisProject::load / apply                │ save(snapshot)
                ▼                                          ▼
┌────────────────────────────── src/core ─────────────────────────────────┐
│ QgisProject                                                             │
│   · capas del proyecto (id, datasource, provider, tipo, visible)        │
│   · SRC y encuadre del mapcanvas                                        │
│   · nodos conocidos; el resto se conserva                               │
│   ├── QgisSymbology: Style/LabelStyle/RasterStyle <-> subárbol XML      │
│   ├── XmlNode: árbol conservador (QXmlStreamReader / QXmlStreamWriter)  │
│   └── ZipArchive: minizip, lee y reescribe todas las entradas           │
└─────────────────────────────────────────────────────────────────────────┘
                ▲                                          ▲
                │ capas ya abiertas                        │
┌───────────────┴──────────────────────────────────────────┴──────────────┐
│ VectorLayerLoader (variante por capa concreta) + VectorLayer/RasterLayer│
└─────────────────────────────────────────────────────────────────────────┘
```

**Abrir:** se detecta `.qgz`/`.qgs` antes que una capa → `ZipArchive` extrae la
entrada `.qgs` (o se lee el archivo plano) → `XmlNode` lo interpreta →
`QgisProject::load` resuelve SRC, encuadre, árbol y capas → `MainWindow` pide al
cargador las capas en el orden del proyecto → al llegar cada una aplica
visibilidad y estilos y la inserta en su sitio → al terminar fija el SRC y el
encuadre, repinta una vez y actualiza título y avisos.

**Guardar:** `MainWindow` construye un `ProjectSnapshot` desde el lienzo (fondo a
frente) → `QgisProject::save` actualiza solo los nodos conocidos, añade las capas
nuevas arriba y quita las cerradas → serializa el XML → `ZipArchive` escribe un
ZIP completo en un temporal → copia el original a `.bak` → sustituye el original.
Si algo falla, el original no se toca y el estado sigue modificado.

## 3. Decisiones de implementación

| Decisión | Motivo |
|---|---|
| `minizip` con `find_package(MINIZIP CONFIG REQUIRED)` y `MINIZIP::minizip` | Es C estable, ya está instalado por GDAL, permite copiar entradas y controlar el nombre de la entrada `.qgs`. `QZipWriter` es API privada de Qt |
| Añadir `minizip` a `vcpkg/vcpkg.json` | Documenta la dependencia directa aunque el modo manifiesto esté desactivado |
| `XmlNode` con `QXmlStreamReader`/`QXmlStreamWriter` | Evita `Qt6::Xml`; conserva atributos ordenados, hijos, comentarios y texto. El DTD no se resuelve (no hay red) y se reemite la misma línea `<!DOCTYPE qgis ...>` |
| Sin declaración XML al escribir | QGIS escribe primero el `DOCTYPE`; reproducirlo mantiene el archivo familiar y evita diferencias innecesarias |
| `QgisProject::load` devuelve las capas **de fondo a frente** | Es el orden de `MapCanvas`; invertir en la lectura deja el apilado igual que en QGIS |
| El orden de las capas existentes no se regenera | MiniGIS no permite reordenar; reescribir el orden desde el lienzo cambiaría el proyecto sin que el usuario lo pidiera. Las nuevas se insertan arriba |
| `datasource` se resuelve una vez (`ProjectDatasource`) | Misma lógica para abrir, comparar y reescribir rutas; tolera `|layername=`, `/`, `\`, `file://` y rutas relativas |
| El cargador gana `startRequests(QVector<LayerRequest>)` | Hay que cargar una capa concreta de un GeoPackage, en orden y sin expandir todo el archivo. `start(QStringList)` se implementa encima y no cambia |
| `MainWindow` asocia cada capa viva con el `id` de su entrada | Evita emparejar por nombre; las capas nuevas no tienen id hasta guardar |
| Un solo `ProjectSnapshot` para firma y guardado | El estado que se compara es exactamente el que se escribe; no hay dos definiciones de «proyecto» |
| Firma con hash de un texto canónico (`'g',17`) | Detecta cambios reales, no reescrituras de formato; volver al estado guardado vuelve a «limpio» |
| Guardado atómico: temporal en la misma carpeta, `.bak` y sustitución | Un `.qgz` truncado es pérdida de datos; el temporal y la copia permiten recuperarse |
| «Guardar como» recalcula rutas si cambia la carpeta | Un proyecto movido con rutas relativas quedaría roto; se resuelve contra la ruta original y se vuelve a relativizar |
| Etiquetas: leer `simple` y `basic`, escribir `simple` | QGIS 3.40 escribe `simple` y lo lee; `basic` (3.44) solo se interpreta. Escribir `basic` exigiría modelar estilos múltiples |
| Plantilla de `maplayer` nuevo copiada de una capa del mismo tipo | Hereda atributos que QGIS espera sin inventarlos; si no hay ninguna, se usa una constante revisada contra QGIS |
| `styleRepresentable=false` en renderizadores no soportados | Se evita destruir un categorizado/graduado al ocultar o guardar; el usuario puede sustituirlo si edita el estilo |
| Capas no disponibles fuera del lienzo pero dentro del XML | Un archivo movido no puede borrar su capa del proyecto |
| Código de salida 7 propio | Distingue «el proyecto no se pudo abrir/guardar» de «la capa no se pudo abrir» (3) y del uso incorrecto (1) |
| La puerta de QGIS es un script PyQGIS opcional en CTest | Valida el formato real sin convertir QGIS en dependencia de compilación |

## 4. Orden de implementación

```
T0 entorno (hecho)
      │
T1 fixture QGIS
      │
      ▼
T2 XmlNode + ZipArchive
      │
      ▼
T3 carga del proyecto + --project-info            ◀ checkpoint A
      │
      ▼
T4 abrir proyecto en la ventana                   ◀ checkpoint B1
      │
      ▼
T5 guardar sin cambios (ZIP, .bak, atómico)       ◀ checkpoint B2
      │
      ▼
T6 guardar cambios de estado                      ◀ checkpoint C1
      │
      ▼
T7 capas nuevas y cerradas
      │
      ▼
T8 estado modificado, título y diálogos           ◀ checkpoint C2
      │
      ▼
T9 contrato CLI completo (--check-project)        ◀ checkpoint D
      │
      ▼
T10 puerta de QGIS
      │
      ▼
T11 README, spec y cierre                         ◀ checkpoint E
```

### Fase 0 — Entorno (T0)

Ya ejecutada: `env.bat`, presets y README apuntan a las rutas reales (Qt 6.10.3
en `C:\Qtwin`, vcpkg `D:\VCPKG`, SDK `D:\Windows Kits`). Configuración limpia,
compilación y 18/18 verificados el 2026-11-03.

### Fase A — Lectura (T1–T3)

Fixture creado con PyQGIS, `XmlNode`, `ZipArchive`, `QgisProject::load` y
`--project-info` con su prueba. Al final de la fase se puede describir un
proyecto sin abrir ventana.

### Fase B — Abrir y guardar sin cambios (T4–T5)

`MainWindow` abre el proyecto (cargador por capa, SRC, extensión, estilos,
visibilidad), y guarda una copia idéntica en `.qgz`/`.qgs` conservando entradas y
XML desconocido. Aquí se valida que el archivo guardado se puede volver a abrir
en MiniGIS y en QGIS.

### Fase C — Cambios y ciclo de vida (T6–T8)

Se escriben simbología, etiquetas, estilo ráster y encuadre; se añaden y quitan
capas; el título y el diálogo de cierre reflejan el estado modificado.

### Fase D — Contrato y compatibilidad (T9–T10)

`--check-project` con las 27 comprobaciones, `--save-project`, códigos de error y
la puerta de QGIS sobre el proyecto modificado.

### Fase E — Cierre (T11)

README con opciones, límites y verificación; el spec pasa a «implementado» con
los resultados reales.

## 5. Índice de tareas

Detalle completo (criterios, verificación, archivos y tamaño) en
[todo-proyecto.md](todo-proyecto.md).

| # | Tarea | Fase | Dep. | Tamaño |
|---|---|---|---|---|
| T0 | Entorno: rutas reales de Qt, vcpkg y SDK | 0 | — | S (hecho) |
| T1 | Fixture QGIS `.qgz`/`.qgs` generado con PyQGIS | A | T0 | S (hecho) |
| T2 | `XmlNode` y `ZipArchive` (minizip) | A | T1 | M (hecho) |
| T3 | Carga del proyecto, simbología de lectura y `--project-info` | A | T2 | L (hecho) |
| T4 | Abrir proyecto en la ventana y aplicar su estado | B | T3 | L (hecho) |
| T5 | Guardar sin cambios (ZIP, `.bak`, escritura atómica) | B | T4 | L (hecho) |
| T6 | Guardar cambios de simbología, etiquetas, ráster y encuadre | C | T5 | L (hecho) |
| T7 | Capas nuevas y cerradas, ids y plantillas | C | T6 | M (hecho) |
| T8 | Estado modificado, título, diálogos y atajos | C | T7 | M (hecho; diálogo pendiente de prueba manual) |
| T9 | Contrato CLI completo (`--check-project`, `--save-project`) | D | T8 | M |
| T10 | Puerta de compatibilidad con QGIS (PyQGIS) | D | T9 | S |
| T11 | README, spec y verificación final | E | T10 | S |

## 6. Riesgos y mitigaciones

| Riesgo | Impacto | Mitigación |
|---|---|---|
| `minizip` con API nueva (minizip 4) incompatible con `zip.h`/`unzip.h` clásicos | Alto | Verificar las cabeceras en T2 y encapsular todo en `ZipArchive`; si la API instalada no expone el API clásico, usar el lector/escritor de vcpkg compat (documentado) antes de escribir el resto |
| Un `maplayer` generado no lo abre QGIS | Alto | Copiar la plantilla de una capa real del mismo tipo, reemplazar solo lo conocido y validar con la puerta de T10 antes de dar T7 por cerrada |
| Pérdida de XML desconocido al serializar | Alto | `XmlNode` conserva todo; las comprobaciones 12–14 comparan antes/después y fallan si se toca un renderizador ajeno |
| El orden de apilado se invierte | Medio | Invertir en lectura y escritura de forma simétrica; comprobación 1 y captura |
| QGIS interpreta `<layerorder>` al revés | Medio | Supuesto 13 del spec: el árbol manda; `<layerorder>` solo se actualiza para añadir/quitar ids. La puerta de QGIS lo confirma |
| Conversión de escala y halo imprecisa | Medio | Fórmulas documentadas, tolerancia del 1 % y prueba de ida y vuelta |
| Escritura no atómica en Windows (rename sobre archivo existente) | Alto | Temporal + `.bak` + borrar/renombrar con restauración; prueba 24 con destino imposible |
| Cargador entrega capas fuera de orden y el apilado cambia | Medio | Peticiones en orden y comprobación de orden; si el cargador emite desordenado, `MainWindow` inserta por índice |
| Mezcla de moc/DLLs de dos Qt al cambiar de versión | Medio | T0 ya reconstruyó limpio; documentar en README y no tocar el build entre tareas |
| El fixture depende de QGIS y no está en todos los equipos | Bajo | El fixture se versiona; el script de generación es opcional y la puerta de QGIS también |
| El `.qgz` guardado crece o cambia compresión | Bajo | Se copian las entradas ajenas; el `.qgs` se reescribe y se comprime con deflate, como QGIS |

## 7. Paralelización

* **T1** (fixture) es independiente del código: se puede hacer a la vez que T2.
* **T10** (script de QGIS) se puede escribir en paralelo a T6–T9, pero se valida
  después de T9, cuando existe el artefacto modificado.
* **T11** se redacta con T10 en marcha, pero se cierra al final.
* Todo lo demás es secuencial: T2 → T3 → T4 → T5 → T6 → T7 → T8 → T9 comparten
  `minigisqgisproject.*` y `MainWindow`.

## 8. Puntos de control

| Checkpoint | Después de | Exige |
|---|---|---|
| A — Lectura | T3 | `--project-info` del fixture informa 3 capas vectoriales + 1 ráster, la rota, SRC y extensión; 18/18 (hecho: 20/20) |
| B1 — Abrir | T4 | El proyecto se abre en la ventana con orden, visibilidad, estilos y encuadre; captura con las 4 filas (hecho; también TCAR de 18 capas) |
| B2 — Guardar | T5 | Guardar una copia sin cambios y reabrirla da el mismo estado; entradas ajenas y XML desconocido intactos; `.bak` (hecho; QGIS 3.40 reabre la copia) |
| C1 — Cambios | T6 | Visibilidad, simbología, etiquetas, ráster y encuadre se guardan y reabren (hecho; QGIS lo confirma) |
| C2 — Ciclo de vida | T8 | Añadir y quitar capas deja el proyecto coherente; el título y el diálogo de cierre reflejan los cambios (hecho salvo prueba manual del diálogo) |
| D — Contrato | T9 | `--check-project` con las 27 comprobaciones en `OK`; códigos 2/7/1 |
| E — Cierre | T11 | Puerta de QGIS en verde, SC1–SC14 verificados, README y spec al día |

Cada checkpoint implica **revisión humana** antes de seguir, salvo autorización
expresa para continuar.

## 9. Preguntas abiertas para esta revisión

Ninguna bloqueante. Las respuestas 6, 7, 9 y 10 del spec (§14) no se discutieron
explícitamente y se mantienen las propuestas por defecto: sin proyecto nuevo
desde cero, puerta de QGIS manual + opcional en CTest, un diálogo de detalle al
abrir y escala de etiquetas aproximada.

La única decisión técnica que conviene revisar es la **escritura atómica con
`.bak`** (orden temporal → copia → sustitución) porque toca el archivo del
usuario; está en §3 y en la comprobación 24.
