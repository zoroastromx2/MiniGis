#pragma once

#include <QString>

namespace minigis
{

/**
 * Configuracion de dibujado de una capa raster.
 *
 * Es el equivalente de `Style` para el vectorial: se guarda en la capa, y
 * cambiarla sube la revision para que el lienzo invalide su cache de cuadro.
 * En esta primera version cubre la seleccion de bandas, el estiramiento de
 * contraste, el muestreo y la opacidad.
 */
struct RasterStyle
{
  /** Como se combinan las bandas para producir color. */
  enum class BandMode
  {
    Grayscale, // una banda, replica en R/G/B
    Rgb        // tres bandas (o mas) con seleccion R/G/B
  };

  /**
   * Estiramiento de contraste aplicado a cada banda. Por defecto no se estira:
   * las imagenes Byte se dibujan con sus valores originales y el estiramiento
   * queda como una eleccion del usuario en el dialogo de estilo.
   */
  enum class Stretch
  {
    None,        // Byte tal cual; otros tipos, normalizacion por tipo
    MinMax,      // minimo y maximo de la muestra
    PercentClip  // recorte de percentiles (2-98 por defecto)
  };

  enum class Resampling
  {
    Nearest,
    Bilinear
  };

  BandMode bandMode = BandMode::Rgb;
  /** Bandas 1-based; en modo Gray solo se usa `redBand`. */
  int redBand = 1;
  int greenBand = 2;
  int blueBand = 3;
  /** Banda alfa (0 = ninguna). */
  int alphaBand = 0;

  Stretch stretch = Stretch::None;
  double percentLow = 2.0;
  double percentHigh = 98.0;

  /** Rango manual para `MinMax`; si `useCustomRange` es false se usa la muestra. */
  bool useCustomRange = false;
  double minValue = 0.0;
  double maxValue = 1.0;

  Resampling resampling = Resampling::Bilinear;
  /** Opacidad 0..100. */
  int opacity = 100;
  /** Si es true, los valores NoData del archivo se dibujan transparentes. */
  bool useNodata = true;

  bool operator==(const RasterStyle &other) const
  {
    return bandMode == other.bandMode && redBand == other.redBand && greenBand == other.greenBand
           && blueBand == other.blueBand && alphaBand == other.alphaBand && stretch == other.stretch
           && percentLow == other.percentLow && percentHigh == other.percentHigh
           && useCustomRange == other.useCustomRange && minValue == other.minValue
           && maxValue == other.maxValue && resampling == other.resampling && opacity == other.opacity
           && useNodata == other.useNodata;
  }
  bool operator!=(const RasterStyle &other) const { return !(*this == other); }
};

} // namespace minigis
