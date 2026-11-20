"""Genera los GeoParquet que GDAL no escribe de forma directa.

Casos:
  * sin_crs.parquet      poligonos con `crs` nulo (GeoParquet sin SRC)
  * vacio.parquet        capa con 0 elementos
  * tabla.parquet        tabla Parquet sin geometria (negativo de --check)

Se ejecuta desde scripts\\make_geoparquet_samples.bat. No forma parte del build
ni de ctest; el resultado se versiona en data\\sample\\geoparquet.

Requiere pyarrow y geopandas (el Python de OSGeo4W 3.12 los trae).
"""

import sys
from pathlib import Path

import geopandas as gpd
import pyarrow as pa
import pyarrow.parquet as pq


def main() -> int:
    if len(sys.argv) != 3:
        print("uso: make_geoparquet_samples.py <data/sample> <data/sample/geoparquet>")
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)

    parcels = gpd.read_file(src / "parcels.geojson")

    # Sin SRC: el `crs` del metadato `geo` queda nulo. Sirve para comprobar que
    # MiniGIS avisa en vez de asumir 4326 en silencio.
    sin_crs = parcels.copy()
    sin_crs.crs = None
    sin_crs.to_parquet(dst / "sin_crs.parquet")

    # Capa valida con cero elementos: no es un error de apertura.
    parcels.iloc[0:0].copy().to_parquet(dst / "vacio.parquet")

    # Tabla Parquet sin columna de geometria: no es un GeoParquet.
    tabla = pa.table({"a": [1, 2], "b": ["x", "y"]})
    pq.write_table(tabla, dst / "tabla.parquet")

    print(f"[ok] sin_crs.parquet, vacio.parquet y tabla.parquet en {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
