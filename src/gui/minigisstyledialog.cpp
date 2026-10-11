#include "minigisstyledialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace minigis
{
namespace
{

void applyColorToButton(QPushButton *button, const QColor &color)
{
  if (!button)
    return;

  button->setText(color.name(QColor::HexRgb).toUpper());
  button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                          .arg(color.name(QColor::HexRgb),
                               color.lightness() > 128 ? QStringLiteral("#202020") : QStringLiteral("#f0f0f0")));
}

} // namespace

StyleDialog::StyleDialog(const QString &layerName, const Style &style, QWidget *parent)
  : QDialog(parent)
  , mStyle(style)
  , mInitialStyle(style)
{
  buildUi(layerName);
}

void StyleDialog::buildUi(const QString &layerName)
{
  setWindowTitle(QStringLiteral("Simbologia de «%1»").arg(layerName));

  mFillButton = new QPushButton(this);
  mStrokeButton = new QPushButton(this);
  connect(mFillButton, &QPushButton::clicked, this, &StyleDialog::chooseFillColor);
  connect(mStrokeButton, &QPushButton::clicked, this, &StyleDialog::chooseStrokeColor);

  mFillCheck = new QCheckBox(QStringLiteral("Rellenar las geometrias"), this);
  mFillCheck->setChecked(mStyle.fillEnabled);

  mStrokeWidthSpin = new QDoubleSpinBox(this);
  mStrokeWidthSpin->setRange(0.0, 20.0);
  mStrokeWidthSpin->setSingleStep(0.25);
  mStrokeWidthSpin->setDecimals(2);
  mStrokeWidthSpin->setValue(mStyle.strokeWidth);

  mPointSizeSpin = new QDoubleSpinBox(this);
  mPointSizeSpin->setRange(1.0, 60.0);
  mPointSizeSpin->setSingleStep(0.5);
  mPointSizeSpin->setDecimals(1);
  mPointSizeSpin->setValue(mStyle.pointSize);
  mPointSizeSpin->setToolTip(QStringLiteral("Diametro en pixeles de los puntos (solo capas de puntos)"));

  mOpacitySlider = new QSlider(Qt::Horizontal, this);
  mOpacitySlider->setRange(0, 100);
  mOpacitySlider->setValue(mStyle.opacity);

  auto *opacityRow = new QHBoxLayout;
  opacityRow->addWidget(mOpacitySlider, 1);
  auto *opacityValue = new QLabel(QStringLiteral("%1 %").arg(mStyle.opacity), this);
  opacityRow->addWidget(opacityValue);
  connect(mOpacitySlider, &QSlider::valueChanged, opacityValue,
          [opacityValue](int value) { opacityValue->setText(QStringLiteral("%1 %").arg(value)); });

  auto *form = new QFormLayout;
  form->addRow(QStringLiteral("Color de relleno"), mFillButton);
  form->addRow(QString(), mFillCheck);
  form->addRow(QStringLiteral("Color de contorno"), mStrokeButton);
  form->addRow(QStringLiteral("Grosor del contorno"), mStrokeWidthSpin);
  form->addRow(QStringLiteral("Tamano del punto"), mPointSizeSpin);
  form->addRow(QStringLiteral("Opacidad"), opacityRow);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  auto *resetButton = buttons->addButton(QStringLiteral("Restablecer"), QDialogButtonBox::ResetRole);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(resetButton, &QPushButton::clicked, this, &StyleDialog::restoreInitialStyle);

  auto *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(buttons);

  updateColorButtons();
  resize(380, sizeHint().height());
}

void StyleDialog::chooseFillColor()
{
  const QColor chosen = QColorDialog::getColor(mStyle.fillColor, this, QStringLiteral("Color de relleno"));
  if (chosen.isValid())
  {
    mStyle.fillColor = chosen;
    updateColorButtons();
  }
}

void StyleDialog::chooseStrokeColor()
{
  const QColor chosen = QColorDialog::getColor(mStyle.strokeColor, this, QStringLiteral("Color de contorno"));
  if (chosen.isValid())
  {
    mStyle.strokeColor = chosen;
    updateColorButtons();
  }
}

void StyleDialog::updateColorButtons()
{
  applyColorToButton(mFillButton, mStyle.fillColor);
  applyColorToButton(mStrokeButton, mStyle.strokeColor);
}

void StyleDialog::restoreInitialStyle()
{
  mStyle = mInitialStyle;
  mFillCheck->setChecked(mStyle.fillEnabled);
  mStrokeWidthSpin->setValue(mStyle.strokeWidth);
  mPointSizeSpin->setValue(mStyle.pointSize);
  mOpacitySlider->setValue(mStyle.opacity);
  updateColorButtons();
}

Style StyleDialog::style() const
{
  Style result = mStyle;
  result.fillEnabled = mFillCheck->isChecked();
  result.strokeWidth = mStrokeWidthSpin->value();
  result.pointSize = mPointSizeSpin->value();
  result.opacity = mOpacitySlider->value();
  return result;
}

} // namespace minigis
