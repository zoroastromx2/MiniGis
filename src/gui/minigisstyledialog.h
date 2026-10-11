#pragma once

#include <QDialog>

#include "minigisstyle.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;

namespace minigis
{

/** Dialogo sencillo de simbologia: color de relleno, contorno, grosor y opacidad. */
class StyleDialog : public QDialog
{
  Q_OBJECT

public:
  StyleDialog(const QString &layerName, const Style &style, QWidget *parent = nullptr);

  /** Lee los controles y devuelve el estilo resultante. */
  Style style() const;

private:
  void buildUi(const QString &layerName);
  void chooseFillColor();
  void chooseStrokeColor();
  void updateColorButtons();
  void restoreInitialStyle();

  Style mStyle;
  Style mInitialStyle;
  QPushButton *mFillButton = nullptr;
  QPushButton *mStrokeButton = nullptr;
  QDoubleSpinBox *mStrokeWidthSpin = nullptr;
  QDoubleSpinBox *mPointSizeSpin = nullptr;
  QSlider *mOpacitySlider = nullptr;
  QCheckBox *mFillCheck = nullptr;
};

} // namespace minigis
