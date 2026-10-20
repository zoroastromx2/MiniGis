#include "minigisrasterstyledialog.h"

#include <QCheckBox>
#include <QComboBox>
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

void addModeItem(QComboBox *combo, const QString &text, RasterStyle::BandMode mode)
{
  combo->addItem(text, static_cast<int>(mode));
}

void addStretchItem(QComboBox *combo, const QString &text, RasterStyle::Stretch mode)
{
  combo->addItem(text, static_cast<int>(mode));
}

int dataFor(const QComboBox *combo)
{
  return combo->currentData().toInt();
}

} // namespace

RasterStyleDialog::RasterStyleDialog(const QString &layerName, const QStringList &bandLabels,
                                     const RasterStyle &style, QWidget *parent)
  : QDialog(parent)
  , mStyle(style)
  , mInitialStyle(style)
{
  buildUi(layerName, bandLabels);
}

void RasterStyleDialog::buildUi(const QString &layerName, const QStringList &bandLabels)
{
  setWindowTitle(QStringLiteral("Estilo del raster «%1»").arg(layerName));

  mModeCombo = new QComboBox(this);
  addModeItem(mModeCombo, QStringLiteral("Color (RGB)"), RasterStyle::BandMode::Rgb);
  addModeItem(mModeCombo, QStringLiteral("Escala de grises"), RasterStyle::BandMode::Grayscale);
  mModeCombo->setCurrentIndex(mStyle.bandMode == RasterStyle::BandMode::Rgb ? 0 : 1);
  if (bandLabels.size() < 3)
    mModeCombo->setCurrentIndex(1);
  mModeCombo->setEnabled(bandLabels.size() >= 3);

  mRedCombo = new QComboBox(this);
  mGreenCombo = new QComboBox(this);
  mBlueCombo = new QComboBox(this);
  mAlphaCombo = new QComboBox(this);
  fillBandCombos(bandLabels);

  mStretchCombo = new QComboBox(this);
  addStretchItem(mStretchCombo, QStringLiteral("Ninguno"), RasterStyle::Stretch::None);
  addStretchItem(mStretchCombo, QStringLiteral("Minimo y maximo"), RasterStyle::Stretch::MinMax);
  addStretchItem(mStretchCombo, QStringLiteral("Percentiles (recorte)"), RasterStyle::Stretch::PercentClip);
  mStretchCombo->setCurrentIndex(mStretchCombo->findData(static_cast<int>(mStyle.stretch)));

  mCustomRangeCheck = new QCheckBox(QStringLiteral("Usar un rango manual"), this);
  mCustomRangeCheck->setChecked(mStyle.useCustomRange);

  mMinSpin = new QDoubleSpinBox(this);
  mMinSpin->setRange(-1.0e12, 1.0e12);
  mMinSpin->setDecimals(4);
  mMinSpin->setValue(mStyle.minValue);

  mMaxSpin = new QDoubleSpinBox(this);
  mMaxSpin->setRange(-1.0e12, 1.0e12);
  mMaxSpin->setDecimals(4);
  mMaxSpin->setValue(mStyle.maxValue);

  mPercentLowSpin = new QDoubleSpinBox(this);
  mPercentLowSpin->setRange(0.0, 49.0);
  mPercentLowSpin->setDecimals(1);
  mPercentLowSpin->setSingleStep(0.5);
  mPercentLowSpin->setValue(mStyle.percentLow);
  mPercentLowSpin->setSuffix(QStringLiteral(" %"));

  mPercentHighSpin = new QDoubleSpinBox(this);
  mPercentHighSpin->setRange(51.0, 100.0);
  mPercentHighSpin->setDecimals(1);
  mPercentHighSpin->setSingleStep(0.5);
  mPercentHighSpin->setValue(mStyle.percentHigh);
  mPercentHighSpin->setSuffix(QStringLiteral(" %"));

  mResamplingCombo = new QComboBox(this);
  mResamplingCombo->addItem(QStringLiteral("Bilineal (suave)"),
                            static_cast<int>(RasterStyle::Resampling::Bilinear));
  mResamplingCombo->addItem(QStringLiteral("Vecino mas cercano (nitido)"),
                            static_cast<int>(RasterStyle::Resampling::Nearest));
  mResamplingCombo->setCurrentIndex(
    mResamplingCombo->findData(static_cast<int>(mStyle.resampling)));

  mNoDataCheck = new QCheckBox(QStringLiteral("Dibujar el NoData transparente"), this);
  mNoDataCheck->setChecked(mStyle.useNodata);

  mOpacitySlider = new QSlider(Qt::Horizontal, this);
  mOpacitySlider->setRange(0, 100);
  mOpacitySlider->setValue(mStyle.opacity);

  auto *opacityRow = new QHBoxLayout;
  opacityRow->addWidget(mOpacitySlider, 1);
  auto *opacityValue = new QLabel(QStringLiteral("%1 %").arg(mStyle.opacity), this);
  opacityRow->addWidget(opacityValue);
  connect(mOpacitySlider, &QSlider::valueChanged, opacityValue,
          [opacityValue](int value) { opacityValue->setText(QStringLiteral("%1 %").arg(value)); });

  auto *bandsRow = new QHBoxLayout;
  bandsRow->addWidget(new QLabel(QStringLiteral("R"), this));
  bandsRow->addWidget(mRedCombo, 1);
  bandsRow->addWidget(new QLabel(QStringLiteral("G"), this));
  bandsRow->addWidget(mGreenCombo, 1);
  bandsRow->addWidget(new QLabel(QStringLiteral("B"), this));
  bandsRow->addWidget(mBlueCombo, 1);

  auto *form = new QFormLayout;
  form->addRow(QStringLiteral("Modo"), mModeCombo);
  form->addRow(QStringLiteral("Bandas"), bandsRow);
  form->addRow(QStringLiteral("Banda alfa"), mAlphaCombo);
  form->addRow(QStringLiteral("Estiramiento"), mStretchCombo);
  form->addRow(QString(), mCustomRangeCheck);
  form->addRow(QStringLiteral("Valor minimo"), mMinSpin);
  form->addRow(QStringLiteral("Valor maximo"), mMaxSpin);
  form->addRow(QStringLiteral("Percentil inferior"), mPercentLowSpin);
  form->addRow(QStringLiteral("Percentil superior"), mPercentHighSpin);
  form->addRow(QStringLiteral("Remuestreo"), mResamplingCombo);
  form->addRow(QString(), mNoDataCheck);
  form->addRow(QStringLiteral("Opacidad"), opacityRow);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  auto *resetButton = buttons->addButton(QStringLiteral("Restablecer"), QDialogButtonBox::ResetRole);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(resetButton, &QPushButton::clicked, this, &RasterStyleDialog::restoreInitialStyle);

  auto *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(buttons);

  connect(mModeCombo, &QComboBox::currentIndexChanged, this, &RasterStyleDialog::updateEnabledState);
  connect(mStretchCombo, &QComboBox::currentIndexChanged, this, &RasterStyleDialog::updateEnabledState);
  connect(mCustomRangeCheck, &QCheckBox::toggled, this, &RasterStyleDialog::updateEnabledState);

  updateEnabledState();
  resize(520, sizeHint().height());
}

void RasterStyleDialog::fillBandCombos(const QStringList &bandLabels)
{
  const auto addBands = [&bandLabels](QComboBox *combo, const QString &noneText) {
    if (!noneText.isEmpty())
      combo->addItem(noneText, 0);
    for (int i = 0; i < bandLabels.size(); ++i)
      combo->addItem(bandLabels.at(i), i + 1);
  };

  addBands(mRedCombo, QString());
  addBands(mGreenCombo, QString());
  addBands(mBlueCombo, QString());
  addBands(mAlphaCombo, QStringLiteral("Ninguna"));

  const auto select = [](QComboBox *combo, int band) {
    const int index = combo->findData(band);
    combo->setCurrentIndex(index >= 0 ? index : 0);
  };

  select(mRedCombo, mStyle.redBand);
  select(mGreenCombo, mStyle.greenBand);
  select(mBlueCombo, mStyle.blueBand);
  select(mAlphaCombo, mStyle.alphaBand);
}

void RasterStyleDialog::updateEnabledState()
{
  const bool grayscale = dataFor(mModeCombo) == static_cast<int>(RasterStyle::BandMode::Grayscale);
  mGreenCombo->setEnabled(!grayscale);
  mBlueCombo->setEnabled(!grayscale);

  const auto stretch = static_cast<RasterStyle::Stretch>(dataFor(mStretchCombo));
  mCustomRangeCheck->setEnabled(stretch == RasterStyle::Stretch::MinMax);
  const bool custom = stretch == RasterStyle::Stretch::MinMax && mCustomRangeCheck->isChecked();
  mMinSpin->setEnabled(custom);
  mMaxSpin->setEnabled(custom);

  const bool percentClip = stretch == RasterStyle::Stretch::PercentClip;
  mPercentLowSpin->setEnabled(percentClip);
  mPercentHighSpin->setEnabled(percentClip);
}

void RasterStyleDialog::restoreInitialStyle()
{
  mStyle = mInitialStyle;

  mModeCombo->setCurrentIndex(mStyle.bandMode == RasterStyle::BandMode::Rgb ? 0 : 1);
  const auto select = [](QComboBox *combo, int band) {
    const int index = combo->findData(band);
    combo->setCurrentIndex(index >= 0 ? index : 0);
  };
  select(mRedCombo, mStyle.redBand);
  select(mGreenCombo, mStyle.greenBand);
  select(mBlueCombo, mStyle.blueBand);
  select(mAlphaCombo, mStyle.alphaBand);

  mStretchCombo->setCurrentIndex(mStretchCombo->findData(static_cast<int>(mStyle.stretch)));
  mCustomRangeCheck->setChecked(mStyle.useCustomRange);
  mMinSpin->setValue(mStyle.minValue);
  mMaxSpin->setValue(mStyle.maxValue);
  mPercentLowSpin->setValue(mStyle.percentLow);
  mPercentHighSpin->setValue(mStyle.percentHigh);
  mResamplingCombo->setCurrentIndex(mResamplingCombo->findData(static_cast<int>(mStyle.resampling)));
  mNoDataCheck->setChecked(mStyle.useNodata);
  mOpacitySlider->setValue(mStyle.opacity);

  updateEnabledState();
}

RasterStyle RasterStyleDialog::style() const
{
  RasterStyle result = mStyle;

  result.bandMode = dataFor(mModeCombo) == static_cast<int>(RasterStyle::BandMode::Rgb)
                      ? RasterStyle::BandMode::Rgb
                      : RasterStyle::BandMode::Grayscale;
  result.redBand = mRedCombo->currentData().toInt();
  result.greenBand = mGreenCombo->currentData().toInt();
  result.blueBand = mBlueCombo->currentData().toInt();
  result.alphaBand = mAlphaCombo->currentData().toInt();

  result.stretch = static_cast<RasterStyle::Stretch>(dataFor(mStretchCombo));
  result.useCustomRange = mCustomRangeCheck->isChecked();
  result.minValue = mMinSpin->value();
  result.maxValue = mMaxSpin->value();
  result.percentLow = mPercentLowSpin->value();
  result.percentHigh = mPercentHighSpin->value();

  result.resampling = static_cast<RasterStyle::Resampling>(dataFor(mResamplingCombo));
  result.useNodata = mNoDataCheck->isChecked();
  result.opacity = mOpacitySlider->value();

  if (result.bandMode == RasterStyle::BandMode::Grayscale)
  {
    result.greenBand = result.redBand;
    result.blueBand = result.redBand;
  }

  return result;
}

} // namespace minigis
