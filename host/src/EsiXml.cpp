#include "EsiXml.h"

#include <QFile>
#include <QXmlStreamReader>

namespace {

// "#x1600" / "0x1600" / "0x0018" / "24" → 无符号整数
quint32 parseIndex(const QString &s)
{
    QString t = s.trimmed();
    if (t.startsWith(QStringLiteral("#x"), Qt::CaseInsensitive)) t = t.mid(2);
    else if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) t = t.mid(2);
    bool ok = false;
    const quint32 v = t.toUInt(&ok, 16);
    return ok ? v : (quint32)s.trimmed().toUInt();
}

// 解析一个 <SubItem>…</SubItem>（进入时 reader 位于起始元素上）
EsiObjEntry parseSubItem(QXmlStreamReader &r)
{
    EsiObjEntry e;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement() && r.name() == QStringLiteral("SubItem")) break;
        if (!r.isStartElement()) continue;
        const auto n = r.name();
        if (n == QStringLiteral("Name"))        e.name = r.readElementText();
        else if (n == QStringLiteral("Type"))   e.type = r.readElementText();
        else if (n == QStringLiteral("BitSize")) e.bitSize = r.readElementText().toInt();
        else if (n == QStringLiteral("Access")) e.access = r.readElementText();
        else if (n == QStringLiteral("DefaultData"))
            e.def = QByteArray::fromHex(r.readElementText().trimmed().toLatin1());
    }
    return e;
}

// 解析一个 <Object>…</Object>（进入时 reader 位于起始元素上），产出若干对象条目
void parseObject(QXmlStreamReader &r, QVector<EsiObjEntry> *out)
{
    quint16 index = 0; QString name, type, access; int bitSize = 0; QByteArray def;
    QVector<EsiObjEntry> subs;

    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement() && r.name() == QStringLiteral("Object")) break;
        if (!r.isStartElement()) continue;
        const auto n = r.name();
        if (n == QStringLiteral("Index"))        index = (quint16)parseIndex(r.readElementText());
        else if (n == QStringLiteral("Name"))    name = r.readElementText();
        else if (n == QStringLiteral("Type"))    type = r.readElementText();
        else if (n == QStringLiteral("BitSize")) bitSize = r.readElementText().toInt();
        else if (n == QStringLiteral("Access"))  access = r.readElementText();
        else if (n == QStringLiteral("DefaultData"))
            def = QByteArray::fromHex(r.readElementText().trimmed().toLatin1());
        else if (n == QStringLiteral("SubItem")) subs.append(parseSubItem(r));
    }

    if (!subs.isEmpty()) {
        // record/array 型：子项按出现顺序即子索引 0,1,2…
        for (int i = 0; i < subs.size(); ++i) {
            EsiObjEntry e = subs.at(i);
            e.index = index;
            e.sub = (quint8)i;
            if (e.type.isEmpty()) e.type = type;
            out->append(e);
        }
    } else {
        EsiObjEntry e;
        e.index = index; e.sub = 0; e.name = name;
        e.type = type; e.bitSize = bitSize; e.access = access; e.def = def;
        out->append(e);
    }
}

// 解析一条 <Entry>…</Entry>
EsiPdoEntry parsePdoEntry(QXmlStreamReader &r)
{
    EsiPdoEntry e;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement() && r.name() == QStringLiteral("Entry")) break;
        if (!r.isStartElement()) continue;
        const auto n = r.name();
        if (n == QStringLiteral("Index"))         e.index = (quint16)parseIndex(r.readElementText());
        else if (n == QStringLiteral("SubIndex")) e.sub = (quint8)parseIndex(r.readElementText());
        else if (n == QStringLiteral("BitLen"))   e.bitLen = r.readElementText().toInt();
        else if (n == QStringLiteral("Name"))     e.name = r.readElementText();
    }
    return e;
}

// 解析一个 <RxPdo>/<TxPdo>…</RxPdo|TxPdo>
void parsePdo(QXmlStreamReader &r, QVector<EsiPdo> *out)
{
    EsiPdo p;
    const auto attrs = r.attributes();
    if (attrs.hasAttribute(QStringLiteral("Sm")))    p.sm = attrs.value(QStringLiteral("Sm")).toInt();
    if (attrs.hasAttribute(QStringLiteral("Fixed"))) p.fixed = (attrs.value(QStringLiteral("Fixed")) == QStringLiteral("true"));

    const auto pdoName = r.name().toString();
    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement() && r.name() == pdoName) break;
        if (!r.isStartElement()) continue;
        const auto n = r.name();
        if (n == QStringLiteral("Index"))     p.index = (quint16)parseIndex(r.readElementText());
        else if (n == QStringLiteral("Name")) p.name = r.readElementText();
        else if (n == QStringLiteral("Entry")) p.entries.append(parsePdoEntry(r));
    }
    out->append(p);
}

} // namespace

const EsiObjEntry *EsiXmlData::findObject(quint16 index, quint8 sub) const
{
    for (const EsiObjEntry &e : objects)
        if (e.index == index && e.sub == sub) return &e;
    return nullptr;
}

int EsiXmlData::controlwordByteOffset() const
{
    for (const EsiPdo &p : rxPdos) {
        int bits = 0;
        for (const EsiPdoEntry &e : p.entries) {
            if (e.index == 0x6040 && e.sub == 0) return bits / 8;
            bits += e.bitLen;
        }
    }
    return -1;
}

int EsiXmlData::statuswordByteOffset() const
{
    for (const EsiPdo &p : txPdos) {
        int bits = 0;
        for (const EsiPdoEntry &e : p.entries) {
            if (e.index == 0x6041 && e.sub == 0) return bits / 8;
            bits += e.bitLen;
        }
    }
    return -1;
}

bool parseEsiXml(const QByteArray &xml, EsiXmlData *out, QString *err)
{
    if (!out) return false;
    *out = EsiXmlData();

    QXmlStreamReader r(xml);
    QString devName;
    bool inDevice = false;
    int deviceDepth = 0;
    int depth = 0;

    while (!r.atEnd()) {
        const auto tok = r.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            ++depth;
            const auto n = r.name();
            if (n == QStringLiteral("Device") && !inDevice) { inDevice = true; deviceDepth = depth; }
            else if (n == QStringLiteral("Object")) parseObject(r, &out->objects);
            else if (n == QStringLiteral("RxPdo"))  parsePdo(r, &out->rxPdos);
            else if (n == QStringLiteral("TxPdo"))  parsePdo(r, &out->txPdos);
            else if (inDevice && devName.isEmpty() && n == QStringLiteral("Name"))
                devName = r.readElementText();
        } else if (tok == QXmlStreamReader::EndElement) {
            if (inDevice && depth == deviceDepth) inDevice = false;
            --depth;
        }
    }
    if (r.hasError()) { if (err) *err = r.errorString(); return false; }

    out->device = devName;
    if (out->objects.isEmpty() && out->rxPdos.isEmpty() && out->txPdos.isEmpty()) {
        if (err) *err = QStringLiteral("未在 XML 中找到 <Object> 或 <RxPdo>/<TxPdo>");
        return false;
    }
    return true;
}

bool loadEsiXml(const QString &path, EsiXmlData *out, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法打开文件：%1").arg(path);
        return false;
    }
    const QByteArray xml = f.readAll();
    f.close();
    return parseEsiXml(xml, out, err);
}
