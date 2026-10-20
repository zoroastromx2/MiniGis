#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MiniGIS - generador determinista de rasters de ejemplo.

Escribe los GeoTIFF que usan las tareas del visor:

  ortho_rgb.tif   256x256, 3 bandas Byte, EPSG:4326 (cuatro cuadrantes)
  dem_utm30.tif   256x256, 1 banda Int16, EPSG:25830 (con NoData)
  landcover.tif   128x128, 1 banda Byte con paleta, EPSG:4326

Con --big DIR escribe ademas terrain_big.tif (4096x4096 con overviews 2-16).

No hay aleatoriedad ni marcas de tiempo: dos ejecuciones con los mismos
argumentos producen archivos con los mismos bytes. Se crea cada banda en un
dataset MEM y se copia a GTiff con CreateCopy (COMPRESS=DEFLATE), sin
estadisticas ni metadatos variables.

Uso:
  python scripts/make_sample_rasters.py [--out DIR] [--big DIR]
"""

import argparse
import os
import sys
from pathlib import Path

try:
    import numpy as np
    from osgeo import gdal, osr
except ImportError as exc:  # pragma: no cover - depende del entorno
    print(f"[error] Falta una dependencia de Python: {exc}", file=sys.stderr)
    print("        Usa el Python de OSGeo4W "
          "(D:\\OSGeo4w\\apps\\Python312\\python.exe).", file=sys.stderr)
    sys.exit(1)

gdal.UseExceptions()

# Salida reproducible: sin PAM (evita .aux.xml con fechas), un solo hilo y
# overviews comprimidas con los mismos parametros que el nivel base.
gdal.SetConfigOption("GDAL_PAM_ENABLED", "NO")
gdal.SetConfigOption("GDAL_NUM_THREADS", "1")
gdal.SetConfigOption("COMPRESS_OVERVIEW", "DEFLATE")
gdal.SetConfigOption("PREDICTOR_OVERVIEW", "2")
gdal.SetConfigOption("ZLEVEL_OVERVIEW", "6")

GTIFF_OPTIONS = [
    "TILED=NO",
    "COMPRESS=DEFLATE",
    "PREDICTOR=2",
    "ZLEVEL=6",
    "BIGTIFF=IF_SAFER",
]

# Extensiones geograficas (xmin, ymin, xmax, ymax).
EXTENT_GEO = (-4.6, 39.8, -3.0, 41.2)
EXTENT_UTM30 = (430000.0, 4470000.0, 431000.0, 4471000.0)


def _srs(epsg):
    """SRC a partir del codigo EPSG, con orden tradicional (x, y)."""
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(epsg)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    return srs


def _geotransform(extent, width, height):
    """Geotransform norte-arriba a partir de la extension y el tamano."""
    xmin, ymin, xmax, ymax = extent
    return (
        xmin,
        (xmax - xmin) / width,
        0.0,
        ymax,
        0.0,
        -(ymax - ymin) / height,
    )


def _remove_sidecars(path):
    """Borra .aux.xml/.ovr previos para que no contaminen la salida nueva."""
    for suffix in (".aux.xml", ".ovr"):
        sidecar = Path(str(path) + suffix)
        if sidecar.exists():
            sidecar.unlink()


def _report(path):
    """Imprime ruta y tamano en bytes de un archivo recien escrito."""
    size = path.stat().st_size
    try:
        shown = Path(os.path.relpath(path))
    except ValueError:  # otra unidad de disco en Windows
        shown = path
    print(f"[ok] {shown} ({size} bytes)")
    return size


def write_geotiff(path, data, epsg, extent, gdal_type, color_interp=None,
                  color_table=None, nodata=None, report=True):
    """Escribe un array numpy como GeoTIFF reproducible.

    data: (alto, ancho) para una banda o (bandas, alto, ancho) si hay varias.
    report=False deja al llamante imprimir el tamano final (p. ej. tras crear
    las overviews).
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    _remove_sidecars(path)

    if data.ndim == 2:
        band_data = [data]
    else:
        band_data = [data[i] for i in range(data.shape[0])]

    height, width = band_data[0].shape
    mem = gdal.GetDriverByName("MEM").Create(
        "", width, height, len(band_data), gdal_type)
    if mem is None:
        raise RuntimeError(f"no se pudo crear el dataset MEM para {path.name}")

    mem.SetGeoTransform(_geotransform(extent, width, height))
    mem.SetProjection(_srs(epsg).ExportToWkt())

    for index, array in enumerate(band_data, start=1):
        band = mem.GetRasterBand(index)
        band.WriteArray(array)
        if nodata is not None:
            band.SetNoDataValue(nodata)
        if color_table is not None:
            band.SetColorTable(color_table)
        if color_interp is not None:
            band.SetColorInterpretation(color_interp[index - 1])

    # CreateCopy conserva geotransform, SRC, NoData, paleta y ColorInterp.
    out = gdal.GetDriverByName("GTiff").CreateCopy(
        str(path), mem, strict=1, options=GTIFF_OPTIONS)
    if out is None:
        raise RuntimeError(f"GDAL no pudo escribir {path}")
    out.FlushCache()
    out = None
    mem = None

    if report:
        _report(path)


def build_ortho(out_dir):
    """Ortofoto RGB de cuatro cuadrantes planos (NO, NE, SO, SE)."""
    height = width = 256
    rgb = np.empty((3, height, width), dtype=np.uint8)

    # Noroeste: rojo.
    rgb[0, :128, :128] = 220
    rgb[1, :128, :128] = 50
    rgb[2, :128, :128] = 50
    # Noreste: verde.
    rgb[0, :128, 128:] = 50
    rgb[1, :128, 128:] = 180
    rgb[2, :128, 128:] = 70
    # Suroeste: azul.
    rgb[0, 128:, :128] = 40
    rgb[1, 128:, :128] = 80
    rgb[2, 128:, :128] = 200
    # Sureste: blanco.
    rgb[0, 128:, 128:] = 245
    rgb[1, 128:, 128:] = 245
    rgb[2, 128:, 128:] = 245

    write_geotiff(
        Path(out_dir) / "ortho_rgb.tif",
        rgb,
        4326,
        EXTENT_GEO,
        gdal.GDT_Byte,
        color_interp=[gdal.GCI_RedBand, gdal.GCI_GreenBand, gdal.GCI_BlueBand],
    )


def build_dem(out_dir):
    """DEM UTM 30N con pendiente modular y un bloque NoData de 16x16."""
    height = width = 256
    rows = np.arange(height, dtype=np.int32)[:, None]
    cols = np.arange(width, dtype=np.int32)[None, :]
    dem = (100 + (rows + cols) % 11).astype(np.int16)

    # Bloque NoData de la esquina superior izquierda (filas y columnas 0..15).
    dem[0:16, 0:16] = -9999
    # Punto de control de valor alto exigido en (10,10): prevalece sobre el
    # bloque anterior, que queda a -9999 salvo ese pixel.
    dem[10, 10] = 1000

    write_geotiff(
        Path(out_dir) / "dem_utm30.tif",
        dem,
        25830,
        EXTENT_UTM30,
        gdal.GDT_Int16,
        nodata=-9999,
    )


def _landcover_color_table():
    """Paleta de 256 entradas: 0 transparente y las tres clases opacas."""
    table = gdal.ColorTable()
    for index in range(256):
        table.SetColorEntry(index, (0, 0, 0, 0))
    table.SetColorEntry(0, (0, 0, 0, 0))
    table.SetColorEntry(1, (70, 140, 70, 255))    # bosque
    table.SetColorEntry(2, (200, 180, 90, 255))   # cultivo
    table.SetColorEntry(3, (70, 110, 180, 255))   # agua
    return table


def build_landcover(out_dir):
    """Ocupacion del suelo en tres franjas verticales con paleta y NoData=0."""
    height = width = 128
    values = np.empty((height, width), dtype=np.uint8)
    values[:, 0:42] = 1
    values[:, 42:85] = 2
    values[:, 85:] = 3
    values[0:8, 0:8] = 0  # NoData en la esquina superior izquierda

    write_geotiff(
        Path(out_dir) / "landcover.tif",
        values,
        4326,
        EXTENT_GEO,
        gdal.GDT_Byte,
        color_interp=[gdal.GCI_PaletteIndex],
        color_table=_landcover_color_table(),
        nodata=0,
    )


def build_big(big_dir):
    """Raster grande 4096x4096 con patron fijo y overviews 2, 4, 8 y 16."""
    size = 4096
    index = np.arange(size, dtype=np.uint32) // 8
    terrain = ((index[:, None] + index[None, :]) % 256).astype(np.uint8)

    path = Path(big_dir) / "terrain_big.tif"
    write_geotiff(path, terrain, 4326, EXTENT_GEO, gdal.GDT_Byte, report=False)

    dataset = gdal.Open(str(path), gdal.GA_Update)
    if dataset is None:
        raise RuntimeError(f"no se pudo reabrir {path} para las overviews")
    dataset.BuildOverviews("AVERAGE", [2, 4, 8, 16])
    overview_count = dataset.GetRasterBand(1).GetOverviewCount()
    dataset.FlushCache()
    dataset = None

    if overview_count != 4:
        raise RuntimeError(
            f"{path.name}: se esperaban 4 overviews y hay {overview_count}")

    _report(path)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Genera los GeoTIFF de ejemplo de MiniGIS (determinista).")
    parser.add_argument(
        "--out", metavar="DIR",
        help="directorio de los tres GeoTIFF pequenos "
             "(por defecto data/sample/rasters)")
    parser.add_argument(
        "--big", metavar="DIR",
        help="directorio donde crear terrain_big.tif (4096x4096, con overviews)")
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[1]
    out_dir = Path(args.out) if args.out else root / "data" / "sample" / "rasters"

    print(f"[minigis] Generando GeoTIFF de ejemplo en {out_dir}...")
    build_ortho(out_dir)
    build_dem(out_dir)
    build_landcover(out_dir)

    if args.big:
        print(f"[minigis] Generando terrain_big.tif en {args.big}...")
        build_big(Path(args.big))

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # mensaje claro y codigo de salida 1
        print(f"[error] No se pudieron generar los rasters: {exc}", file=sys.stderr)
        sys.exit(1)
