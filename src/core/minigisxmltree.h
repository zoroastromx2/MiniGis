#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <utility>

#include "minigiserror.h"

class QXmlStreamWriter;

namespace minigis
{

/**
 * Nodo de un arbol XML minimo pensado para conservar documentos ajenos.
 *
 * MiniGIS solo interpreta una parte del formato de QGIS. Para poder guardar un
 * proyecto sin destruir maquetas, relaciones o simbolos que no entiende, el
 * documento se lee a este arbol y se vuelve a escribir tal cual, cambiando
 * unicamente los nodos que el usuario ha modificado.
 *
 * Propiedades que se conservan:
 *   * el orden de los elementos y de sus atributos;
 *   * el texto de los elementos hoja (WKT, valores, titulos);
 *   * los comentarios XML.
 *
 * El `DOCTYPE` de QGIS se emite siempre al escribir la raiz; no se guarda el
 * texto original de la cabecera porque QGIS no lo interpreta.
 */
class XmlNode
{
public:
  /** Tipo de nodo; los elementos son los unicos que tienen nombre e hijos. */
  enum class Type
  {
    Element,
    Comment
  };

  XmlNode() = default;
  explicit XmlNode(const QString &name);

  /** Crea un nodo de comentario XML (no tiene nombre ni atributos). */
  static XmlNode comment(const QString &text);

  /**
   * Lee un documento XML completo. `root` recibe el elemento raiz. En caso de
   * error devuelve false y rellena `error` con la linea y la columna.
   */
  static bool parse(const QByteArray &xml, XmlNode *root, Error *error);

  /**
   * Serializa el arbol. Para la raiz `qgis` escribe el `DOCTYPE` de QGIS y
   * despues el documento en UTF-8 con sangrado de dos espacios.
   */
  QByteArray toXml() const;

  bool isElement() const { return mType == Type::Element; }
  bool isComment() const { return mType == Type::Comment; }

  const QString &name() const { return mName; }
  void setName(const QString &name) { mName = name; }
  const QString &text() const { return mText; }
  void setText(const QString &text) { mText = text; }

  const QVector<XmlNode> &children() const { return mChildren; }
  QVector<XmlNode> &children() { return mChildren; }

  /** Atributos en el orden en que aparecieron en el documento. */
  const QVector<QPair<QString, QString>> &attributes() const { return mAttributes; }

  bool hasAttribute(const QString &name) const;
  QString attribute(const QString &name, const QString &fallback = QString()) const;
  void setAttribute(const QString &name, const QString &value);
  void removeAttribute(const QString &name);

  /**
   * Hijo elemento con ese nombre; `occurrence` permite pedir el segundo, el
   * tercero, etc. Devuelve nullptr si no existe. El puntero es valido hasta que
   * se modifiquen los hijos del nodo.
   */
  XmlNode *child(const QString &name, int occurrence = 0);
  const XmlNode *child(const QString &name, int occurrence = 0) const;

  /** Todos los hijos elemento con ese nombre, en orden. */
  QVector<XmlNode *> childrenNamed(const QString &name);
  QVector<const XmlNode *> childrenNamed(const QString &name) const;

  /**
   * Busca un descendiente siguiendo una ruta de nombres, por ejemplo
   * {"projectlayers", "maplayer"}. Devuelve nullptr si algun tramo falta.
   */
  XmlNode *path(const QStringList &names);
  const XmlNode *path(const QStringList &names) const;

  /** Anade un hijo al final y devuelve el puntero al nodo anadido. */
  XmlNode *appendChild(const XmlNode &child);

  /** Indice del primer hijo con ese nombre, o -1. */
  int indexOfChild(const QString &name) const;

  bool removeChild(const QString &name, int occurrence = 0);
  void clearChildren() { mChildren.clear(); }

private:
  void writeTo(QXmlStreamWriter &writer) const;

  Type mType = Type::Element;
  QString mName;
  QString mText;
  QVector<QPair<QString, QString>> mAttributes;
  QVector<XmlNode> mChildren;
};

} // namespace minigis
