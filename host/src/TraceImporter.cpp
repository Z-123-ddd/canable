#include "TraceImporter.h"

#include <QFile>
#include <QPair>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace trace {
namespace {

// ---------- 小工具 ----------
bool isHexByte(const QString &t)
{
    if (t.size() != 2) return false;
    for (QChar c : t) if (!isxdigit(c.unicode())) return false;
    return true;
}

// 字段是否像一个十六进制 CAN ID（允许末尾 'x'）
bool looksLikeHexId(QString s)
{
    s = s.trimmed();
    if (s.isEmpty()) return false;
    if (s.endsWith(QLatin1Char('x'), Qt::CaseInsensitive)) s.chop(1);
    if (s.isEmpty()) return false;
    for (QChar c : s) if (!isxdigit(c.unicode())) return false;
    return true;
}

bool isDigits(const QString &t)
{
    if (t.isEmpty()) return false;
    for (QChar c : t) if (!c.isDigit()) return false;
    return true;
}

// 解析 CAN ID（可能带末尾 'x' 表示扩展帧），base 为 16 或 10
quint32 parseId(const QString &raw, int base, bool &extended)
{
    QString s = raw.trimmed();
    extended = false;
    if (s.endsWith(QLatin1Char('x'), Qt::CaseInsensitive)) {
        extended = true;
        s.chop(1);
    }
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        s = s.mid(2);
    bool ok = false;
    quint32 id = s.toUInt(&ok, base);
    return ok ? id : 0xFFFFFFFFu;
}

// TRC/按长度判定的 ID：去掉可能的 'x' 尾缀后，>4 位十六进制即扩展帧
quint32 parseIdByLen(const QString &raw, bool &extended)
{
    QString s = raw.trimmed();
    if (s.endsWith(QLatin1Char('x'), Qt::CaseInsensitive)) s.chop(1);
    extended = (s.size() > 4);
    bool ok = false;
    quint32 v = s.toUInt(&ok, 16);
    return ok ? v : 0xFFFFFFFFu;
}

// 连续/空格分隔十六进制串 → 字节
QByteArray hexToBytes(QString s)
{
    s = s.trimmed();
    s.remove(QLatin1Char(' ')).remove(QLatin1Char(':')).remove(QLatin1Char('-'))
     .remove(QLatin1Char('\t')).remove(QLatin1Char('_'));
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        s = s.mid(2);
    return QByteArray::fromHex(s.toLatin1());
}

// 把一行按空白切成至多 maxParts+1 段（最后一段保留其中的空格，用于 TRC 的数据列）
QStringList splitMaxSplit(const QString &s, int maxParts)
{
    QStringList out;
    int i = 0, n = s.size();
    while (i < n && out.size() < maxParts) {
        while (i < n && s.at(i).isSpace()) ++i;
        int start = i;
        while (i < n && !s.at(i).isSpace()) ++i;
        if (i > start) out << s.mid(start, i - start);
    }
    while (i < n && s.at(i).isSpace()) ++i;
    if (i < n) out << s.mid(i);
    return out;
}

QStringList splitWs(const QString &s)
{
    return s.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

// 解析 TRC 头部 ;$DELIMITER= 之后定义的分隔符号：
//   空 = 空白分隔(返回 null QChar)；tab/\t = 制表符；comma/semicolon/space = 对应字符；
//   其余取第一个字符（支持 , ; | 等任意单字符分隔符）。
QChar delimiterFromSpec(const QString &specRaw)
{
    QString spec = specRaw.trimmed();
    if (spec.size() >= 2 && spec.startsWith(QLatin1Char('"')) && spec.endsWith(QLatin1Char('"')))
        spec = spec.mid(1, spec.size() - 2);          // 去掉可能的引号
    if (spec.isEmpty())
        return QChar();
    const QString low = spec.toLower();
    if (low == QStringLiteral("tab") || low == QStringLiteral("\\t")) return QLatin1Char('\t');
    if (low == QStringLiteral("comma"))     return QLatin1Char(',');
    if (low == QStringLiteral("semicolon")) return QLatin1Char(';');
    if (low == QStringLiteral("space"))     return QLatin1Char(' ');
    return spec.at(0);
}

QString normHeader(QString h)
{
    h = h.trimmed().toLower();
    if (!h.isEmpty() && h.at(0) == QChar(0xFEFF)) h.remove(0, 1);   // BOM
    h.remove(QLatin1Char(' ')).remove(QLatin1Char('"'))
     .remove(QLatin1Char('_')).remove(QLatin1Char('-'));
    return h;
}

bool truthy(QString v)
{
    v = v.trimmed().toLower();
    return v == QStringLiteral("1") || v == QStringLiteral("true") || v == QStringLiteral("t")
        || v == QStringLiteral("yes") || v == QStringLiteral("y")
        || v == QStringLiteral("扩展") || v == QStringLiteral("扩展帧")
        || v == QStringLiteral("extended") || v == QStringLiteral("e");
}

// 带引号的最小 CSV 切分（delim 为 , ; 或 \t）
QStringList splitCsvLine(const QString &line, QChar delim)
{
    QStringList out; QString cur; bool inQ = false;
    for (int i = 0; i < line.size(); ++i) {
        QChar c = line.at(i);
        if (inQ) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) { cur += QLatin1Char('"'); ++i; }
                else inQ = false;
            } else cur += c;
        } else {
            if (c == QLatin1Char('"')) inQ = true;
            else if (c == delim) { out << cur; cur.clear(); }
            else cur += c;
        }
    }
    out << cur;
    for (QString &f : out) f = f.trimmed();
    return out;
}

enum Fmt { F_CSV, F_ASC, F_TRC, F_CANDUMP };

Fmt detect(const QStringList &lines)
{
    static QRegularExpression ascMsg(
        QStringLiteral("^\\d+\\.\\d+\\s+(CANFD|\\d+\\s+(\\w+\\s+(Tx|Rx)|ErrorFrame))"),
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression candRead(
        QStringLiteral("^\\S+\\s+[0-9A-Fa-f]{3,8}\\s+\\[\\d+\\]"));
    for (int i = 0; i < lines.size() && i < 60; ++i) {
        const QString s = lines[i].trimmed();
        if (s.isEmpty()) continue;
        if (s.startsWith(QStringLiteral(";$FILEVERSION")) || s.startsWith(QStringLiteral(";$COLUMNS")))
            return F_TRC;
        if (s.contains(QStringLiteral("Begin Triggerblock"), Qt::CaseInsensitive)
            || s.startsWith(QStringLiteral("date "), Qt::CaseInsensitive)
            || s.startsWith(QStringLiteral("base "), Qt::CaseInsensitive))
            return F_ASC;
        if (s.startsWith(QLatin1Char('(')) && s.contains(QLatin1Char('#')))
            return F_CANDUMP;
        if (ascMsg.match(s).hasMatch()) return F_ASC;
        if (candRead.match(s).hasMatch()) return F_CANDUMP;
    }
    return F_CSV;
}

// ---------- Vector ASC ----------
void parseAsc(const QStringList &lines, Result &r)
{
    static QRegularExpression ascMsg(
        QStringLiteral("^\\d+\\.\\d+\\s+(CANFD|\\d+\\s+(\\w+\\s+(Tx|Rx)|ErrorFrame))"),
        QRegularExpression::CaseInsensitiveOption);
    int base = 16;

    for (const QString &raw : lines) {
        const QString s = raw.trimmed();
        if (s.isEmpty()) continue;
        const QString low = s.toLower();

        if (low.startsWith(QStringLiteral("date ")) || low.startsWith(QStringLiteral("//"))
            || low.startsWith(QLatin1Char(';'))) { continue; }
        if (low.startsWith(QStringLiteral("base "))) {
            base = low.contains(QStringLiteral("dec")) ? 10 : 16;
            continue;
        }
        if (low.contains(QStringLiteral("triggerblock"))
            || low.startsWith(QStringLiteral("start of measurement"))
            || low.contains(QStringLiteral("internal events logged"))) { continue; }
        if (!ascMsg.match(s).hasMatch()) { ++r.skipped; continue; }

        const QStringList tok = splitWs(s);
        if (tok.size() < 3) { ++r.skipped; continue; }

        Frame f;
        if (tok[1].compare(QStringLiteral("CANFD"), Qt::CaseInsensitive) == 0) {
            // <ts> CANFD <ch> <dir> <id> [name] <brs> <esi> <dlc> <len> <data...>
            if (tok.size() < 8) { ++r.skipped; continue; }
            f.id = parseId(tok[4], base, f.extended);
            QString brsTok; QStringList after;
            if (isDigits(tok[5])) { brsTok = tok[5]; after = tok.mid(6); }
            else                  { brsTok = tok[6]; after = tok.mid(7); }
            if (after.size() < 3) { ++r.skipped; continue; }
            f.fd = true;
            f.brs = (brsTok == QStringLiteral("1"));
            int dataLen = after[2].toInt();
            QByteArray data;
            for (int i = 3; i < after.size(); ++i)
                if (isHexByte(after[i])) data.append(static_cast<char>(after[i].toInt(nullptr, 16)));
            if (dataLen == 0) { ++r.skipped; continue; }        // 远程帧
            if (data.size() > dataLen) data.truncate(dataLen);
            f.data = data;
        } else {
            // <ts> <ch> <id>[x] <dir> d|r <dlc> [data...]
            if (tok.size() < 5) { ++r.skipped; continue; }
            const QString dtype = tok[4].toLower();
            if (dtype == QStringLiteral("r")) { ++r.skipped; continue; }   // 远程帧
            if (dtype != QStringLiteral("d")) { ++r.skipped; continue; }
            f.id = parseId(tok[2], base, f.extended);
            QByteArray data;
            for (int i = 6; i < tok.size(); ++i)
                if (isHexByte(tok[i])) data.append(static_cast<char>(tok[i].toInt(nullptr, 16)));
            f.data = data;
        }
        if (f.id == 0xFFFFFFFFu) { ++r.skipped; continue; }
        r.frames.append(f);
    }
}

// ---------- PEAK TRC ----------
void parseTrc(const QStringList &lines, Result &r)
{
    int ver = 0;
    QStringList cols;
    QChar delim;   // null = 空白分隔；否则按该字符分隔（;$DELIMITER=）
    for (const QString &raw : lines) {
        const QString s = raw.trimmed();
        if (s.isEmpty()) continue;
        if (s.startsWith(QStringLiteral(";$FILEVERSION"))) {
            ver = static_cast<int>(s.mid(s.indexOf(QLatin1Char('=')) + 1).toDouble() * 100 + 0.5);
            continue;
        }
        if (s.startsWith(QStringLiteral(";$DELIMITER"))) {
            delim = delimiterFromSpec(s.mid(s.indexOf(QLatin1Char('=')) + 1));
            continue;   // 空值 = 空白分隔
        }
        if (s.startsWith(QStringLiteral(";$COLUMNS"))) {
            cols = s.mid(s.indexOf(QLatin1Char('=')) + 1).split(QLatin1Char(','));
            for (QString &c : cols) c = c.trimmed();
            continue;
        }
        if (s.startsWith(QLatin1Char(';'))) continue;

        Frame f;
        if (!cols.isEmpty()) {
            // 有 $COLUMNS：按列名解析。优先用 $DELIMITER 声明的分隔符；
            // 若它切不出合法 ID 列（文件声明与实际不一致），回退自动探测。
            const int idIdx = cols.indexOf(QStringLiteral("I"));
            QList<QStringList> cands;
            if (!delim.isNull())
                cands.append(splitCsvLine(s, delim));                 // 声明分隔符优先
            cands.append(splitMaxSplit(s, cols.size() - 1));          // 空白
            const QChar alts[] = { QLatin1Char(','), QLatin1Char(';'),
                                   QLatin1Char('|'), QLatin1Char('\t') };
            for (QChar c : alts)
                cands.append(splitCsvLine(s, c));                     // 自动探测

            QStringList tok;
            for (const QStringList &c : cands) {
                if (idIdx >= 0 && idIdx < c.size() && looksLikeHexId(c[idIdx])) { tok = c; break; }
            }
            if (tok.isEmpty()) tok = cands.first();

            auto col = [&](int i) { return (i >= 0 && i < tok.size()) ? tok[i].trimmed() : QString(); };

            // 数据列 = 最后一个 'D'；方向列 = 小写 'd'，否则第一个非数据的 'D'
            const int dataIdx = cols.lastIndexOf(QStringLiteral("D"));
            int dirIdx = cols.indexOf(QStringLiteral("d"));
            if (dirIdx < 0)
                for (int i = 0; i < cols.size(); ++i)
                    if (cols[i] == QStringLiteral("D") && i != dataIdx) { dirIdx = i; break; }

            const QString type  = col(cols.indexOf(QStringLiteral("T")));
            const QString idStr = col(idIdx);
            if (idStr.isEmpty()) { ++r.skipped; continue; }
            f.id = parseIdByLen(idStr, f.extended);
            f.rtr = (type.compare(QStringLiteral("RR"), Qt::CaseInsensitive) == 0);
            f.fd  = (type.compare(QStringLiteral("FD"), Qt::CaseInsensitive) == 0
                     || type.compare(QStringLiteral("FB"), Qt::CaseInsensitive) == 0
                     || type.compare(QStringLiteral("FE"), Qt::CaseInsensitive) == 0
                     || type.compare(QStringLiteral("BI"), Qt::CaseInsensitive) == 0);
            f.brs = (type.compare(QStringLiteral("FB"), Qt::CaseInsensitive) == 0
                     || type.compare(QStringLiteral("FE"), Qt::CaseInsensitive) == 0);
            if (!f.rtr) {
                QStringList dfields;
                for (int i = dataIdx; i >= 0 && i < tok.size(); ++i) dfields << tok[i];
                f.data = hexToBytes(dfields.join(QLatin1Char(' ')));
            }
        } else if (ver >= 110) {
            // v1.1: msgnr) time Tx/Rx id dlc data... ; v1.3: msgnr time ch dir id dlc data...
            const QStringList tok = delim.isNull() ? splitMaxSplit(s, 6) : splitCsvLine(s, delim);
            if (ver >= 130) {
                if (tok.size() < 7) { ++r.skipped; continue; }
                f.id = parseIdByLen(tok[4], f.extended);
                QByteArray d; for (int i = 7; i < tok.size(); ++i) if (isHexByte(tok[i])) d.append(static_cast<char>(tok[i].toInt(nullptr, 16)));
                f.data = d;
            } else {
                if (tok.size() < 5) { ++r.skipped; continue; }
                f.id = parseIdByLen(tok[3], f.extended);
                QByteArray d; for (int i = 5; i < tok.size(); ++i) if (isHexByte(tok[i])) d.append(static_cast<char>(tok[i].toInt(nullptr, 16)));
                f.data = d;
            }
        } else {
            // v1.0: msgnr) time id dlc data...
            const QStringList tok = delim.isNull() ? splitWs(s) : splitCsvLine(s, delim);
            if (tok.size() < 4) { ++r.skipped; continue; }
            if (tok[2] == QStringLiteral("FFFFFFFF")) { ++r.skipped; continue; }  // bus info
            f.id = parseIdByLen(tok[2], f.extended);
            QByteArray d; for (int i = 4; i < tok.size(); ++i) if (isHexByte(tok[i])) d.append(static_cast<char>(tok[i].toInt(nullptr, 16)));
            f.data = d;
        }
        if (f.id == 0xFFFFFFFFu) { ++r.skipped; continue; }
        r.frames.append(f);
    }
}

// ---------- candump ----------
void parseCandump(const QStringList &lines, Result &r)
{
    static QRegularExpression candRead(
        QStringLiteral("^\\S+\\s+[0-9A-Fa-f]{3,8}\\s+\\[\\d+\\]"));

    for (const QString &raw : lines) {
        const QString s = raw.trimmed();
        if (s.isEmpty()) continue;

        if (s.startsWith(QLatin1Char('(')) && s.contains(QLatin1Char('#'))) {
            // -L 日志格式：(ts) iface id#data
            const QStringList tok = splitWs(s);
            if (tok.size() < 3) { ++r.skipped; continue; }
            const QString frame = tok[2];
            const int hash = frame.indexOf(QLatin1Char('#'));
            if (hash < 0) { ++r.skipped; continue; }
            const QString idStr = frame.left(hash);
            QString data = frame.mid(hash + 1);
            Frame f;
            f.extended = idStr.size() > 3;
            bool ok = false;
            quint32 id = idStr.toUInt(&ok, 16);
            if (!ok || (id & 0x20000000u)) { ++r.skipped; continue; }   // 错误帧
            f.id = id & 0x1FFFFFFFu;
            if (!data.isEmpty() && data.at(0) == QLatin1Char('#')) {
                f.fd = true;
                const int flags = QString(data.at(1)).toInt(nullptr, 16);
                f.brs = (flags & 0x1) != 0;
                data = data.mid(2);
            }
            if (!data.isEmpty() && (data.at(0) == QLatin1Char('r') || data.at(0) == QLatin1Char('R'))) {
                ++r.skipped; continue;   // 远程帧
            }
            QByteArray b;
            for (int i = 0; i + 1 < data.size(); i += 2)
                b.append(static_cast<char>(data.mid(i, 2).toInt(nullptr, 16)));
            f.data = b;
            r.frames.append(f);
        } else if (candRead.match(s).hasMatch()) {
            // 可读格式：iface id [len] data...
            const QStringList tok = splitWs(s);
            if (tok.size() < 3) { ++r.skipped; continue; }
            if (s.contains(QStringLiteral("remote request"), Qt::CaseInsensitive)) { ++r.skipped; continue; }
            Frame f;
            f.extended = tok[1].size() > 3;
            f.id = tok[1].toUInt(nullptr, 16);
            QByteArray b;
            for (int i = 3; i < tok.size(); ++i)
                if (isHexByte(tok[i])) b.append(static_cast<char>(tok[i].toInt(nullptr, 16)));
            f.data = b;
            r.frames.append(f);
        } else {
            ++r.skipped;
        }
    }
}

// ---------- 通用 CSV ----------
void parseCsv(const QStringList &lines, Result &r)
{
    int headerIdx = -1;
    for (int i = 0; i < lines.size(); ++i)
        if (!lines[i].trimmed().isEmpty()) { headerIdx = i; break; }
    if (headerIdx < 0) return;

    const QString headerLine = lines[headerIdx];
    QChar delim = QLatin1Char(',');
    if (!headerLine.contains(QLatin1Char(',')) && headerLine.contains(QLatin1Char(';')))
        delim = QLatin1Char(';');
    else if (!headerLine.contains(QLatin1Char(',')) && headerLine.contains(QLatin1Char('\t')))
        delim = QLatin1Char('\t');

    const QStringList hdr = splitCsvLine(headerLine, delim);

    int idxId = -1, idxExt = -1, idxType = -1, idxRtr = -1, idxData = -1;
    QVector<QPair<int,int>> byteCols;       // (字节序号, 列下标)
    bool pythonCan = false;

    for (int c = 0; c < hdr.size(); ++c) {
        const QString h = normHeader(hdr[c]);
        if (h.isEmpty()) continue;
        if (h.contains(QStringLiteral("arbitrationid"))) pythonCan = true;

        static QRegularExpression byteRe(QStringLiteral("^(d|data|byte|b)([0-8])$"));
        auto m = byteRe.match(h);
        if (h == QStringLiteral("id") || h == QStringLiteral("canid")
            || h == QStringLiteral("arbitrationid") || h == QStringLiteral("帧id")) {
            idxId = c;
        } else if (m.hasMatch()) {
            byteCols.append({ m.captured(2).toInt(), c });
        } else if (h.contains(QStringLiteral("time")) || h == QStringLiteral("时间")) {
            /* 时间列，未使用 */
        } else if (h.contains(QStringLiteral("ext")) || h == QStringLiteral("帧格式")
                   || h == QStringLiteral("ide") || h == QStringLiteral("格式")) {
            idxExt = c;
        } else if (h.contains(QStringLiteral("rtr")) || h.contains(QStringLiteral("remote"))) {
            idxRtr = c;
        } else if (h.contains(QStringLiteral("帧类型")) || h == QStringLiteral("type")
                   || h == QStringLiteral("type2")) {
            idxType = c;
        } else if (h.contains(QStringLiteral("dlc")) || h.contains(QStringLiteral("len"))
                   || h.contains(QStringLiteral("长度")) || h.contains(QStringLiteral("size"))) {
            /* 长度列，由数据字节数决定 */
        } else if (h == QStringLiteral("data") || h == QStringLiteral("数据") || h.contains(QStringLiteral("data"))) {
            idxData = c;
        }
    }
    if (idxId < 0) { r.error = QStringLiteral("CSV 缺少 ID 列"); return; }
    std::sort(byteCols.begin(), byteCols.end(),
              [](const QPair<int,int> &a, const QPair<int,int> &b) { return a.first < b.first; });

    for (int i = headerIdx + 1; i < lines.size(); ++i) {
        if (lines[i].trimmed().isEmpty()) continue;
        const QStringList row = splitCsvLine(lines[i], delim);
        if (row.size() <= idxId) { ++r.skipped; continue; }

        Frame f;
        const QString idRaw = row[idxId];
        if (idRaw.trimmed().isEmpty()) { ++r.skipped; continue; }
        bool ext = (idxExt >= 0 && idxExt < row.size()) ? truthy(row[idxExt]) : false;
        QString idStr = idRaw.trimmed();
        if (idStr.endsWith(QLatin1Char('x'), Qt::CaseInsensitive)) { ext = true; idStr.chop(1); }
        bool ok = false;
        f.id = idStr.toUInt(&ok, 16);
        if (!ok) { ++r.skipped; continue; }
        f.extended = ext;

        if (idxType >= 0 && idxType < row.size()) {
            const QString v = row[idxType].toLower();
            if (v.contains(QStringLiteral("fd"))) f.fd = true;
            if (v.contains(QStringLiteral("加速")) || v.contains(QStringLiteral("brs"))) { f.fd = true; f.brs = true; }
            if (v.contains(QStringLiteral("远程")) || v.contains(QStringLiteral("remote"))) f.rtr = true;
        }
        if (idxRtr >= 0 && idxRtr < row.size() && truthy(row[idxRtr])) f.rtr = true;

        if (!f.rtr) {
            if (idxData >= 0 && idxData < row.size()) {
                if (pythonCan) f.data = QByteArray::fromBase64(row[idxData].trimmed().toLatin1());
                else           f.data = hexToBytes(row[idxData]);
            } else if (!byteCols.isEmpty()) {
                QByteArray d;
                for (const auto &bc : byteCols) {
                    if (bc.second >= row.size()) continue;
                    const QString cell = row[bc.second].trimmed();
                    if (cell.isEmpty()) continue;
                    d.append(static_cast<char>(cell.toInt(nullptr, 16)));
                }
                f.data = d;
            }
        }
        r.frames.append(f);
    }
}

} // namespace

Result load(const QString &path)
{
    Result r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("无法打开文件：%1").arg(path);
        return r;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QString text = QString::fromUtf8(raw);
    if (text.contains(QChar(0xFFFD)))
        text = QString::fromLocal8Bit(raw);           // 非 UTF-8（如 GBK）回退
    if (!text.isEmpty() && text.at(0) == QChar(0xFEFF))
        text.remove(0, 1);                            // 去掉 BOM

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]")));

    const Fmt fmt = detect(lines);
    switch (fmt) {
    case F_TRC:      r.format = QStringLiteral("PEAK TRC");   parseTrc(lines, r);      break;
    case F_ASC:      r.format = QStringLiteral("Vector ASC"); parseAsc(lines, r);      break;
    case F_CANDUMP:  r.format = QStringLiteral("candump");    parseCandump(lines, r);  break;
    default:         r.format = QStringLiteral("通用 CSV");   parseCsv(lines, r);      break;
    }

    if (!r.error.isEmpty())
        return r;                                     // CSV 缺 ID 列等
    r.ok = true;
    return r;
}

} // namespace trace
