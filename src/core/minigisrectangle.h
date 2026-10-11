#pragma once

#include <QString>

namespace minigis
{

/**
 * Rectangulo en coordenadas de mapa (equivalente minimo a QgsRectangle).
 *
 * Un rectangulo es valido cuando se ha inicializado con valores; puede ser
 * degenerado (ancho o alto cero), por ejemplo la extension de una capa de
 * puntos, y en ese caso `isEmpty()` devuelve true.
 */
class Rectangle
{
public:
  Rectangle() = default;
  Rectangle(double xMin, double yMin, double xMax, double yMax);

  bool isValid() const { return mValid; }
  bool isEmpty() const;

  double xMin() const { return mXMin; }
  double yMin() const { return mYMin; }
  double xMax() const { return mXMax; }
  double yMax() const { return mYMax; }

  double width() const { return mXMax - mXMin; }
  double height() const { return mYMax - mYMin; }
  double centerX() const { return (mXMin + mXMax) / 2.0; }
  double centerY() const { return (mYMin + mYMax) / 2.0; }

  void setNull();
  void extend(double x, double y);
  void combine(const Rectangle &other);

  /** Copia ampliada un porcentaje en cada direccion (0.05 = 5 %). */
  Rectangle buffered(double ratio) const;

  /** Copia con un ancho/alto minimo, para extensiones degeneradas. */
  Rectangle withMinimumSize(double minimumSpan) const;

  /** Copia ajustada a una relacion de aspecto ancho/alto. */
  Rectangle adjustedToAspect(double widthOverHeight) const;

  bool intersects(const Rectangle &other) const;
  bool contains(double x, double y) const;

  QString toString() const;

  bool operator==(const Rectangle &other) const;
  bool operator!=(const Rectangle &other) const { return !(*this == other); }

private:
  double mXMin = 0.0;
  double mYMin = 0.0;
  double mXMax = 0.0;
  double mYMax = 0.0;
  bool mValid = false;
};

} // namespace minigis
