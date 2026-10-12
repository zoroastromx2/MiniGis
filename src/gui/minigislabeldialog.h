#pragma once

#include <QDialog>
#include <QVector>

#include "minigislabelstyle.h"
#include "minigisvectorlayer.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace minigis
{

class LabelPreview;

/**
 * Dialogo de etiquetas de una capa.
 *
 * Agrupa todo lo que hace falta para un etiquetado utilizable:
 *
 *   * **Campos**: que campos se muestran, en que orden y con que separador.
 *   * **Fuente**: familia, tamano, negrita, cursiva, color y color de halo.
 *   * **Escala**: en que margen de escala aparecen y si el tamano se mide en
 *     puntos de pantalla (habitual) o en unidades del mapa.
 *   * **Colocacion**: respecto al elemento, con desplazamiento, distancia minima
 *     entre etiquetas y tamano minimo del elemento.
 *   * **Saturacion**: limite de etiquetas, descarte de textos repetidos y
 *     prioridad.
 *
 * Incluye una vista previa que dibuja el texto con la configuracion actual sobre
 * un fondo de color, para comprobar de un vistazo el halo y el contraste.
 */
class LabelDialog : public QDialog
{
  Q_OBJECT

public:
  LabelDialog(const QString &layerName, const QVector<VectorLayer::FieldInfo> &fields,
              const LabelStyle &style, double currentMapUnitsPerPixel, QWidget *parent = nullptr);

  /** Configuracion resultante de los controles. */
  LabelStyle labelStyle() const;

private:
  void buildUi(const QString &layerName);
  void fillFields(const QVector<VectorLayer::FieldInfo> &fields, const LabelStyle &style);
  void updatePreview();
  void chooseTextColor();
  void chooseHaloColor();
  void useCurrentScaleAsLimit(bool maximum);
  void moveSelectedField(int delta);

  /** Lee el estado actual de los controles. */
  LabelStyle collect() const;

  /**
   * Tamano que tendra la etiqueta en pixeles con la configuracion actual.
   * Es la unica forma de que el usuario entienda que significa el numero cuando el
   * tamano se mide en unidades del mapa.
   */
  double currentPixelSize() const;

  /** Ajusta el rango del control de tamano al modo elegido. */
  void updateSizeRange(LabelStyle::SizeMode mode);

  /** Convierte el tamano al cambiar de modo, para no cambiar el tamano aparente. */
  void onSizeModeChanged(int index);

  /** Actualiza la etiqueta que muestra el tamano en pixeles y la vista previa. */
  void updateSizeReadout();

  QVector<VectorLayer::FieldInfo> mFields;

  QCheckBox *mEnabledCheck = nullptr;
  QListWidget *mFieldList = nullptr;
  QPushButton *mFieldUpButton = nullptr;
  QPushButton *mFieldDownButton = nullptr;
  QLineEdit *mSeparatorEdit = nullptr;

  QFontComboBox *mFamilyCombo = nullptr;
  QDoubleSpinBox *mSizeSpin = nullptr;
  QCheckBox *mBoldCheck = nullptr;
  QCheckBox *mItalicCheck = nullptr;
  QPushButton *mColorButton = nullptr;

  QCheckBox *mHaloCheck = nullptr;
  QPushButton *mHaloColorButton = nullptr;
  QDoubleSpinBox *mHaloWidthSpin = nullptr;

  QComboBox *mSizeModeCombo = nullptr;
  QDoubleSpinBox *mMinimumScaleSpin = nullptr;
  QDoubleSpinBox *mMaximumScaleSpin = nullptr;

  QComboBox *mPlacementCombo = nullptr;
  QDoubleSpinBox *mOffsetXSpin = nullptr;
  QDoubleSpinBox *mOffsetYSpin = nullptr;
  QDoubleSpinBox *mMinimumDistanceSpin = nullptr;
  QDoubleSpinBox *mMinimumFeatureSizeSpin = nullptr;

  QSpinBox *mMaximumLabelsSpin = nullptr;
  QCheckBox *mDiscardDuplicatesCheck = nullptr;
  QSpinBox *mPrioritySpin = nullptr;

  LabelPreview *mPreview = nullptr;
  QLabel *mSizeReadout = nullptr;
  double mCurrentMapUnitsPerPixel = 0.0;
  /** Modo de tamano del que se viene, para convertir al cambiarlo. */
  LabelStyle::SizeMode mLastSizeMode = LabelStyle::SizeMode::Screen;
};

} // namespace minigis
