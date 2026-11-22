#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;

namespace minigis
{

/**
 * Ventana de progreso de la carga de un mosaico.
 *
 * Tiene dos fases (abrir las hojas y preparar la replica) y un solo boton de
 * cancelar que actua sobre la fase en curso. Los textos se pueden leer desde las
 * pruebas sin ventana (`--snapshot-progress`), por eso expone `phase()`,
 * `percent()` y `message()`.
 */
class LoadProgressDialog : public QDialog
{
  Q_OBJECT

public:
  explicit LoadProgressDialog(QWidget *parent = nullptr);

  /** Titulo de la fase ("Cargando imagenes", "Preparando el mosaico"). */
  void setPhase(const QString &phase);
  QString phase() const;

  /** Avance 0..100 con el mensaje principal. */
  void setProgress(int percent, const QString &message);

  /** Linea pequena bajo la barra: archivo actual, nivel de detalle... */
  void setDetail(const QString &detail);
  QString detail() const;

  int percent() const;
  QString message() const;

signals:
  void cancelled();

private:
  QLabel *mPhaseLabel = nullptr;
  QLabel *mMessageLabel = nullptr;
  QLabel *mDetailLabel = nullptr;
  QProgressBar *mBar = nullptr;
  QPushButton *mCancelButton = nullptr;
};

} // namespace minigis
