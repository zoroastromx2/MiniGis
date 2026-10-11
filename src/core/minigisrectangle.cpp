#include "minigisrectangle.h"

#include <QStringList>

#include <algorithm>

namespace minigis
{

Rectangle::Rectangle(double xMin, double yMin, double xMax, double yMax)
  : mXMin(std::min(xMin, xMax))
  , mYMin(std::min(yMin, yMax))
  , mXMax(std::max(xMin, xMax))
  , mYMax(std::max(yMin, yMax))
  , mValid(true)
{
}

bool Rectangle::isEmpty() const
{
  return !mValid || mXMax <= mXMin || mYMax <= mYMin;
}

void Rectangle::setNull()
{
  mXMin = 0.0;
  mYMin = 0.0;
  mXMax = 0.0;
  mYMax = 0.0;
  mValid = false;
}

void Rectangle::extend(double x, double y)
{
  if (!mValid)
  {
    mXMin = mXMax = x;
    mYMin = mYMax = y;
    mValid = true;
    return;
  }
  mXMin = std::min(mXMin, x);
  mYMin = std::min(mYMin, y);
  mXMax = std::max(mXMax, x);
  mYMax = std::max(mYMax, y);
}

void Rectangle::combine(const Rectangle &other)
{
  if (!other.isValid())
    return;
  if (!mValid)
  {
    *this = other;
    return;
  }
  mXMin = std::min(mXMin, other.mXMin);
  mYMin = std::min(mYMin, other.mYMin);
  mXMax = std::max(mXMax, other.mXMax);
  mYMax = std::max(mYMax, other.mYMax);
}

Rectangle Rectangle::buffered(double ratio) const
{
  if (!mValid)
    return *this;

  const double deltaX = std::max(width(), 1e-9) * ratio;
  const double deltaY = std::max(height(), 1e-9) * ratio;
  return Rectangle(mXMin - deltaX, mYMin - deltaY, mXMax + deltaX, mYMax + deltaY);
}

Rectangle Rectangle::withMinimumSize(double minimumSpan) const
{
  if (!mValid)
    return *this;

  Rectangle result = *this;
  if (result.width() < minimumSpan)
  {
    const double half = (minimumSpan - result.width()) / 2.0;
    result.mXMin -= half;
    result.mXMax += half;
  }
  if (result.height() < minimumSpan)
  {
    const double half = (minimumSpan - result.height()) / 2.0;
    result.mYMin -= half;
    result.mYMax += half;
  }
  return result;
}

Rectangle Rectangle::adjustedToAspect(double widthOverHeight) const
{
  if (!mValid || widthOverHeight <= 0.0)
    return *this;

  Rectangle result = *this;
  const double currentAspect = (result.height() > 0.0) ? result.width() / result.height() : widthOverHeight;

  if (currentAspect > widthOverHeight)
  {
    // Demasiado ancho: se amplia en Y manteniendo el centro.
    const double targetHeight = result.width() / widthOverHeight;
    const double center = result.centerY();
    result.mYMin = center - targetHeight / 2.0;
    result.mYMax = center + targetHeight / 2.0;
  }
  else if (currentAspect < widthOverHeight)
  {
    // Demasiado alto: se amplia en X manteniendo el centro.
    const double targetWidth = result.height() * widthOverHeight;
    const double center = result.centerX();
    result.mXMin = center - targetWidth / 2.0;
    result.mXMax = center + targetWidth / 2.0;
  }
  return result;
}

bool Rectangle::intersects(const Rectangle &other) const
{
  if (!mValid || !other.mValid)
    return false;
  return !(other.mXMin > mXMax || other.mXMax < mXMin || other.mYMin > mYMax || other.mYMax < mYMin);
}

bool Rectangle::contains(double x, double y) const
{
  if (!mValid)
    return false;
  return x >= mXMin && x <= mXMax && y >= mYMin && y <= mYMax;
}

QString Rectangle::toString() const
{
  if (!mValid)
    return QStringLiteral("(sin extension)");

  // Seis decimales: suficiente para grados y para metros con precision centimetrica.
  return QStringLiteral("%1, %2 : %3, %4")
    .arg(mXMin, 0, 'f', 6)
    .arg(mYMin, 0, 'f', 6)
    .arg(mXMax, 0, 'f', 6)
    .arg(mYMax, 0, 'f', 6);
}

bool Rectangle::operator==(const Rectangle &other) const
{
  if (mValid != other.mValid)
    return false;
  if (!mValid)
    return true;
  return mXMin == other.mXMin && mYMin == other.mYMin && mXMax == other.mXMax && mYMax == other.mYMax;
}

} // namespace minigis
