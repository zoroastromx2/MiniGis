#pragma once

#include <QDialog>
#include <QStringList>

#include "minigisrasterstyle.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSlider;

namespace minigis
{

/**
 * Dialogo de estilo de una capa raster: bandas, estiramiento de contraste,
 * muestreo, NoData y opacidad. Sigue el patron de `StyleDialog`: modal, sin
 * vista previa (el resultado se ve en el lienzo al aceptar) y con
 * *Restablecer* para volver a los valores con los que se abrio.
 */
class RasterStyleDialog : public QDialog
{
  Q_OBJECT

public:
  RasterStyleDialog(const QString &layerName, const QStringList &bandLabels, const RasterStyle &style,
                    QWidget *parent = nullptr);

  /** Lee los controles y devuelve el estilo resultante. */
  RasterStyle style() const;

private:
  void buildUi(const QString &layerName, const QStringList &bandLabels);
  void updateEnabledState();
  void restoreInitialStyle();
  void fillBandCombos(const QStringList &bandLabels);

  RasterStyle mStyle;
  RasterStyle mInitialStyle;

  QComboBox *mModeCombo = nullptr;
  QComboBox *mRedCombo = nullptr;
  QComboBox *mGreenCombo = nullptr;
  QComboBox *mBlueCombo = nullptr;
  QComboBox *mAlphaCombo = nullptr;
  QComboBox *mStretchCombo = nullptr;
  QComboBox *mResamplingCombo = nullptr;
  QCheckBox *mCustomRangeCheck = nullptr;
  QCheckBox *mNoDataCheck = nullptr;
  QDoubleSpinBox *mMinSpin = nullptr;
  QDoubleSpinBox *mMaxSpin = nullptr;
  QDoubleSpinBox *mPercentLowSpin = nullptr;
  QDoubleSpinBox *mPercentHighSpin = nullptr;
  QSlider *mOpacitySlider = nullptr;
};

} // namespace minigis
