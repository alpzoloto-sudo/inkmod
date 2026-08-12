#include "EpubOpfLite.h"

#include <XmlParserUtils.h>
#include <expat.h>

#include <cstring>

namespace {

const char* findAttr(const XML_Char** atts, const char* name) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

// expat gives local names without namespace prefixes when namespace
// processing is off (the default XML_ParserCreate() used below), so plain
// "item"/"itemref"/"spine" matches regardless of whatever namespace prefix
// (or none) the OPF actually uses.
void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* out = static_cast<EpubOpfLite*>(userData);
  if (strcmp(name, "item") == 0) {
    const char* id = findAttr(atts, "id");
    const char* href = findAttr(atts, "href");
    if (id && href) {
      const char* mediaType = findAttr(atts, "media-type");
      out->manifest.push_back({id, href, mediaType ? mediaType : ""});
    }
  } else if (strcmp(name, "itemref") == 0) {
    const char* idref = findAttr(atts, "idref");
    if (idref) out->spineIdrefs.emplace_back(idref);
  } else if (strcmp(name, "spine") == 0) {
    const char* toc = findAttr(atts, "toc");
    if (toc) out->tocNcxItemId = toc;
  }
}

}  // namespace

bool EpubOpfLite::parse(const std::string& opfContent, EpubOpfLite& out) {
  out = EpubOpfLite{};
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) return false;
  XML_SetUserData(parser, &out);
  XML_SetStartElementHandler(parser, startElement);

  const XML_Status status =
      XML_Parse(parser, opfContent.data(), static_cast<int>(opfContent.size()), XML_TRUE);
  const bool ok = (status == XML_STATUS_OK);
  destroyXmlParser(parser);
  return ok;
}
