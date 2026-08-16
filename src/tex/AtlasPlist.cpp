#include "tex/AtlasPlist.h"

#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QXmlStreamReader>

#include "store/MpkIndex.h"

namespace {

// ── Apple XML plist -> QVariant tree ────────────────────────────────────────
// Minimal, strict reader for the subset a Cocos sheet uses: dict / array /
// string / integer / real / true / false / data (kept as raw base64 text).

QVariant readValue(QXmlStreamReader& xml);

QVariant readDict(QXmlStreamReader& xml)
{
    QVariantMap map;
    QString key;
    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::EndElement &&
            xml.name() == QLatin1String("dict"))
            return map;
        if (tok != QXmlStreamReader::StartElement) continue;
        if (xml.name() == QLatin1String("key")) {
            key = xml.readElementText();
        } else {
            map.insert(key, readValue(xml));
            key.clear();
        }
    }
    return map;
}

QVariant readArray(QXmlStreamReader& xml)
{
    QVariantList list;
    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::EndElement &&
            xml.name() == QLatin1String("array"))
            return list;
        if (tok != QXmlStreamReader::StartElement) continue;
        list.append(readValue(xml));
    }
    return list;
}

// Called with the reader ON a value's StartElement.
QVariant readValue(QXmlStreamReader& xml)
{
    const QStringView n = xml.name();
    if (n == QLatin1String("dict"))    return readDict(xml);
    if (n == QLatin1String("array"))   return readArray(xml);
    if (n == QLatin1String("string"))  return xml.readElementText();
    if (n == QLatin1String("integer")) return xml.readElementText().toLongLong();
    if (n == QLatin1String("real"))    return xml.readElementText().toDouble();
    if (n == QLatin1String("true"))  { xml.skipCurrentElement(); return true; }
    if (n == QLatin1String("false")) { xml.skipCurrentElement(); return false; }
    if (n == QLatin1String("data") || n == QLatin1String("date"))
        return xml.readElementText();
    xml.skipCurrentElement();
    return {};
}

// "{{289,1},{72,73}}" / "{72,73}" -> up to 4 numbers (order preserved).
// Cocos writes integers; reals appear in some exporters, so parse as double.
QList<double> numbersIn(const QString& s)
{
    QList<double> out;
    QString cur;
    for (const QChar c : s) {
        if (c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('.') ||
            c == QLatin1Char('+')) {
            cur += c;
        } else if (!cur.isEmpty()) {
            out.append(cur.toDouble());
            cur.clear();
        }
    }
    if (!cur.isEmpty()) out.append(cur.toDouble());
    return out;
}

QString hexHead(const uint8_t* data, size_t len, size_t n = 16)
{
    QString s;
    for (size_t i = 0; i < len && i < n; ++i)
        s += QStringLiteral("%1 ").arg(data[i], 2, 16, QLatin1Char('0'));
    return s.trimmed();
}

}  // namespace

namespace AtlasPlist {

size_t findDescriptor(const di::MpkIndex& mpk, const QString& displayName)
{
    QString leaf = displayName;
    const int slash = leaf.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) leaf = leaf.mid(slash + 1);
    // A display name may carry the repo type as an extension ("x.Texture2D");
    // the plist stem never does.
    if (leaf.endsWith(QLatin1String(".texture2d"), Qt::CaseInsensitive))
        leaf.chop(int(sizeof(".texture2d")) - 1);
    if (leaf.isEmpty()) return SIZE_MAX;

    // Measured location: every one of the 13,665 descriptors lives flat in
    // Package/UIScript/. Exact match first, lowercase second (index names are
    // stored as shipped; repo names are lowercase).
    const std::string base = "Package/UIScript/";
    size_t id = mpk.find(base + leaf.toStdString() + ".plist");
    if (id == SIZE_MAX)
        id = mpk.find(base + leaf.toLower().toStdString() + ".plist");
    return id;
}

bool parse(const uint8_t* data, size_t len, Sheet* out, QString* err)
{
    if (!out) return false;
    out->frames.clear();
    out->textureName.clear();
    out->format = -1;
    if (!data || len < 8) {
        if (err) *err = QStringLiteral("descriptor empty (%1 bytes)").arg(len);
        return false;
    }

    // Encoding detection — never assume. readAsset() has already inflated any
    // ZZZ4 wrapper, so what arrives here is the payload encoding itself.
    if (len >= 8 && memcmp(data, "bplist00", 8) == 0) {
        if (err)
            *err = QStringLiteral(
                "descriptor is a BINARY plist (bplist00) — XML expected; not "
                "yet decoded. Report this so the reader can be extended.");
        return false;
    }
    size_t start = 0;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        start = 3;   // UTF-8 BOM
    while (start < len && (data[start] == ' ' || data[start] == '\t' ||
                           data[start] == '\r' || data[start] == '\n'))
        ++start;
    if (start >= len || data[start] != '<') {
        if (err)
            *err = QStringLiteral(
                       "descriptor encoding not recognized (first bytes: %1) — "
                       "report this so the reader can be extended")
                       .arg(hexHead(data, len));
        return false;
    }

    QXmlStreamReader xml(QByteArray::fromRawData(
        reinterpret_cast<const char*>(data), (int)len));
    QVariantMap root;
    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::StartElement &&
            xml.name() == QLatin1String("dict")) {
            root = readDict(xml).toMap();
            break;
        }
    }
    if (xml.hasError()) {
        if (err)
            *err = QStringLiteral("plist XML error: %1 (line %2)")
                       .arg(xml.errorString())
                       .arg(xml.lineNumber());
        return false;
    }
    if (root.isEmpty()) {
        if (err) *err = QStringLiteral("plist has no top-level dict");
        return false;
    }

    const QVariantMap meta = root.value(QStringLiteral("metadata")).toMap();
    out->format = meta.contains(QStringLiteral("format"))
                      ? meta.value(QStringLiteral("format")).toInt()
                      : -1;
    out->textureName =
        meta.value(QStringLiteral("realTextureFileName"),
                   meta.value(QStringLiteral("textureFileName"))).toString();

    const QVariantMap frames = root.value(QStringLiteral("frames")).toMap();
    if (frames.isEmpty()) {
        if (err) *err = QStringLiteral("plist has no frames dictionary");
        return false;
    }

    for (auto it = frames.constBegin(); it != frames.constEnd(); ++it) {
        const QVariantMap f = it.value().toMap();
        Frame fr;
        fr.name = it.key();
        if (f.contains(QStringLiteral("textureRect"))) {
            // format 3
            const QList<double> r =
                numbersIn(f.value(QStringLiteral("textureRect")).toString());
            if (r.size() < 4) continue;
            fr.x = (int)r[0]; fr.y = (int)r[1];
            fr.w = (int)r[2]; fr.h = (int)r[3];
            fr.rotated = f.value(QStringLiteral("textureRotated")).toBool();
            const QList<double> ss = numbersIn(
                f.value(QStringLiteral("spriteSourceSize")).toString());
            if (ss.size() >= 2) { fr.srcW = (int)ss[0]; fr.srcH = (int)ss[1]; }
            for (const QVariant& a :
                 f.value(QStringLiteral("aliases")).toList())
                fr.aliases << a.toString();
        } else if (f.contains(QStringLiteral("frame"))) {
            // formats 1 and 2
            const QList<double> r =
                numbersIn(f.value(QStringLiteral("frame")).toString());
            if (r.size() < 4) continue;
            fr.x = (int)r[0]; fr.y = (int)r[1];
            fr.w = (int)r[2]; fr.h = (int)r[3];
            fr.rotated = f.value(QStringLiteral("rotated")).toBool();
            const QList<double> ss =
                numbersIn(f.value(QStringLiteral("sourceSize")).toString());
            if (ss.size() >= 2) { fr.srcW = (int)ss[0]; fr.srcH = (int)ss[1]; }
        } else if (f.contains(QStringLiteral("x"))) {
            // format 0
            fr.x = f.value(QStringLiteral("x")).toInt();
            fr.y = f.value(QStringLiteral("y")).toInt();
            fr.w = f.value(QStringLiteral("width")).toInt();
            fr.h = f.value(QStringLiteral("height")).toInt();
            fr.srcW = qAbs(f.value(QStringLiteral("originalWidth")).toInt());
            fr.srcH = qAbs(f.value(QStringLiteral("originalHeight")).toInt());
        } else {
            continue;   // unrecognized frame shape — skip, never invent
        }
        if (fr.w > 0 && fr.h > 0) out->frames.push_back(fr);
    }

    if (out->frames.empty()) {
        if (err)
            *err = QStringLiteral(
                       "frames dictionary parsed but yielded 0 usable frames "
                       "(format %1) — report this")
                       .arg(out->format);
        return false;
    }
    return true;
}

}  // namespace AtlasPlist
