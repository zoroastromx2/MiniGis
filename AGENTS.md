# AGENTS.md

## Que es esto

MiniGIS: visor SIG minimo en C++17 con Qt 6 y GDAL/OGR. Un solo ejecutable de
consola `minigis` que carga capas vectoriales, las dibuja y admite modos sin
ventana (`--info`, `--snapshot`, `--check-*`) para verificar todo sin pantalla.

La referencia completa (compilar, depurar en Qt Creator, CLI, solucion de
problemas, decisiones de diseno) esta en `README.md`; el detalle de rendimiento,
en `RENDIMIENTO.md`. No las dupliques aqui: leelas cuando las necesites.

## Bucle de trabajo

Compilar y probar con Qt 6.10.3 y el preset `windows-msvc-release` (en este equipo
vcpkg solo tiene la variante Release de GDAL; `windows-msvc-debug` necesitaria la
variante Debug, que no esta instalada):

```bat
scripts\configure.bat windows-msvc-release
scripts\build.bat     windows-msvc-release
ctest --preset windows-msvc-release --output-on-failure
```

- El ejecutable sale en `build\<preset>\bin\minigis.exe`; para lanzarlo usa
  `scripts\run.bat windows-msvc-release` o `bin\minigis.exe <args>`.
- Si anades o quitas un archivo fuente, edita tambien el `CMakeLists.txt` de su
  carpeta. Las listas de fuentes son explicitas: nunca uses `file(GLOB ...)`.

## Normas que debes respetar

### Arquitectura

- Tres capas: `src/core` (datos y render, sin widgets), `src/gui` (widgets),
  `src/app` (ejecutable). `minigis_core` no puede depender de `src/gui` ni de
  widgets.
- GDAL se usa solo por la **API C de OGR** (`OGR_L_*`, `OGR_F_*`), nunca las
  clases C++ de `OGRLayer`: es la superficie estable entre versiones.

### Manejo de errores

- Todo fallo reportable devuelve `minigis::Error` con `message` (que paso) y
  `hint` (que hacer). Regla: un error sobre el que el usuario no puede actuar es
  un defecto. Ver `src/core/minigiserror.h`.
- Los fallos se propagan por retorno (`nullptr` + `Error*`), no por excepciones.

### Convenciones de codigo

- Comentarios en **espanol, sin tildes** (ASCII), en estilo Doxygen (`/** */`)
  sobre clases y metodos publicos. Explica el por que, no el que.
- Tipos y clases en `PascalCase`; metodos y variables en `camelCase`; miembros
  privados con prefijo `m` (`mSourcePath`); enumerados `enum class`; namespace
  `minigis`.
- Archivos `minigis<nombre>.cpp/.h`, todo en minusculas y pegado. C++17,
  `#pragma once`.

### Contrato de la CLI

- Codigos de salida estables consumidos por las pruebas (`0` exito, `1` uso,
  `2` archivo no encontrado, `3` GDAL no abre, `4` no escribe imagen, `5` sin
  capas, `6` supera presupuesto de rendimiento). Tabla completa en `README.md`
  seccion 6; no los cambies sin actualizar `tests/`.
- `stdout` = datos, `stderr` = diagnostico (`-V`).

### Render y rendimiento

- El dibujo es la ruta caliente; esta medido y con puerta de regresion
  (`minigis.rendimiento`). Antes de tocar el motor de render, lee
  `RENDIMIENTO.md`.
- Todo estado que afecta a como se dibuja una capa (simbologia, visibilidad,
  almacen de geometrias) debe pasar por `VectorLayer::revision()` /
  `bumpRevision()`: el lienzo cachea el cuadro y esa revision es la clave de su
  cache. No invalides la cache a mano.

### Pruebas

- Pruebas de humo con CTest (`tests/`, via `run_cli_test.cmake`), en plataforma
  `offscreen`.
- Cualquier comportamiento verificable debe tener un modo sin ventana
  (`--info`, `--snapshot`, `--check-*`) para poder probarlo; es como se testea
  el motor de datos y el de render.
