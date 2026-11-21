# Implementation Plan: rendimiento de capas vectoriales grandes

Spec: [`SPEC-rendimiento-capas-grandes.md`](../SPEC-rendimiento-capas-grandes.md).
Tareas: [`tasks/todo-rendimiento.md`](todo-rendimiento.md).

## Overview

Hacer que la capa de rios topograficos de LINZ (`nz-river-centrelines-topo-150k.parquet`,
711 574 lineas, ~13,5 M de vertices reales, 226,5 MB) se abra en segundos y se manipule con
fluidez, mejorando la ruta compartida (almacen, render y tabla de atributos) para que sirva
tambien a shapefile y GeoPackage.

Punto de partida medido: 21 s por cuadro, 20,9 s el `--snapshot`, 34 s la ventana sin tabla y
531 s con tabla, `--check-geoparquet` sin construir almacen.

## Architecture Decisions

* **D1 — Almacen de 1 GB duro.** `GeometryStore::Options` gana `budgetBytes` (1 GB por defecto,
  `MINIGIS_STORE_BUDGET_MB` lo baja) y el limite de vertices se deriva de el, con el tope
  historico de 8 M como suelo. Se construye una vez y el render consulta el R-tree y los niveles.
* **D2 — La simplificacion distingue lineas de poligonos.** El minimo de 4 vertices por anillo
  solo vale para poligonos; una polilinea se queda con sus 2 extremos. Sin esto, cada rio
  simplificado a 2 puntos se restauraba entero y los niveles no reducian nada.
* **D3 — Elementos sub-pixel como puntos.** En modo memoria, un elemento cuya envolvente mide
  menos de `minimumGeometryPixels` en pantalla se dibuja como punto (muestreado con `dotBudget`)
  en lugar de construir un trazado. Es la diferencia entre 0,9 s y 50 ms con 711 000 elementos
  en pantalla, y visualmente indistinguible por debajo de 2 px.
* **D4 — Tabla por paginas.** `AttributeTableModel` pide paginas contiguas de 128 filas
  (`VectorLayer::fieldValuesPage`, `OGR_L_SetNextByIndex` + `GetNextFeature`) y cachea 4
  paginas. El acceso aleatorio por FID (una lectura de milisegundos en Parquet) desaparece del
  camino de pintado.
* **D5 — Sin dependencias nuevas.** Todo con GDAL/OGR C y Qt; no Arrow directo.
* **D6 — Diagnostico en `--bench --json`.** Se anaden los tiempos por fase del almacen
  (`read_ms`, `index_ms`, `levels_ms`) sin cambiar los campos existentes.
* **D7 — Puerta opt-in.** La capa no se versiona; la prueba de rendimiento se registra siempre y
  se omite sin `MINIGIS_GEOPARQUET_BENCH_DATASET`.

## Fases y verificacion

| Fase | Cambio | Verificacion |
|---|---|---|
| F1 | Presupuesto de 1 GB y limite de vertices | `--check-geoparquet` informa `storeFeatures: 711574`; `-V` imprime vertices y MB |
| F2 | Simplificacion de lineas y sub-pixel | `--bench 24` mediana ≤ 200 ms; `--snapshot` render ≤ 0,4 s |
| F3 | Tabla por paginas | `--snapshot-window --with-attributes` ≤ 12 s; primera pagina ≤ 2 s |
| F4 | Diagnostico `--bench` | JSON con `read_ms`, `index_ms`, `levels_ms`; `ctest` en verde |
| F5 | Pruebas y documentacion | prueba opt-in + README/RENDIMIENTO con cifras |

## Riesgos

* El cambio de layout de `GeometryStore::Options` exige **compilacion limpia**: el MSVC en
  espanol no registra dependencias de cabeceras y quedan objetos viejos (sintoma: presupuesto
  basura y corrupcion de heap).
* La nube de puntos muestreada puede dejar el mapa demasiado ralo a escala nacional; el umbral
  (`minimumGeometryPixels`, 2 px) y `dotBudget` (150 000) se ajustan midiendo.
* La tabla secuencial puede no soportar saltos grandes; si `SetNextByIndex` falla se recorre
  desde el principio, que sigue siendo una sola pasada.
