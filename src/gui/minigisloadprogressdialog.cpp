#include "minigisloadprogressdialog.h"

#include <QFrame>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace minigis
{

LoadProgressDialog::LoadProgressDialog(QWidget *parent) : QDialog(parent)
{
  setWindowTitle(QStringLiteral("Cargando el mosaico"));
  setWindowModality(Qt::WindowModal);
  setMinimumWidth(460);
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);

  mPhaseLabel = new QLabel(QStringLiteral("Cargando imagenes"), this);
  QFont phaseFont = mPhaseLabel->font();
  phaseFont.setBold(true);
  mPhaseLabel->setFont(phaseFont);
  layout->addWidget(mPhaseLabel);

  mBar = new QProgressBar(this);
  mBar->setRange(0, 100);
  mBar->setValue(0);
  mBar->setTextVisible(true);
  layout->addWidget(mBar);

  mMessageLabel = new QLabel(QString(), this);
  mMessageLabel->setWordWrap(true);
  layout->addWidget(mMessageLabel);

  mDetailLabel = new QLabel(QString(), this);
  mDetailLabel->setWordWrap(true);
  mDetailLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
  layout->addWidget(mDetailLabel);

  auto *line = new QFrame(this);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  layout->addWidget(line);

  auto *buttons = new QHBoxLayout;
  buttons->addStretch(1);
  mCancelButton = new QPushButton(QStringLiteral("Cancelar"), this);
  mCancelButton->setAutoDefault(false);
  mCancelButton->setDefault(false);
  connect(mCancelButton, &QPushButton::clicked, this, [this]() {
    mCancelButton->setEnabled(false);
    mCancelButton->setText(QStringLiteral("Cancelando..."));
    emit cancelled();
  });
  buttons->addWidget(mCancelButton);
  layout->addLayout(buttons);
}

void LoadProgressDialog::setPhase(const QString &phase)
{
  mPhaseLabel->setText(phase);
  setWindowTitle(phase);
}

QString LoadProgressDialog::phase() const
{
  return mPhaseLabel->text();
}

void LoadProgressDialog::setProgress(int percent, const QString &message)
{
  mBar->setValue(std::clamp(percent, 0, 100));
  mMessageLabel->setText(message);
}

void LoadProgressDialog::setDetail(const QString &detail)
{
  mDetailLabel->setText(detail);
}

QString LoadProgressDialog::detail() const
{
  return mDetailLabel->text();
}

int LoadProgressDialog::percent() const
{
  return mBar->value();
}

QString LoadProgressDialog::message() const
{
  return mMessageLabel->text();
}

} // namespace minigis
