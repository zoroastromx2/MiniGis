#include "minigislabeldialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace minigis
{
namespace
{

/** Muestra el color elegido en un boton. */
void applyColorToButton(QPushButton *button, const QColor &color)
{
  if (!button)
    return;

  button->setText(color.name(QColor::HexRgb).toUpper());
  button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                          .arg(color.name(QColor::HexRgb),
                               color.lightness() > 128 ? QStringLiteral("#202020") : QStringLiteral("#f0f0f0")));
}

/** Texto de ejemplo de la vista previa cuando no hay campos seleccionados. */
const char *kSampleText = "P-001 · residencial";

} // namespace

/**
 * Vista previa: dibuja el texto con el estilo actual sobre un fondo de color, que
 * es la unica forma de comprobar de un vistazo si el halo hace legible el texto.
 */
class LabelPreview : public QWidget
{
public:
  explicit LabelPreview(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setMinimumHeight(72);
  }

  void setPreview(const LabelStyle &style, const QString &text, double mapUnitsPerPixel)
  {
    mStyle = style;
    mText = text;
    mMapUnitsPerPixel = mapUnitsPerPixel;
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // Fondo que imita un relleno de mapa, para juzgar el contraste del halo.
    painter.fillRect(rect(), QColor(120, 150, 185));
    painter.setPen(QColor(90, 120, 155));
    for (int x = 0; x < width(); x += 24)
      painter.drawLine(x, 0, x, height());

    const QString text = mText.isEmpty() ? QString::fromUtf8(kSampleText) : mText;

    QFont font = painter.font();
    if (!mStyle.fontFamily.isEmpty())
      font.setFamily(mStyle.fontFamily);
    font.setBold(mStyle.bold);
    font.setItalic(mStyle.italic);
    if (mStyle.sizeMode == LabelStyle::SizeMode::MapUnits)
    {
      // La vista previa se dibuja a la escala actual: el usuario ve el tamano real.
      const double pixels = mMapUnitsPerPixel > 0.0 ? mStyle.fontSize / mMapUnitsPerPixel : 10.0;
      font.setPixelSize(std::max(1, static_cast<int>(std::lround(std::clamp(pixels, 1.0, 400.0)))));
    }
    else
    {
      font.setPointSizeF(std::clamp(mStyle.fontSize, 0.5, 400.0));
    }

    const QFontMetricsF metrics(font);
    const QRectF bounds = metrics.boundingRect(text);
    const QPointF baseline((width() - bounds.width()) / 2.0 - bounds.left(),
                           (height() + metrics.ascent() - metrics.descent()) / 2.0);

    if (mStyle.haloEnabled && mStyle.haloWidth > 0.0)
    {
      QPainterPath path;
      path.addText(baseline, font, text);
      painter.setPen(QPen(mStyle.haloColor, mStyle.haloWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(path);
      painter.fillPath(path, mStyle.color);
    }
    else
    {
      painter.setFont(font);
      painter.setPen(mStyle.color);
      painter.drawText(baseline, text);
    }
  }

private:
  LabelStyle mStyle;
  QString mText;
  double mMapUnitsPerPixel = 1.0;
};

LabelDialog::LabelDialog(const QString &layerName, const QVector<VectorLayer::FieldInfo> &fields,
                         const LabelStyle &style, double currentMapUnitsPerPixel, QWidget *parent)
  : QDialog(parent)
  , mFields(fields)
  , mCurrentMapUnitsPerPixel(currentMapUnitsPerPixel)
{
  buildUi(layerName);
  fillFields(fields, style);
  updatePreview();
}

void LabelDialog::buildUi(const QString &layerName)
{
  setWindowTitle(QStringLiteral("Etiquetas de «%1»").arg(layerName));

  mEnabledCheck = new QCheckBox(QStringLiteral("Mostrar las etiquetas de esta capa"), this);

  // --- Campos ---------------------------------------------------------------
  mFieldList = new QListWidget(this);
  mFieldList->setSelectionMode(QAbstractItemView::SingleSelection);
  mFieldList->setMinimumHeight(120);
  mFieldList->setToolTip(QStringLiteral("Marca los campos que se muestran. El orden de la lista es el "
                                        "orden del texto."));

  mFieldUpButton = new QPushButton(QStringLiteral("Subir"), this);
  mFieldDownButton = new QPushButton(QStringLiteral("Bajar"), this);
  auto *fieldButtons = new QVBoxLayout;
  fieldButtons->addWidget(mFieldUpButton);
  fieldButtons->addWidget(mFieldDownButton);
  fieldButtons->addStretch(1);

  auto *fieldRow = new QHBoxLayout;
  fieldRow->addWidget(mFieldList, 1);
  fieldRow->addLayout(fieldButtons);

  mSeparatorEdit = new QLineEdit(this);
  mSeparatorEdit->setToolTip(QStringLiteral("Texto que se inserta entre dos campos (por ejemplo « · »)"));

  auto *fieldsBox = new QGroupBox(QStringLiteral("Campos que se muestran"), this);
  auto *fieldsLayout = new QFormLayout(fieldsBox);
  fieldsLayout->addRow(fieldRow);
  fieldsLayout->addRow(QStringLiteral("Separador"), mSeparatorEdit);

  // --- Fuente ---------------------------------------------------------------
  mFamilyCombo = new QFontComboBox(this);
  mFamilyCombo->setToolTip(QStringLiteral("Familia tipografica de la etiqueta"));

  mSizeSpin = new QDoubleSpinBox(this);
  mSizeSpin->setRange(1.0, 200.0);
  mSizeSpin->setDecimals(1);
  mSizeSpin->setSingleStep(1.0);

  mBoldCheck = new QCheckBox(QStringLiteral("Negrita"), this);
  mItalicCheck = new QCheckBox(QStringLiteral("Cursiva"), this);
  mColorButton = new QPushButton(this);

  auto *styleWidget = new QWidget(this);
  auto *styleRow = new QHBoxLayout(styleWidget);
  styleRow->setContentsMargins(0, 0, 0, 0);
  styleRow->addWidget(mBoldCheck);
  styleRow->addWidget(mItalicCheck);
  styleRow->addWidget(new QLabel(QStringLiteral("Color"), this));
  styleRow->addWidget(mColorButton);
  styleRow->addStretch(1);

  mSizeModeCombo = new QComboBox(this);
  mSizeModeCombo->addItem(QStringLiteral("Puntos de pantalla (el texto no cambia al acercarse)"),
                          static_cast<int>(LabelStyle::SizeMode::Screen));
  mSizeModeCombo->addItem(QStringLiteral("Unidades del mapa (el texto crece al acercarse)"),
                          static_cast<int>(LabelStyle::SizeMode::MapUnits));
  mSizeModeCombo->setToolTip(QStringLiteral("Como se mide el tamano de la fuente"));

  // --- Halo -----------------------------------------------------------------
  mHaloCheck = new QCheckBox(QStringLiteral("Halo alrededor del texto"), this);
  mHaloColorButton = new QPushButton(this);
  mHaloWidthSpin = new QDoubleSpinBox(this);
  mHaloWidthSpin->setRange(0.0, 8.0);
  mHaloWidthSpin->setDecimals(1);
  mHaloWidthSpin->setSingleStep(0.2);
  mHaloWidthSpin->setSuffix(QStringLiteral(" px"));
  mHaloWidthSpin->setToolTip(QStringLiteral("Grosor del halo. Un halo claro hace legible el texto "
                                            "sobre cualquier relleno."));

  auto *haloWidget = new QWidget(this);
  auto *haloRow = new QHBoxLayout(haloWidget);
  haloRow->setContentsMargins(0, 0, 0, 0);
  haloRow->addWidget(mHaloCheck);
  haloRow->addWidget(new QLabel(QStringLiteral("Color"), this));
  haloRow->addWidget(mHaloColorButton);
  haloRow->addWidget(new QLabel(QStringLiteral("Grosor"), this));
  haloRow->addWidget(mHaloWidthSpin);
  haloRow->addStretch(1);

  mSizeReadout = new QLabel(this);
  mSizeReadout->setWordWrap(true);

  auto *sizeWidget = new QWidget(this);
  auto *sizeRow = new QVBoxLayout(sizeWidget);
  sizeRow->setContentsMargins(0, 0, 0, 0);
  sizeRow->setSpacing(2);
  sizeRow->addWidget(mSizeSpin);
  sizeRow->addWidget(mSizeReadout);

  auto *fontBox = new QGroupBox(QStringLiteral("Fuente"), this);
  auto *fontLayout = new QFormLayout(fontBox);
  fontLayout->addRow(QStringLiteral("Familia"), mFamilyCombo);
  fontLayout->addRow(QStringLiteral("Tamano"), sizeWidget);
  fontLayout->addRow(QStringLiteral("Escala del tamano"), mSizeModeCombo);
  fontLayout->addRow(QString(), styleWidget);
  fontLayout->addRow(QString(), haloWidget);

  // --- Escala ---------------------------------------------------------------
  mMinimumScaleSpin = new QDoubleSpinBox(this);
  mMinimumScaleSpin->setRange(0.0, 1e9);
  mMinimumScaleSpin->setDecimals(6);
  mMinimumScaleSpin->setToolTip(QStringLiteral("Unidades de mapa por pixel (la misma magnitud que "
                                               "muestra la barra de estado). 0 = sin limite."));

  mMaximumScaleSpin = new QDoubleSpinBox(this);
  mMaximumScaleSpin->setRange(0.0, 1e9);
  mMaximumScaleSpin->setDecimals(6);
  mMaximumScaleSpin->setToolTip(mMinimumScaleSpin->toolTip());

  auto *useMinButton = new QPushButton(QStringLiteral("Usar la actual"), this);
  auto *useMaxButton = new QPushButton(QStringLiteral("Usar la actual"), this);

  auto *minRow = new QHBoxLayout;
  minRow->addWidget(mMinimumScaleSpin, 1);
  minRow->addWidget(useMinButton);
  auto *maxRow = new QHBoxLayout;
  maxRow->addWidget(mMaximumScaleSpin, 1);
  maxRow->addWidget(useMaxButton);

  auto *scaleBox = new QGroupBox(QStringLiteral("A que escalas se ven (unidades de mapa por pixel)"), this);
  auto *scaleLayout = new QFormLayout(scaleBox);
  scaleLayout->addRow(QStringLiteral("Ocultar si la escala es mas alejada que"), minRow);
  scaleLayout->addRow(QStringLiteral("Ocultar si la escala es mas cercana que"), maxRow);

  // --- Colocacion -----------------------------------------------------------
  mPlacementCombo = new QComboBox(this);
  mPlacementCombo->addItem(QStringLiteral("Encima del elemento"),
                           static_cast<int>(LabelStyle::Placement::AroundPoint));
  mPlacementCombo->addItem(QStringLiteral("Centrado en el elemento"),
                           static_cast<int>(LabelStyle::Placement::OverPoint));
  mPlacementCombo->addItem(QStringLiteral("Sobre el punto medio (lineas)"),
                           static_cast<int>(LabelStyle::Placement::Line));

  mOffsetXSpin = new QDoubleSpinBox(this);
  mOffsetXSpin->setRange(-500.0, 500.0);
  mOffsetXSpin->setSuffix(QStringLiteral(" px"));
  mOffsetYSpin = new QDoubleSpinBox(this);
  mOffsetYSpin->setRange(-500.0, 500.0);
  mOffsetYSpin->setSuffix(QStringLiteral(" px"));

  mMinimumDistanceSpin = new QDoubleSpinBox(this);
  mMinimumDistanceSpin->setRange(0.0, 200.0);
  mMinimumDistanceSpin->setSuffix(QStringLiteral(" px"));
  mMinimumDistanceSpin->setToolTip(QStringLiteral("Separacion minima entre etiquetas: evita que se "
                                                  "solapen. 0 = no se comprueba."));

  mMinimumFeatureSizeSpin = new QDoubleSpinBox(this);
  mMinimumFeatureSizeSpin->setRange(0.0, 500.0);
  mMinimumFeatureSizeSpin->setSuffix(QStringLiteral(" px"));
  mMinimumFeatureSizeSpin->setToolTip(QStringLiteral("No etiquetar elementos mas pequenos que esto en "
                                                     "pantalla (evita texto sobre elementos que no se ven)."));

  auto *placementBox = new QGroupBox(QStringLiteral("Colocacion y legibilidad"), this);
  auto *placementLayout = new QFormLayout(placementBox);
  placementLayout->addRow(QStringLiteral("Posicion"), mPlacementCombo);
  placementLayout->addRow(QStringLiteral("Desplazamiento X"), mOffsetXSpin);
  placementLayout->addRow(QStringLiteral("Desplazamiento Y"), mOffsetYSpin);
  placementLayout->addRow(QStringLiteral("Distancia entre etiquetas"), mMinimumDistanceSpin);
  placementLayout->addRow(QStringLiteral("Tamano minimo del elemento"), mMinimumFeatureSizeSpin);

  // --- Saturacion -----------------------------------------------------------
  mMaximumLabelsSpin = new QSpinBox(this);
  mMaximumLabelsSpin->setRange(0, 1000000);
  mMaximumLabelsSpin->setToolTip(QStringLiteral("Limite de etiquetas por cuadro (0 = sin limite). "
                                                "Acota el coste del dibujado."));

  mDiscardDuplicatesCheck = new QCheckBox(QStringLiteral("No repetir el mismo texto en la vista"), this);
  mPrioritySpin = new QSpinBox(this);
  mPrioritySpin->setRange(-1000, 1000);
  mPrioritySpin->setToolTip(QStringLiteral("Menor = se coloca antes cuando hay que descartar etiquetas."));

  auto *limitBox = new QGroupBox(QStringLiteral("Saturacion del mapa"), this);
  auto *limitLayout = new QFormLayout(limitBox);
  limitLayout->addRow(QStringLiteral("Maximo de etiquetas"), mMaximumLabelsSpin);
  limitLayout->addRow(QString(), mDiscardDuplicatesCheck);
  limitLayout->addRow(QStringLiteral("Prioridad"), mPrioritySpin);

  // --- Vista previa y botones ----------------------------------------------
  mPreview = new LabelPreview(this);

  auto *previewBox = new QGroupBox(QStringLiteral("Vista previa"), this);
  auto *previewLayout = new QVBoxLayout(previewBox);
  previewLayout->addWidget(mPreview);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                         | QDialogButtonBox::RestoreDefaults,
                                       this);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Aceptar"));
  buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancelar"));
  buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("Restablecer"));

  auto *columns = new QHBoxLayout;
  auto *leftColumn = new QVBoxLayout;
  leftColumn->addWidget(fieldsBox);
  leftColumn->addWidget(fontBox);
  auto *rightColumn = new QVBoxLayout;
  rightColumn->addWidget(scaleBox);
  rightColumn->addWidget(placementBox);
  rightColumn->addWidget(limitBox);
  columns->addLayout(leftColumn, 1);
  columns->addLayout(rightColumn, 1);

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(mEnabledCheck);
  layout->addLayout(columns);
  layout->addWidget(previewBox);
  layout->addWidget(buttons);

  // --- Conexiones ----------------------------------------------------------
  const auto connectPreview = [this]() { updatePreview(); };
  connect(mEnabledCheck, &QCheckBox::toggled, this, connectPreview);
  connect(mFieldList, &QListWidget::itemChanged, this, connectPreview);
  connect(mFieldList, &QListWidget::itemSelectionChanged, this, connectPreview);
  connect(mSeparatorEdit, &QLineEdit::textChanged, this, connectPreview);
  connect(mFamilyCombo, &QFontComboBox::currentFontChanged, this, connectPreview);
  connect(mSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, connectPreview);
  connect(mBoldCheck, &QCheckBox::toggled, this, connectPreview);
  connect(mItalicCheck, &QCheckBox::toggled, this, connectPreview);
  connect(mHaloCheck, &QCheckBox::toggled, this, connectPreview);
  connect(mHaloWidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, connectPreview);
  connect(mSizeModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &LabelDialog::onSizeModeChanged);
  connect(mSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          &LabelDialog::updateSizeReadout);
  connect(mColorButton, &QPushButton::clicked, this, &LabelDialog::chooseTextColor);
  connect(mHaloColorButton, &QPushButton::clicked, this, &LabelDialog::chooseHaloColor);
  connect(mFieldUpButton, &QPushButton::clicked, this, [this]() { moveSelectedField(-1); });
  connect(mFieldDownButton, &QPushButton::clicked, this, [this]() { moveSelectedField(1); });
  connect(useMinButton, &QPushButton::clicked, this, [this]() { useCurrentScaleAsLimit(false); });
  connect(useMaxButton, &QPushButton::clicked, this, [this]() { useCurrentScaleAsLimit(true); });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this]() {
    fillFields(mFields, LabelStyle());
    updatePreview();
  });
}

void LabelDialog::fillFields(const QVector<VectorLayer::FieldInfo> &fields, const LabelStyle &style)
{
  mEnabledCheck->setChecked(style.enabled);

  mFieldList->clear();
  for (const VectorLayer::FieldInfo &field : fields)
  {
    auto *item = new QListWidgetItem(QStringLiteral("%1 (%2)").arg(field.name, field.typeName), mFieldList);
    item->setData(Qt::UserRole, field.name);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(style.fields.contains(field.name, Qt::CaseInsensitive) ? Qt::Checked : Qt::Unchecked);
  }

  mSeparatorEdit->setText(style.separator);
  mFamilyCombo->setCurrentFont(QFont(style.fontFamily.isEmpty() ? font().family() : style.fontFamily));

  // El MODO y el RANGO van ANTES que el valor: en unidades del mapa el valor puede
  // ser del orden de 0,0001, y si el rango todavia fuese el de puntos, el control lo
  // recortaria a 1 y el texto saldria gigante (era exactamente el fallo).
  const int sizeModeIndex = mSizeModeCombo->findData(static_cast<int>(style.sizeMode));
  {
    const QSignalBlocker blocker(mSizeModeCombo);
    mSizeModeCombo->setCurrentIndex(sizeModeIndex >= 0 ? sizeModeIndex : 0);
  }
  mLastSizeMode = style.sizeMode;
  updateSizeRange(style.sizeMode);
  mSizeSpin->setValue(style.fontSize);

  mBoldCheck->setChecked(style.bold);
  mItalicCheck->setChecked(style.italic);
  applyColorToButton(mColorButton, style.color);

  mHaloCheck->setChecked(style.haloEnabled);
  applyColorToButton(mHaloColorButton, style.haloColor);
  mHaloWidthSpin->setValue(style.haloWidth);

  updateSizeReadout();

  mMinimumScaleSpin->setValue(style.minimumScale);
  mMaximumScaleSpin->setValue(style.maximumScale);

  const int placementIndex = mPlacementCombo->findData(static_cast<int>(style.placement));
  mPlacementCombo->setCurrentIndex(placementIndex >= 0 ? placementIndex : 0);
  mOffsetXSpin->setValue(style.offsetX);
  mOffsetYSpin->setValue(style.offsetY);
  mMinimumDistanceSpin->setValue(style.minimumDistance);
  mMinimumFeatureSizeSpin->setValue(style.minimumFeatureSize);

  mMaximumLabelsSpin->setValue(static_cast<int>(style.maximumLabels));
  mDiscardDuplicatesCheck->setChecked(style.discardDuplicates);
  mPrioritySpin->setValue(style.priority);
}

double LabelDialog::currentPixelSize() const
{
  if (mSizeModeCombo->currentData().toInt() == static_cast<int>(LabelStyle::SizeMode::MapUnits))
  {
    // El valor ya esta en unidades del mapa: pasa a pixeles con la escala actual.
    return mCurrentMapUnitsPerPixel > 0.0 ? mSizeSpin->value() / mCurrentMapUnitsPerPixel : 0.0;
  }

  QFont font = mFamilyCombo->currentFont();
  font.setPointSizeF(mSizeSpin->value());
  font.setBold(mBoldCheck->isChecked());
  font.setItalic(mItalicCheck->isChecked());
  return QFontMetricsF(font).height();
}

void LabelDialog::updateSizeRange(LabelStyle::SizeMode mode)
{
  const QSignalBlocker blocker(mSizeSpin);

  if (mode == LabelStyle::SizeMode::MapUnits)
  {
    // En unidades del mapa el valor puede ser muy pequeno (con un SRC geografico,
    // del orden de 0,001) o muy grande (con uno proyectado en metros, del orden de
    // 10). Un rango fijo de puntos hacia imposible escribir el valor correcto: era
    // la causa de que el texto saliera gigante al elegir este modo.
    mSizeSpin->setDecimals(6);
    mSizeSpin->setRange(0.000001, 1000000.0);
    mSizeSpin->setSingleStep(mCurrentMapUnitsPerPixel > 0.0 ? mCurrentMapUnitsPerPixel : 1.0);
    mSizeSpin->setSuffix(QStringLiteral(" u.m."));
  }
  else
  {
    mSizeSpin->setDecimals(1);
    mSizeSpin->setRange(1.0, 200.0);
    mSizeSpin->setSingleStep(1.0);
    mSizeSpin->setSuffix(QStringLiteral(" pt"));
  }
}

void LabelDialog::onSizeModeChanged(int index)
{
  const auto mode = static_cast<LabelStyle::SizeMode>(mSizeModeCombo->itemData(index).toInt());
  if (mode == mLastSizeMode)
    return;

  // Se convierte el valor para que la etiqueta NO cambie de tamano aparente al
  // cambiar de modo: el usuario ve el mismo texto y ajusta a partir de ahi.
  const double pixels = currentPixelSize();
  updateSizeRange(mode);

  if (pixels > 0.0)
  {
    if (mode == LabelStyle::SizeMode::MapUnits)
    {
      if (mCurrentMapUnitsPerPixel > 0.0)
        mSizeSpin->setValue(pixels * mCurrentMapUnitsPerPixel);
    }
    else
    {
      QFont font = mFamilyCombo->currentFont();
      font.setPixelSize(std::max(1, static_cast<int>(std::lround(pixels))));
      mSizeSpin->setValue(font.pointSizeF() > 0.0 ? font.pointSizeF() : pixels * 72.0 / 96.0);
    }
  }

  mLastSizeMode = mode;
  updateSizeReadout();
}

void LabelDialog::updateSizeReadout()
{
  if (!mSizeReadout)
    return;

  const double pixels = currentPixelSize();

  if (mSizeModeCombo->currentData().toInt() == static_cast<int>(LabelStyle::SizeMode::MapUnits))
  {
    mSizeReadout->setText(QStringLiteral("≈ %1 px a la escala actual; el texto crece al acercarse")
                            .arg(pixels, 0, 'f', 1));
  }
  else
  {
    mSizeReadout->setText(QStringLiteral("≈ %1 px; el texto no cambia al acercarse")
                            .arg(pixels, 0, 'f', 1));
  }

  // Aviso cuando el valor deja el texto fuera de lo legible: es la senal de que el
  // numero no significa lo que se cree.
  const bool unreasonable = pixels > 200.0 || pixels < 3.0;
  mSizeReadout->setStyleSheet(unreasonable ? QStringLiteral("color: #b00020;")
                                           : QStringLiteral("color: palette(mid);"));
}

void LabelDialog::moveSelectedField(int delta)
{
  const int row = mFieldList->currentRow();
  const int target = row + delta;
  if (row < 0 || target < 0 || target >= mFieldList->count())
    return;

  QListWidgetItem *item = mFieldList->takeItem(row);
  mFieldList->insertItem(target, item);
  mFieldList->setCurrentRow(target);
  updatePreview();
}

void LabelDialog::chooseTextColor()
{
  const QColor chosen = QColorDialog::getColor(collect().color, this, QStringLiteral("Color del texto"));
  if (chosen.isValid())
  {
    applyColorToButton(mColorButton, chosen);
    updatePreview();
  }
}

void LabelDialog::chooseHaloColor()
{
  const QColor chosen = QColorDialog::getColor(collect().haloColor, this, QStringLiteral("Color del halo"));
  if (chosen.isValid())
  {
    applyColorToButton(mHaloColorButton, chosen);
    updatePreview();
  }
}

void LabelDialog::useCurrentScaleAsLimit(bool maximum)
{
  if (mCurrentMapUnitsPerPixel <= 0.0)
    return;

  if (maximum)
    mMaximumScaleSpin->setValue(mCurrentMapUnitsPerPixel);
  else
    mMinimumScaleSpin->setValue(mCurrentMapUnitsPerPixel);
}

LabelStyle LabelDialog::collect() const
{
  LabelStyle style;
  style.enabled = mEnabledCheck->isChecked();
  style.separator = mSeparatorEdit->text();

  for (int row = 0; row < mFieldList->count(); ++row)
  {
    const QListWidgetItem *item = mFieldList->item(row);
    if (item->checkState() == Qt::Checked)
      style.fields << item->data(Qt::UserRole).toString();
  }

  style.sizeMode = static_cast<LabelStyle::SizeMode>(mSizeModeCombo->currentData().toInt());
  style.fontFamily = mFamilyCombo->currentFont().family();
  style.fontSize = mSizeSpin->value();
  style.bold = mBoldCheck->isChecked();
  style.italic = mItalicCheck->isChecked();
  style.color = QColor(mColorButton->text());

  style.haloEnabled = mHaloCheck->isChecked();
  style.haloColor = QColor(mHaloColorButton->text());
  style.haloWidth = mHaloWidthSpin->value();

  style.minimumScale = mMinimumScaleSpin->value();
  style.maximumScale = mMaximumScaleSpin->value();

  style.placement = static_cast<LabelStyle::Placement>(mPlacementCombo->currentData().toInt());
  style.offsetX = mOffsetXSpin->value();
  style.offsetY = mOffsetYSpin->value();
  style.minimumDistance = mMinimumDistanceSpin->value();
  style.minimumFeatureSize = mMinimumFeatureSizeSpin->value();

  style.maximumLabels = mMaximumLabelsSpin->value();
  style.discardDuplicates = mDiscardDuplicatesCheck->isChecked();
  style.priority = mPrioritySpin->value();

  return style;
}

LabelStyle LabelDialog::labelStyle() const
{
  return collect();
}

void LabelDialog::updatePreview()
{
  const LabelStyle style = collect();

  QString sample;
  for (int row = 0; row < mFieldList->count() && sample.isEmpty(); ++row)
  {
    const QListWidgetItem *item = mFieldList->item(row);
    if (item->checkState() == Qt::Checked)
      sample = item->data(Qt::UserRole).toString();
  }

  if (sample.isEmpty())
    sample = QString::fromUtf8(kSampleText);

  mPreview->setPreview(style, sample, mCurrentMapUnitsPerPixel);
}

} // namespace minigis
