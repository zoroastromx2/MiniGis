# -*- coding: utf-8 -*-
"""Genera el fixture de proyecto QGIS de MiniGIS (``data/sample/projects``).

El proyecto se crea con PyQGIS para que el XML sea el que escribe QGIS de
verdad, y despues se parchea para anadir lo que la interfaz anade al guardar
(``<mapcanvas>``), una capa rota y un nodo desconocido que MiniGIS debe
conservar.

Uso:
    D:\\OSGeo4w\\bin\\python-qgis-ltr.bat tests\\qgis\\make_project_fixture.py
"""

import os
import re
import shutil
import sys
import zipfile

from qgis.core import (
    QgsApplication,
    QgsCategorizedSymbolRenderer,
    QgsCoordinateReferenceSystem,
    QgsFillSymbol,
    QgsMultiBandColorRenderer,
    QgsPalLayerSettings,
    QgsProject,
    QgsRasterLayer,
    QgsRectangle,
    QgsReferencedRectangle,
    QgsRendererCategory,
    QgsSingleSymbolRenderer,
    QgsTextBufferSettings,
    QgsTextFormat,
    QgsVectorLayer,
    QgsVectorLayerSimpleLabeling,
)
from qgis.PyQt.QtGui import QColor, QFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SAMPLE = os.path.join(ROOT, "data", "sample")
OUT_DIR = os.path.join(SAMPLE, "projects")
QGS_NAME = "qgis_ejemplo.qgs"
QGZ_NAME = "qgis_ejemplo.qgz"
QGS_ENTRY = "qgis_ejemplo.qgs"

MISSING_ID = "no_existe_00000000_0000_0000_0000_000000000000"
EXTENT = (430000.0, 4460000.0, 455000.0, 4485000.0)  # EPSG:25830, Madrid


def make_layer(path, name, provider):
    layer = QgsVectorLayer(path, name, provider) if provider == "ogr" else QgsRasterLayer(path, name, provider)
    if not layer.isValid():
        raise RuntimeError("No se pudo abrir la capa de ejemplo: %s" % path)
    return layer


def set_single_fill(layer, fill, outline, width_mm):
    symbol = layer.renderer().symbol().clone()
    symbol.setColor(QColor(*fill))
    symbol_layer = symbol.symbolLayer(0)
    symbol_layer.setStrokeColor(QColor(*outline))
    symbol_layer.setStrokeWidth(width_mm)
    layer.setRenderer(QgsSingleSymbolRenderer(symbol))


def set_categorized(layer, field):
    categories = []
    for value, color in (
        ("residencial", "#e41a1c"),
        ("industrial", "#377eb8"),
        ("verde", "#4daf4a"),
        ("mixto", "#984ea3"),
    ):
        symbol = QgsFillSymbol.createSimple({"color": color, "outline_color": "#333333"})
        categories.append(QgsRendererCategory(value, symbol, value))
    layer.setRenderer(QgsCategorizedSymbolRenderer(field, categories))


def set_labels(layer):
    settings = QgsPalLayerSettings()
    settings.fieldName = "name"
    settings.placement = QgsPalLayerSettings.Placement.OverPoint
    text_format = QgsTextFormat()
    text_format.setFont(QFont("Noto Sans"))
    text_format.setSize(10)
    text_format.setColor(QColor(20, 20, 20))
    buffer = QgsTextBufferSettings()
    buffer.setEnabled(True)
    buffer.setSize(1.5)
    buffer.setColor(QColor(255, 255, 255, 220))
    text_format.setBuffer(buffer)
    settings.setFormat(text_format)
    settings.scaleVisibility = True
    if hasattr(settings, "minimumScale"):
        settings.minimumScale = 5000
        settings.maximumScale = 250000
    else:  # QGIS < 3.40 usaba los nombres scaleMin/scaleMax
        settings.scaleMin = 5000
        settings.scaleMax = 250000
    layer.setLabeling(QgsVectorLayerSimpleLabeling(settings))
    layer.setLabelsEnabled(True)


def relativize(text, filename, relative):
    text = re.sub(
        r"<datasource>[^<]*%s</datasource>" % re.escape(filename),
        "<datasource>%s</datasource>" % relative,
        text,
    )
    text = re.sub(
        r'source="[^"]*%s"' % re.escape(filename),
        'source="%s"' % relative,
        text,
    )
    return text


def patch_project(text):
    """Anade <mapcanvas>, la capa rota y el nodo desconocido; deja rutas relativas."""
    crs_block = re.search(r"<projectCrs>\s*(<spatialrefsys.*?</spatialrefsys>)\s*</projectCrs>", text, re.S)
    if not crs_block:
        raise RuntimeError("El proyecto generado no tiene <projectCrs>")
    spatialrefsys = crs_block.group(1)

    mapcanvas = """  <mapcanvas name="theMapCanvas" annotationsVisible="1">
    <units>meters</units>
    <extent>
      <xmin>%s</xmin>
      <ymin>%s</ymin>
      <xmax>%s</xmax>
      <ymax>%s</ymax>
    </extent>
    <rotation>0</rotation>
    <destinationsrs>
      %s
    </destinationsrs>
    <rendermaptile>0</rendermaptile>
    <expressionContextScope/>
  </mapcanvas>
""" % (EXTENT[0], EXTENT[1], EXTENT[2], EXTENT[3], spatialrefsys)

    if "<mapcanvas" in text:
        raise RuntimeError("El proyecto ya tenia <mapcanvas>; revisa el parcheo")
    text = text.replace("  <projectlayers>", mapcanvas + "  <projectlayers>", 1)

    vector_block = re.search(r"    <maplayer[^>]*type=\"vector\".*?</maplayer>", text, re.S)
    if not vector_block:
        raise RuntimeError("No hay ninguna capa vectorial para clonar la capa rota")
    missing = vector_block.group(0)
    missing = re.sub(r"<id>.*?</id>", "<id>%s</id>" % MISSING_ID, missing, count=1, flags=re.S)
    missing = re.sub(
        r"<datasource>.*?</datasource>",
        "<datasource>../shapefiles/no_existe.shp</datasource>",
        missing,
        count=1,
        flags=re.S,
    )
    missing = re.sub(
        r"<layername>.*?</layername>",
        "<layername>no_existe</layername>",
        missing,
        count=1,
        flags=re.S,
    )
    text = text.replace("  </projectlayers>", missing + "\n  </projectlayers>", 1)

    tree_entry = """    <layer-tree-layer checked="Qt::Unchecked" providerKey="ogr" legend_split_behavior="0" legend_exp="" patch_size="-1,-1" id="%s" name="no_existe" expanded="1" source="../shapefiles/no_existe.shp">
      <customproperties>
        <Option/>
      </customproperties>
    </layer-tree-layer>
""" % MISSING_ID
    text = re.sub(r"(  </layer-tree-group>\s*\n)", tree_entry + r"\1", text, count=1)

    text = text.replace(
        "  </layerorder>",
        '    <layer id="%s"/>\n  </layerorder>' % MISSING_ID,
        1,
    )

    unknown = """  <minigisPrueba version="1">
    <nota>conservar</nota>
  </minigisPrueba>
"""
    text = text.replace("</qgis>", unknown + "</qgis>", 1)

    for filename, relative in (
        ("cities.shp", "../shapefiles/cities.shp"),
        ("parcels.shp", "../shapefiles/parcels.shp"),
        ("ortho_rgb.tif", "../rasters/ortho_rgb.tif"),
        ("no_existe.shp", "../shapefiles/no_existe.shp"),
    ):
        text = relativize(text, filename, relative)

    return text


def rewrite_qgz(qgz_path, qgs_text):
    """Sustituye la entrada .qgs del .qgz conservando el resto de entradas."""
    temporary = qgz_path + ".tmp"
    with zipfile.ZipFile(qgz_path, "r") as source, zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as target:
        found = False
        for entry in source.infolist():
            data = source.read(entry.filename)
            if entry.filename.lower().endswith(".qgs"):
                data = qgs_text.encode("utf-8")
                found = True
            target.writestr(entry, data)
        if not found:
            target.writestr(QGS_ENTRY, qgs_text.encode("utf-8"))
    os.replace(temporary, qgz_path)


# El fixture local reutiliza el WKT de 25830 quitandole el ID de la raiz: es un
# SRC valido de GDAL/QGIS sin codigo de autoridad, el caso que hace fallar a
# quien guarda el encuadre sin transformarlo.
def make_local_crs_project(text):
    """Copia el fixture con el SRC del lienzo como WKT local (sin codigo EPSG)."""
    match = re.search(r"<projectCrs>.*?<wkt>(.*?)</wkt>", text, re.S)
    if not match:
        raise RuntimeError("El fixture no tiene WKT de proyecto")
    wkt = match.group(1).strip().replace(',ID["EPSG",25830]', "")

    spatial = ('<spatialrefsys nativeFormat="Wkt"><wkt>%s</wkt><srid>0</srid>'
               '<authid></authid><description>SRC local de prueba</description>'
               '<geographicflag>false</geographicflag></spatialrefsys>' % wkt)
    text = re.sub(r"<projectCrs>.*?</projectCrs>", "<projectCrs>%s</projectCrs>" % spatial,
                  text, count=1, flags=re.S)
    text = re.sub(r"<destinationsrs>.*?</destinationsrs>", "<destinationsrs>%s</destinationsrs>" % spatial,
                  text, count=1, flags=re.S)
    return text


def check_project(path):
    """Comprueba que QGIS reabre el fixture con las capas y valores esperados."""
    project = QgsProject()
    if not project.read(path):
        raise RuntimeError("QGIS no pudo reabrir %s" % path)

    names = [node.name() for node in project.layerTreeRoot().children()]
    expected = ["cities", "parcels_categorizado", "parcels", "orto"]
    for name in expected:
        if name not in names:
            raise RuntimeError("Falta la capa %s en el arbol: %s" % (name, names))

    if project.crs().authid() != "EPSG:25830":
        raise RuntimeError("SRC del proyecto inesperado: %s" % project.crs().authid())

    cities = project.mapLayersByName("cities")[0]
    if not cities.labelsEnabled():
        raise RuntimeError("La capa cities no tiene etiquetas activas")
    label_settings = cities.labeling().settings()
    if hasattr(label_settings, "minimumScale"):
        scale_min = label_settings.minimumScale
        scale_max = label_settings.maximumScale
    else:
        scale_min = label_settings.scaleMin
        scale_max = label_settings.scaleMax
    if scale_min != 5000 or scale_max != 250000:
        raise RuntimeError("Las escalas de las etiquetas no se leyeron: %s/%s" % (scale_min, scale_max))

    parcels = project.mapLayersByName("parcels")[0]
    if parcels.renderer().type() != "singleSymbol":
        raise RuntimeError("Simbolologia inesperada en parcels: %s" % parcels.renderer().type())

    categorized = project.mapLayersByName("parcels_categorizado")[0]
    if categorized.renderer().type() != "categorizedSymbol":
        raise RuntimeError("Simbolologia inesperada en parcels_categorizado")


def main():
    if not os.path.isdir(os.path.join(SAMPLE, "shapefiles")):
        print("[error] No existe data/sample/shapefiles; ejecuta scripts/make_sample_data.bat", file=sys.stderr)
        return 1

    os.makedirs(OUT_DIR, exist_ok=True)

    app = QgsApplication([], False)
    app.initQgis()
    try:
        project = QgsProject.instance()
        project.clear()
        project.setCrs(QgsCoordinateReferenceSystem("EPSG:25830"))

        cities = make_layer(os.path.join(SAMPLE, "shapefiles", "cities.shp"), "cities", "ogr")
        circle = cities.renderer().symbol().clone()
        circle.setColor(QColor(30, 120, 200))
        circle.symbolLayer(0).setStrokeColor(QColor(20, 40, 60))
        circle.symbolLayer(0).setStrokeWidth(0.3)
        circle.symbolLayer(0).setSize(2.5)
        cities.setRenderer(QgsSingleSymbolRenderer(circle))
        set_labels(cities)

        parcels = make_layer(os.path.join(SAMPLE, "shapefiles", "parcels.shp"), "parcels", "ogr")
        set_single_fill(parcels, (200, 80, 40, 255), (80, 20, 10, 255), 0.5)

        categorized = make_layer(os.path.join(SAMPLE, "shapefiles", "parcels.shp"), "parcels_categorizado", "ogr")
        set_categorized(categorized, "uso")

        ortho = make_layer(os.path.join(SAMPLE, "rasters", "ortho_rgb.tif"), "orto", "gdal")
        renderer = QgsMultiBandColorRenderer(ortho.dataProvider(), 1, 2, 3)
        renderer.setOpacity(0.8)
        ortho.setRenderer(renderer)

        project.addMapLayers([cities, categorized, parcels, ortho])
        project.layerTreeRoot().findLayer(categorized.id()).setItemVisibilityChecked(False)

        project.viewSettings().setDefaultViewExtent(
            QgsReferencedRectangle(
                QgsRectangle(EXTENT[0], EXTENT[1], EXTENT[2], EXTENT[3]),
                QgsCoordinateReferenceSystem("EPSG:25830"),
            )
        )

        qgz_path = os.path.join(OUT_DIR, QGZ_NAME)
        if not project.write(qgz_path):
            raise RuntimeError("QGIS no pudo escribir %s" % qgz_path)

        with zipfile.ZipFile(qgz_path, "r") as archive:
            qgs_entry = next(name for name in archive.namelist() if name.lower().endswith(".qgs"))
            text = archive.read(qgs_entry).decode("utf-8")
        text = patch_project(text)
        rewrite_qgz(qgz_path, text)

        qgs_path = os.path.join(OUT_DIR, QGS_NAME)
        with open(qgs_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)

        # Variante con SRC local sin codigo EPSG (mismo contenido y extension).
        local_text = make_local_crs_project(text)
        local_qgz = os.path.join(OUT_DIR, "qgis_local.qgz")
        with zipfile.ZipFile(qgz_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        with zipfile.ZipFile(local_qgz, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in entries.items():
                if name.lower().endswith(".qgs"):
                    data = local_text.encode("utf-8")
                archive.writestr(name, data)
        with open(os.path.join(OUT_DIR, "qgis_local.qgs"), "w", encoding="utf-8", newline="\n") as handle:
            handle.write(local_text)

        check_project(qgz_path)
        check_project(qgs_path)

        # El proyecto local no se valida con QGIS (WKT sin autoridad): solo se
        # usa para comprobar que MiniGIS conserva su encuadre al guardar.

        print("[ok] fixture escrito: %s" % qgz_path)
        print("[ok] fixture escrito: %s" % qgs_path)
        print("[ok] capas: cities, parcels_categorizado (oculta), parcels, orto, no_existe (rota)")
        return 0
    finally:
        app.exitQgis()


if __name__ == "__main__":
    sys.exit(main())
