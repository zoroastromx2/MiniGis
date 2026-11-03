#include "minigisxmltree.h"

#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace minigis
{

namespace
{

/**
 * Lee un elemento y todos sus descendientes. `reader` esta situado en el
 * StartElement; al volver queda en el EndElement correspondiente.
 */
bool parseElement(QXmlStreamReader &reader, XmlNode *node, Error *error)
{
  node->setName(reader.name().toString());

  const QXmlStreamAttributes attributes = reader.attributes();
  for (const QXmlStreamAttribute &attribute : attributes)
    node->setAttribute(attribute.qualifiedName().toString(), attribute.value().toString());

  while (!reader.atEnd())
  {
    const QXmlStreamReader::TokenType token = reader.readNext();

    if (token == QXmlStreamReader::StartElement)
    {
      XmlNode child;
      if (!parseElement(reader, &child, error))
        return false;
      node->children().append(child);
    }
    else if (token == QXmlStreamReader::EndElement)
    {
      break;
    }
    else if (token == QXmlStreamReader::Characters || token == QXmlStreamReader::EntityReference)
    {
      node->setText(node->text() + reader.text().toString());
    }
    else if (token == QXmlStreamReader::Comment)
    {
      node->children().append(XmlNode::comment(reader.text().toString()));
    }
    else if (token == QXmlStreamReader::Invalid || token == QXmlStreamReader::DTD)
    {
      // Un DTD dentro de un elemento no es valido en estos documentos; se
      // ignora en lugar de fallar para no rechazar proyectos antiguos.
    }
  }

  if (reader.hasError())
  {
    if (error)
    {
      *error = Error {QStringLiteral("El XML del proyecto esta mal formado: %1").arg(reader.errorString()),
                      QStringLiteral("Comprueba que el archivo no se haya truncado y vuelve a guardarlo desde QGIS.")};
    }
    return false;
  }

  // El XML de QGIS viene sangrado: el espacio entre elementos no es contenido.
  if (!node->children().isEmpty() && node->text().trimmed().isEmpty())
    node->setText(QString());

  return true;
}

} // namespace

XmlNode::XmlNode(const QString &name)
  : mName(name)
{
}

XmlNode XmlNode::comment(const QString &text)
{
  XmlNode node;
  node.mType = Type::Comment;
  node.mText = text;
  return node;
}

bool XmlNode::parse(const QByteArray &xml, XmlNode *root, Error *error)
{
  if (!root)
    return false;

  QXmlStreamReader reader(xml);
  while (!reader.atEnd())
  {
    const QXmlStreamReader::TokenType token = reader.readNext();
    if (token == QXmlStreamReader::StartElement)
    {
      XmlNode documentRoot;
      if (!parseElement(reader, &documentRoot, error))
        return false;
      *root = documentRoot;
      return true;
    }
    if (token == QXmlStreamReader::Invalid)
      break;
  }

  if (error)
  {
    *error = Error {
      QStringLiteral("No se encontro ningun elemento raiz en el XML del proyecto."),
      QStringLiteral("El archivo no parece un proyecto de QGIS; comprueba que no sea un ZIP vacio o un archivo de otro tipo.")};
  }
  return false;
}

QByteArray XmlNode::toXml() const
{
  QByteArray data;
  QXmlStreamWriter writer(&data);
  writer.setAutoFormatting(true);
  writer.setAutoFormattingIndent(2);

  // QGIS escribe el DOCTYPE antes de la raiz y no escribe declaracion XML.
  // Repetirlo mantiene el archivo reconocible por QGIS y por otras herramientas.
  if (isElement() && mName == QStringLiteral("qgis"))
  {
    writer.writeDTD(QStringLiteral("<!DOCTYPE qgis PUBLIC 'http://mrcc.com/qgis.dtd' 'SYSTEM'>"));
  }

  writeTo(writer);

  if (writer.hasError())
    return QByteArray();

  return data;
}

bool XmlNode::hasAttribute(const QString &name) const
{
  for (const auto &attribute : mAttributes)
  {
    if (attribute.first == name)
      return true;
  }
  return false;
}

QString XmlNode::attribute(const QString &name, const QString &fallback) const
{
  for (const auto &attribute : mAttributes)
  {
    if (attribute.first == name)
      return attribute.second;
  }
  return fallback;
}

void XmlNode::setAttribute(const QString &name, const QString &value)
{
  for (auto &attribute : mAttributes)
  {
    if (attribute.first == name)
    {
      attribute.second = value;
      return;
    }
  }
  mAttributes.append(qMakePair(name, value));
}

void XmlNode::removeAttribute(const QString &name)
{
  for (int index = 0; index < mAttributes.size(); ++index)
  {
    if (mAttributes.at(index).first == name)
    {
      mAttributes.removeAt(index);
      return;
    }
  }
}

XmlNode *XmlNode::child(const QString &name, int occurrence)
{
  int seen = 0;
  for (XmlNode &candidate : mChildren)
  {
    if (candidate.isElement() && candidate.name() == name)
    {
      if (seen == occurrence)
        return &candidate;
      ++seen;
    }
  }
  return nullptr;
}

const XmlNode *XmlNode::child(const QString &name, int occurrence) const
{
  int seen = 0;
  for (const XmlNode &candidate : mChildren)
  {
    if (candidate.isElement() && candidate.name() == name)
    {
      if (seen == occurrence)
        return &candidate;
      ++seen;
    }
  }
  return nullptr;
}

QVector<XmlNode *> XmlNode::childrenNamed(const QString &name)
{
  QVector<XmlNode *> result;
  for (XmlNode &candidate : mChildren)
  {
    if (candidate.isElement() && candidate.name() == name)
      result.append(&candidate);
  }
  return result;
}

QVector<const XmlNode *> XmlNode::childrenNamed(const QString &name) const
{
  QVector<const XmlNode *> result;
  for (const XmlNode &candidate : mChildren)
  {
    if (candidate.isElement() && candidate.name() == name)
      result.append(&candidate);
  }
  return result;
}

XmlNode *XmlNode::path(const QStringList &names)
{
  XmlNode *current = this;
  for (const QString &name : names)
  {
    if (!current)
      return nullptr;
    current = current->child(name);
  }
  return current;
}

const XmlNode *XmlNode::path(const QStringList &names) const
{
  const XmlNode *current = this;
  for (const QString &name : names)
  {
    if (!current)
      return nullptr;
    current = current->child(name);
  }
  return current;
}

XmlNode *XmlNode::appendChild(const XmlNode &child)
{
  mChildren.append(child);
  return &mChildren.last();
}

int XmlNode::indexOfChild(const QString &name) const
{
  for (int index = 0; index < mChildren.size(); ++index)
  {
    if (mChildren.at(index).isElement() && mChildren.at(index).name() == name)
      return index;
  }
  return -1;
}

bool XmlNode::removeChild(const QString &name, int occurrence)
{
  int seen = 0;
  for (int index = 0; index < mChildren.size(); ++index)
  {
    if (!mChildren.at(index).isElement() || mChildren.at(index).name() != name)
      continue;
    if (seen == occurrence)
    {
      mChildren.removeAt(index);
      return true;
    }
    ++seen;
  }
  return false;
}

void XmlNode::writeTo(QXmlStreamWriter &writer) const
{
  if (isComment())
  {
    writer.writeComment(mText);
    return;
  }

  if (mChildren.isEmpty() && mText.isEmpty())
  {
    writer.writeEmptyElement(mName);
    for (const auto &attribute : mAttributes)
      writer.writeAttribute(attribute.first, attribute.second);
    return;
  }

  writer.writeStartElement(mName);
  for (const auto &attribute : mAttributes)
    writer.writeAttribute(attribute.first, attribute.second);

  if (!mText.isEmpty())
    writer.writeCharacters(mText);

  for (const XmlNode &child : mChildren)
    child.writeTo(writer);

  writer.writeEndElement();
}

} // namespace minigis
