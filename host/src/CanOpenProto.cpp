#include "CanOpenProto.h"

#include <QFile>
#include <QChar>
#include <QStringList>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <cmath>
#include <cstring>

namespace canopen {

// ---- 小端写助手 ----
static void putLE(QByteArray &b, int off, quint32 v, int nbytes)
{
    for (int i = 0; i < nbytes; ++i) b[off + i] = char((v >> (8 * i)) & 0xFF);
}

QVector<QString> functionNames()
{
    return {
        QStringLiteral("NMT（网络管理）"),          // 0x000
        QStringLiteral("SYNC（同步）"),             // 0x080
        QStringLiteral("EMCY(读) 紧急报文"),        // 0x080+node
        QStringLiteral("TPDO1(读) 0x180+node"),     // 0x180+node
        QStringLiteral("RPDO1 0x200+node"),
        QStringLiteral("TPDO2(读) 0x280+node"),
        QStringLiteral("RPDO2 0x300+node"),
        QStringLiteral("TPDO3(读) 0x380+node"),
        QStringLiteral("RPDO3 0x400+node"),
        QStringLiteral("TPDO4(读) 0x480+node"),
        QStringLiteral("RPDO4 0x500+node"),
        QStringLiteral("TSDO(读) 0x580+node"),
        QStringLiteral("RSDO（SDO 请求）0x600+node"),
        QStringLiteral("心跳 0x700+node"),          // 0x700+node
    };
}

quint32 nmtId()  { return 0x000; }
quint32 syncId() { return 0x080; }

quint32 cobId(Function f, int node)
{
    const quint32 n = (quint32)(node & 0x7F);
    switch (f) {
    case F_NMT:       return 0x000;
    case F_SYNC:      return 0x080;
    case F_EMCY:      return 0x080 + n;
    case F_TPDO1:     return 0x180 + n;
    case F_RPDO1:     return 0x200 + n;
    case F_TPDO2:     return 0x280 + n;
    case F_RPDO2:     return 0x300 + n;
    case F_TPDO3:     return 0x380 + n;
    case F_RPDO3:     return 0x400 + n;
    case F_TPDO4:     return 0x480 + n;
    case F_RPDO4:     return 0x500 + n;
    case F_TSDO:      return 0x580 + n;
    case F_RSDO:      return 0x600 + n;
    case F_HEARTBEAT: return 0x700 + n;
    default:          return 0;
    }
}

QByteArray buildNmt(int node, quint8 cmd)
{
    QByteArray b(2, '\0');
    b[0] = char(cmd);
    b[1] = char((quint8)(node & 0x7F));
    return b;
}

QByteArray buildHeartbeat(int node, quint8 state)
{
    Q_UNUSED(node);
    QByteArray b(1, '\0');
    b[0] = char(state);
    return b;
}

QVector<QString> sdoTypeNames()
{
    return { QStringLiteral("int8"), QStringLiteral("int16"), QStringLiteral("int32"),
             QStringLiteral("uint8"), QStringLiteral("uint16"), QStringLiteral("uint32"),
             QStringLiteral("float32") };
}

int sdoTypeBytes(int type)
{
    switch (type) {
    case SDO_INT8: case SDO_UINT8:   return 1;
    case SDO_INT16: case SDO_UINT16: return 2;
    default:                         return 4;   // int32/uint32/float32
    }
}

QByteArray buildSdoWrite(int node, quint16 index, quint8 sub, double value, int type)
{
    Q_UNUSED(node);
    const int nb = sdoTypeBytes(type);
    QByteArray b(8, '\0');
    // 命令码：n = 4 - 字节数，置于 bit3..2
    const int n = 4 - nb;
    b[0] = char(0x23 | ((n & 0x03) << 2));
    b[1] = char(index & 0xFF);
    b[2] = char((index >> 8) & 0xFF);
    b[3] = char(sub);
    quint32 u = 0;
    switch (type) {
    case SDO_INT8:   u = (quint32)(quint8)(qint8)std::lround(value);  break;
    case SDO_INT16:  u = (quint32)(quint16)(qint16)std::lround(value); break;
    case SDO_INT32:  u = (quint32)(qint32)std::llround(value);         break;
    case SDO_UINT8:  u = (quint32)(quint8)std::llround(value);         break;
    case SDO_UINT16: u = (quint32)(quint16)std::llround(value);        break;
    case SDO_UINT32: u = (quint32)std::llround(value);                 break;
    case SDO_FLOAT32: { float f = (float)value; memcpy(&u, &f, 4);     break; }
    }
    putLE(b, 4, u, nb);
    return b;
}

QByteArray buildSdoRead(int node, quint16 index, quint8 sub)
{
    Q_UNUSED(node);
    QByteArray b(8, '\0');
    b[0] = char(0x40);                 // 加速读请求
    b[1] = char(index & 0xFF);
    b[2] = char((index >> 8) & 0xFF);
    b[3] = char(sub);
    return b;
}

Wire buildPdo(Function f, int node, const QByteArray &data)
{
    Wire w;
    w.id = cobId(f, node);
    w.data = data.left(8);
    return w;
}

// ---- 单位/换算（输出轴 → 电机侧 counts）----
static const double kTwoPiCO = 6.283185307179586476925286766559;

QVector<QString> coUnitNames(CoInputKind kind)
{
    switch (kind) {
    case CI_POS:   return { QStringLiteral("转"), QStringLiteral("角度(°)"), QStringLiteral("弧度(rad)") };
    case CI_VEL:   return { QStringLiteral("转每秒(rev/s)"), QStringLiteral("弧度每秒(rad/s)"), QStringLiteral("转每分(rpm)") };
    case CI_ACCEL: return { QStringLiteral("转每二次方秒(rev/s²)"), QStringLiteral("弧度每二次方秒(rad/s²)") };
    case CI_TORQUE: default: return {};
    }
}

double countsPerRev()
{
    const QVector<OdEntry> od = objectDictionary();
    for (const OdEntry &e : od)
        if (e.index == 0x608F && e.sub == 1) {
            bool ok = false;
            const double v = e.dflt.toDouble(&ok);
            if (ok && v > 0.0) return v;
        }
    return 16384.0;
}

double posCounts(double v, int unitIdx, double gear)
{
    double turns;                                   // 输出轴转
    if (unitIdx == 1)      turns = v / 360.0;       // 角度
    else if (unitIdx == 2) turns = v / kTwoPiCO;    // 弧度
    else                   turns = v;               // 转
    return turns * gear * countsPerRev();
}

double velCounts(double v, int unitIdx, double gear)
{
    double tps;                                     // 输出轴转/秒
    if (unitIdx == 1)      tps = v / kTwoPiCO;      // rad/s
    else if (unitIdx == 2) tps = v / 60.0;          // rpm
    else                   tps = v;                 // rev/s
    return tps * gear * countsPerRev();
}

double accelCounts(double v, int unitIdx, double gear)
{
    double tps2 = (unitIdx == 1) ? v / kTwoPiCO : v; // rad/s² → rev/s²；否则 rev/s²
    return tps2 * gear * countsPerRev();
}

double torqueCounts(double nm, double gear)
{
    const double g = (gear == 0.0) ? 1.0 : gear;
    return nm * 100.0 / g;                          // 0.01 Nm/count
}

// ---- 手册「CANopen 协议实战」示例（node_id=1）----
static QVector<Example> g_examples;
static void initExamples()
{
    if (!g_examples.isEmpty()) return;
    auto add = [](Example &e, quint32 id, const char *hex) {
        e.ids.append(id); e.payloads.append(QByteArray::fromHex(hex));
    };
    const quint32 NMT = 0x000, SDO = 0x601;
    auto mk = [&](const char *name, const char *desc) {
        Example e; e.name = QString::fromUtf8(name); e.desc = QString::fromUtf8(desc);
        return e;
    };

    { Example e = mk("周期同步力矩模式", "NMT→op，SDO 设模式/输入，控制字 6/7/15，RPDO4 目标转矩");
      add(e, NMT, "0101");
      add(e, SDO, "2F6060000A000000");   // 操作模式 6060h = 10 (力矩)
      add(e, SDO, "2F02200106000000");   // 2002h:01 输入模式 = 6 (斜坡力矩)
      add(e, SDO, "2B40600006000000");   // 控制字 6040h = 6
      add(e, SDO, "2B40600007000000");   // 6040h = 7
      add(e, SDO, "2B4060000F000000");   // 6040h = 15 (伺服运行)
      add(e, 0x501, "0F006400");         // RPDO4 目标转矩
      e.inputs.append({ QStringLiteral("目标力矩"), CI_TORQUE, 6, 2, 2, 1.0, 0 });  // 1.0Nm→100
      g_examples.append(e); }

    { Example e = mk("周期同步速度模式", "NMT→op，SDO 设模式/输入，控制字 6/7/15，RPDO3 目标速度");
      add(e, NMT, "0101");
      add(e, SDO, "2F60600009000000");   // 6060h = 9 (速度)
      add(e, SDO, "2F02200102000000");   // 2002h:01 = 2 (速度斜坡)
      add(e, SDO, "2B40600006000000");
      add(e, SDO, "2B40600007000000");
      add(e, SDO, "2B4060000F000000");
      add(e, 0x401, "0F0000080000");     // RPDO3 目标速度
      e.inputs.append({ QStringLiteral("目标速度"), CI_VEL, 6, 2, 4, 0.125, 0 });  // 0.125 rev/s
      g_examples.append(e); }

    { Example e = mk("周期同步位置模式", "NMT→op，SDO 设模式/输入，控制字 6/7/15，RPDO2 目标位置");
      add(e, NMT, "0101");
      add(e, SDO, "2F60600008000000");   // 6060h = 8 (位置)
      add(e, SDO, "2F02200103000000");   // 2002h:01 = 3 (位置滤波)
      add(e, SDO, "2B40600006000000");
      add(e, SDO, "2B40600007000000");
      add(e, SDO, "2B4060000F000000");
      add(e, 0x301, "0F0000400000");     // RPDO2 目标位置
      e.inputs.append({ QStringLiteral("目标位置"), CI_POS, 6, 2, 4, 1.0, 0 });    // 1 转
      g_examples.append(e); }

    { Example e = mk("轮廓位置模式", "NMT→op，设轮廓速度/加减速，控制字 6/7/15，RPDO2 目标位置，再 0x1F 触发");
      add(e, NMT, "0101");
      add(e, SDO, "2F60600001000000");   // 6060h = 1 (轮廓位置)
      add(e, SDO, "2381600000800C00");   // 6081h 轮廓速度
      add(e, SDO, "2383600000800200");   // 6083h 轮廓加速度
      add(e, SDO, "2384600000800200");   // 6084h 轮廓减速度
      add(e, SDO, "2B40600006000000");
      add(e, SDO, "2B40600007000000");
      add(e, SDO, "2B4060000F000000");
      add(e, 0x301, "0F0000400000");     // RPDO2 目标位置
      add(e, SDO, "2B4060001F000000");   // 6040h = 31 (新目标位置)
      e.inputs.append({ QStringLiteral("目标位置"),   CI_POS,   8, 2, 4, 1.0, 0 });   // 1 转
      e.inputs.append({ QStringLiteral("轮廓速度"),   CI_VEL,   2, 4, 4, 50.0, 0 });  // 6081h 50 rev/s
      e.inputs.append({ QStringLiteral("轮廓加速度"), CI_ACCEL, 3, 4, 4, 10.0, 0 });  // 6083h 10 rev/s^2
      e.inputs.append({ QStringLiteral("轮廓减速度"), CI_ACCEL, 4, 4, 4, 10.0, 0 });  // 6084h
      g_examples.append(e); }

    { Example e = mk("回零模式", "NMT→op，回零方式/偏移/速度/加速度，控制字 6/7/15/31");
      add(e, NMT, "0101");
      add(e, SDO, "2F60600006000000");   // 6060h = 6 (回零)
      add(e, SDO, "2F98600011000000");   // 6098h 回零方式 = 17
      add(e, SDO, "237C600000400100");   // 607Ch 回零偏移
      add(e, SDO, "2399600100200000");   // 6099h:01 回零速度
      add(e, SDO, "239A600000000400");   // 609Ah 回零加速度
      add(e, SDO, "2B40600006000000");
      add(e, SDO, "2B40600007000000");
      add(e, SDO, "2B4060000F000000");
      add(e, SDO, "2B4060001F000000");   // 6040h = 31 (回零进行中)
      e.inputs.append({ QStringLiteral("回零偏移"),   CI_POS,   3, 4, 4, 5.0, 0 });   // 607Ch 5 转
      e.inputs.append({ QStringLiteral("回零速度"),   CI_VEL,   4, 4, 4, 0.5, 0 });   // 6099h 0.5 rev/s
      e.inputs.append({ QStringLiteral("回零加速度"), CI_ACCEL, 5, 4, 4, 16.0, 0 });  // 609Ah 16 rev/s^2
      g_examples.append(e); }

    { Example e = mk("清零模式", "回零模式 + 回零方式 35（速度环/位置环清零），再退出回零模式");
      add(e, SDO, "2F60600006000000");   // 6060h = 6 (回零)
      add(e, SDO, "2F98600023000000");   // 6098h = 35 (清零)
      add(e, SDO, "2F60600001000000");   // 6060h = 1 (退出，轮廓位置)
      g_examples.append(e); }

    { Example e = mk("保存配置", "1010h:01 写入 'save'(0x73617665) 保存并重启");
      add(e, SDO, "2310100165766173");
      g_examples.append(e); }

    { Example e = mk("PDO 映射修改", "预操作态下把 TPDO1 映射改为[6064h 当前位置][606Ch 当前速度]");
      add(e, SDO, "2300180181010080");   // 1800h:01 无效 TPDO1
      add(e, SDO, "2F001A0000000000");   // 1A00h:00 清空映射数
      add(e, SDO, "23001A0120006460");   // 1A00h:01 = 6064h 0020
      add(e, SDO, "23001A0220006C60");   // 1A00h:02 = 606Ch 0020
      add(e, SDO, "2F001A0002000000");   // 1A00h:00 = 2
      add(e, SDO, "2300180181010000");   // 1800h:01 恢复 TPDO1 ID
      g_examples.append(e); }
}

const QVector<Example> &examples()
{
    initExamples();
    return g_examples;
}

QVector<Wire> buildExampleFrames(const Example &e, const QVector<double> &values,
                                 const QVector<int> &units, double gear)
{
    QVector<QByteArray> payloads = e.payloads;   // 从默认模板出发
    for (int i = 0; i < payloads.size() && i < e.ids.size(); ++i) {
        // 每个输入槽：把用户值换算为 counts 后按小端写入对应帧的数值段
        for (int k = 0; k < e.inputs.size(); ++k) {
            const ExampleInput &in = e.inputs.at(k);
            if (in.frameIndex != i) continue;
            const double v = (k < values.size()) ? values.at(k) : in.defValue;
            const int u = (k < units.size()) ? units.at(k) : in.defUnit;
            double counts = 0.0;
            switch (in.kind) {
            case CI_POS:    counts = posCounts(v, u, gear);   break;
            case CI_VEL:    counts = velCounts(v, u, gear);   break;
            case CI_ACCEL:  counts = accelCounts(v, u, gear); break;
            case CI_TORQUE: counts = torqueCounts(v, gear);   break;
            }
            const qint64 raw = (qint64)std::llround(counts);
            QByteArray &pl = payloads[i];
            const int n = qMin(in.byteLen, pl.size() - in.offset);
            for (int b = 0; b < n; ++b)
                pl[in.offset + b] = char((raw >> (8 * b)) & 0xFF);
        }
    }
    QVector<Wire> out;
    for (int i = 0; i < e.ids.size() && i < payloads.size(); ++i)
        out.append(Wire{ e.ids.at(i), payloads.at(i) });
    return out;
}

int odBitLen(const OdEntry &e)
{
    const QString t = e.dataType;
    if (t == QLatin1String("INT8") || t == QLatin1String("UINT8") || t == QLatin1String("BOOL")) return 8;
    if (t == QLatin1String("INT16") || t == QLatin1String("UINT16")) return 16;
    return 32;
}

QVector<Wire> buildPdoMapChange(int node, Function pdo, const QVector<PdoMapEntry> &objects)
{
    QVector<Wire> out;
    const int comm = pdoCommObjectIndex(pdo);
    const int map  = pdoMappingObjectIndex(pdo);
    if (comm < 0 || map < 0) return out;
    auto sdoW = [&](quint16 idx, quint8 sub, double v, int ty) {
        out.append(Wire{ canopen::cobId(F_RSDO, node), buildSdoWrite(node, idx, sub, v, ty) });
    };
    const quint32 cob = cobId(pdo, node);
    // 1) 无效该 PDO（COB-ID 最高位=1）
    sdoW((quint16)comm, 1, double(cob | 0x80000000u), SDO_UINT32);
    // 2) 清除原有映射内容（映射对象子索引 0 写入 0）
    sdoW((quint16)map, 0, 0, SDO_UINT8);
    // 3) 逐条写入新的映射内容（子索引 1..N）
    for (int k = 0; k < objects.size(); ++k) {
        const PdoMapEntry &m = objects.at(k);
        const quint32 entry = ((quint32)m.index << 16) | ((quint32)(m.sub & 0xFF) << 8)
                              | (quint32)(m.bits & 0xFF);
        sdoW((quint16)map, (quint8)(k + 1), double(entry), SDO_UINT32);
    }
    // 4) 写入映射个数（映射对象子索引 0 = N）
    sdoW((quint16)map, 0, double(objects.size()), SDO_UINT8);
    // 5) 有效该 PDO（COB-ID 最高位=0）
    sdoW((quint16)comm, 1, double(cob), SDO_UINT32);
    return out;
}

// ---- EDS 文本 / 对象字典 ----
static QString g_edsText;
static bool    g_edsLoaded = false;
static QString g_edsSource;
static QVector<OdEntry> g_od;
static bool    g_odDirty = true;

QString externalEdsPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("eds/ds402.eds"));
}

void reloadEds()
{
    g_edsLoaded = false;
    g_edsText.clear();
    g_edsSource.clear();
    g_odDirty = true;
}

QString edsText()
{
    if (!g_edsLoaded) {
        g_edsLoaded = true;
        // 优先外部文件（<exe目录>/eds/ds402.eds），否则内嵌资源
        QFile ext(externalEdsPath());
        if (ext.exists() && ext.open(QIODevice::ReadOnly | QIODevice::Text)) {
            g_edsText = QString::fromUtf8(ext.readAll());
            g_edsSource = QStringLiteral("外部: ") + externalEdsPath();
        } else {
            QFile res(QStringLiteral(":/eds/ds402.eds"));
            if (res.open(QIODevice::ReadOnly | QIODevice::Text)) {
                g_edsText = QString::fromUtf8(res.readAll());
                g_edsSource = QStringLiteral("内置 ds402.eds");
            }
        }
    }
    return g_edsText;
}

QString edsSourceName()
{
    (void)edsText();
    return g_edsSource;
}

bool importEdsFile(const QString &srcPath, QString *err)
{
    QFile src(srcPath);
    if (!src.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法打开所选 EDS 文件");
        return false;
    }
    const QByteArray bytes = src.readAll();
    src.close();

    const QString dst = externalEdsPath();
    QDir().mkpath(QFileInfo(dst).absolutePath());
    QFile out(dst);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("无法写入 %1").arg(dst);
        return false;
    }
    out.write(bytes);
    out.close();

    reloadEds();
    if (edsText().trimmed().isEmpty() || objectDictionary().isEmpty()) {
        if (err) *err = QStringLiteral("导入的 EDS 为空或无法解析");
        return false;
    }
    return true;
}

void resetToEmbeddedEds()
{
    QFile::remove(externalEdsPath());
    reloadEds();
}

// DataType 代码 → 人类可读名（CiA301 标准）
static QString dataTypeName(const QString &code)
{
    bool ok = false;
    const int v = code.trimmed().toInt(&ok, 0);
    if (!ok) return code.trimmed();
    switch (v) {
    case 0x0001: return QStringLiteral("BOOL");
    case 0x0002: return QStringLiteral("INT8");
    case 0x0003: return QStringLiteral("INT16");
    case 0x0004: return QStringLiteral("INT32");
    case 0x0005: return QStringLiteral("UINT8");
    case 0x0006: return QStringLiteral("UINT16");
    case 0x0007: return QStringLiteral("UINT32");
    case 0x0008: return QStringLiteral("REAL32");
    case 0x0009: return QStringLiteral("STRING");
    default:     return code.trimmed();
    }
}

QString odDisplay(const OdEntry &e)
{
    const QString sub = (e.sub < 0)
        ? QString()
        : QStringLiteral(":%1").arg(e.sub, 2, 16, QLatin1Char('0')).toUpper();
    return (QStringLiteral("0x%1%2  %3 (%4)")
                .arg(e.index, 4, 16, QLatin1Char('0')).arg(sub).arg(e.name, e.dataType)).toUpper();
}

QByteArray encodeOdValue(const OdEntry &e, double value)
{
    auto put = [](QByteArray &b, quint32 u, int n) {
        for (int i = 0; i < n; ++i) b[i] = char((u >> (8 * i)) & 0xFF);
    };
    const QString t = e.dataType;
    if (t == QLatin1String("INT8"))   { QByteArray b(1, 0); put(b, (quint32)(quint8)(qint8)std::lround(value), 1);   return b; }
    if (t == QLatin1String("UINT8"))  { QByteArray b(1, 0); put(b, (quint32)(quint8)std::llround(value), 1);         return b; }
    if (t == QLatin1String("INT16"))  { QByteArray b(2, 0); put(b, (quint32)(quint16)(qint16)std::lround(value), 2);  return b; }
    if (t == QLatin1String("UINT16")) { QByteArray b(2, 0); put(b, (quint32)(quint16)std::llround(value), 2);        return b; }
    if (t == QLatin1String("REAL32")) { QByteArray b(4, 0); float f = (float)value; quint32 u; memcpy(&u, &f, 4); put(b, u, 4); return b; }
    QByteArray b(4, 0);   // INT32 / UINT32 / 其它默认 32 位
    put(b, (quint32)std::llround(value), 4);
    return b;
}

QVector<OdEntry> objectDictionary()
{
    if (!g_odDirty) return g_od;
    g_odDirty = false;
    g_od.clear();

    const QString txt = edsText();
    if (txt.isEmpty()) return g_od;

    const QStringList lines = txt.split(QChar(10));
    OdEntry cur; bool inSection = false; bool has = false;
    auto flush = [&]() {
        if (inSection && has) g_od.append(cur);
        cur = OdEntry();
        has = false;
    };
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QChar(';'))) continue;
        if (line.startsWith(QChar('[')) && line.endsWith(QChar(']'))) {
            flush();
            const QString sec = line.mid(1, line.size() - 2);
            // 形如 1003 或 1003sub0
            const int pos = sec.indexOf(QStringLiteral("sub"), 0, Qt::CaseInsensitive);
            bool ok = false;
            if (pos > 0) {
                cur.index = (quint16)sec.left(pos).toInt(&ok, 16);
                cur.sub   = sec.mid(pos + 3).toInt(&ok, 16);
            } else {
                cur.index = (quint16)sec.toInt(&ok, 16);
                cur.sub   = -1;
            }
            inSection = ok;
            has = false;
            continue;
        }
        if (!inSection) continue;
        const int eq = line.indexOf(QChar('='));
        if (eq < 0) continue;
        const QString key = line.left(eq).trimmed();
        const QString val = line.mid(eq + 1).trimmed();
        if (key.compare(QStringLiteral("ParameterName"), Qt::CaseInsensitive) == 0) { cur.name = val; has = true; }
        else if (key.compare(QStringLiteral("AccessType"), Qt::CaseInsensitive) == 0) { cur.access = val; has = true; }
        else if (key.compare(QStringLiteral("DataType"), Qt::CaseInsensitive) == 0) { cur.dataType = dataTypeName(val); has = true; }
        else if (key.compare(QStringLiteral("DefaultValue"), Qt::CaseInsensitive) == 0) { cur.dflt = val; has = true; }
        else if (key.compare(QStringLiteral("PDOMapping"), Qt::CaseInsensitive) == 0) { cur.mappable = (val != QLatin1String("0")); }
    }
    flush();
    return g_od;
}

const OdEntry *findOd(quint16 index, int sub)
{
    const int s = (sub < 0) ? 0 : sub;
    const QVector<OdEntry> od = objectDictionary();
    for (const OdEntry &e : od)
        if (e.index == index && ((e.sub < 0 ? 0 : e.sub) == s)) return &e;
    return nullptr;
}

int sdoTypeFromOd(const QString &dataType)
{
    if (dataType == QLatin1String("INT8"))   return SDO_INT8;
    if (dataType == QLatin1String("INT16"))  return SDO_INT16;
    if (dataType == QLatin1String("INT32"))  return SDO_INT32;
    if (dataType == QLatin1String("UINT8"))  return SDO_UINT8;
    if (dataType == QLatin1String("UINT16")) return SDO_UINT16;
    if (dataType == QLatin1String("REAL32")) return SDO_FLOAT32;
    return SDO_UINT32;
}

int pdoMappingObjectIndex(Function f)
{
    switch (f) {
    case F_RPDO1: return 0x1600;
    case F_RPDO2: return 0x1601;
    case F_RPDO3: return 0x1602;
    case F_RPDO4: return 0x1603;
    case F_TPDO1: return 0x1A00;
    case F_TPDO2: return 0x1A01;
    case F_TPDO3: return 0x1A02;
    case F_TPDO4: return 0x1A03;
    default:      return -1;
    }
}

int pdoCommObjectIndex(Function f)
{
    switch (f) {
    case F_RPDO1: return 0x1400;
    case F_RPDO2: return 0x1401;
    case F_RPDO3: return 0x1402;
    case F_RPDO4: return 0x1403;
    case F_TPDO1: return 0x1800;
    case F_TPDO2: return 0x1801;
    case F_TPDO3: return 0x1802;
    case F_TPDO4: return 0x1803;
    default:      return -1;
    }
}

QVector<PdoMapEntry> pdoMapping(Function f)
{
    QVector<PdoMapEntry> out;
    const int mapObj = pdoMappingObjectIndex(f);
    if (mapObj < 0) return out;
    const QVector<OdEntry> od = objectDictionary();
    for (const OdEntry &e : od) {
        if (e.index != mapObj || e.sub < 1 || e.sub > 8) continue;
        bool ok = false;
        const quint32 v = e.dflt.toUInt(&ok);
        if (!ok || v == 0) continue;              // 未映射
        PdoMapEntry m;
        m.index = (quint16)((v >> 16) & 0xFFFF);
        m.sub   = (v >> 8) & 0xFF;
        m.bits  = v & 0xFF;
        out.append(m);
    }
    return out;
}

// ---- 离线自检 ----
bool selfTest(QString *report)
{
    QString log; bool ok = true;
    auto check = [&](bool cond, const QString &msg) {
        log += QStringLiteral("[%1] CANopen %2").arg(cond ? "PASS" : "FAIL", msg);
        log += QChar(10);
        ok = ok && cond;
    };

    // COB-ID（node=1）
    check(cobId(F_NMT, 1) == 0x000, QStringLiteral("NMT COB-ID == 0x000"));
    check(cobId(F_SYNC, 1) == 0x080, QStringLiteral("SYNC COB-ID == 0x080"));
    check(cobId(F_RPDO1, 1) == 0x201, QStringLiteral("RPDO1(node1) == 0x201"));
    check(cobId(F_TPDO1, 1) == 0x181, QStringLiteral("TPDO1(node1) == 0x181"));
    check(cobId(F_RPDO4, 1) == 0x501, QStringLiteral("RPDO4(node1) == 0x501"));
    check(cobId(F_RSDO, 1) == 0x601, QStringLiteral("RSDO(node1) == 0x601"));
    check(cobId(F_TSDO, 1) == 0x581, QStringLiteral("TSDO(node1) == 0x581"));
    check(cobId(F_HEARTBEAT, 1) == 0x701, QStringLiteral("Heartbeat(node1) == 0x701"));

    // NMT / 心跳
    check(buildNmt(1, 0x01) == QByteArray::fromHex("0101"), QStringLiteral("NMT operational → 01 01"));
    check(buildHeartbeat(1, 0x7F) == QByteArray::fromHex("7F"), QStringLiteral("Heartbeat 0x7F"));

    // SDO 写：6040h:00 = 6 (uint16) → 2B 40 60 00 06 00 00 00
    check(buildSdoWrite(1, 0x6040, 0, 6, SDO_UINT16) == QByteArray::fromHex("2B40600006000000"),
          QStringLiteral("SDO 写 6040h=6 → 2B 40 60 00 06 00 00 00"));
    // SDO 写：6060h:00 = 10 (int8) → 2F 60 60 00 0A 00 00 00
    check(buildSdoWrite(1, 0x6060, 0, 10, SDO_INT8) == QByteArray::fromHex("2F6060000A000000"),
          QStringLiteral("SDO 写 6060h=10 → 2F 60 60 00 0A 00 00 00"));
    // SDO 写：6081h:00 = 100 (uint32) → 23 81 60 00 64 00 00 00
    check(buildSdoWrite(1, 0x6081, 0, 100, SDO_UINT32) == QByteArray::fromHex("2381600064000000"),
          QStringLiteral("SDO 写 6081h=100 → 23 81 60 00 64 00 00 00"));
    // SDO 读：6040h:00 → 40 40 60 00 00 00 00 00
    check(buildSdoRead(1, 0x6040, 0) == QByteArray::fromHex("4040600000000000"),
          QStringLiteral("SDO 读 6040h → 40 40 60 00 00 00 00 00"));

    // 实战示例表
    {
        const QVector<Example> &ex = examples();
        check(ex.size() == 8, QStringLiteral("CANopen 实战示例 8 条"));
        bool velOk = false;
        for (const Example &e : ex) {
            if (e.ids.size() == 7 && e.ids.first() == 0x000 &&
                e.ids.last() == 0x401 &&
                e.payloads.first() == QByteArray::fromHex("0101")) velOk = true;
        }
        check(velOk, QStringLiteral("周期同步速度模式序列（NMT...RPDO3=0x401）"));
    }

    // 对象字典（EDS）
    {
        const QVector<OdEntry> od = objectDictionary();
        check(!od.isEmpty(), QStringLiteral("EDS 对象字典已解析（%1 条）").arg(od.size()));
        bool cw = false, target = false;
        for (const OdEntry &e : od) {
            if (e.index == 0x6040 && e.name.contains(QStringLiteral("Controlword"))) cw = true;
            if (e.index == 0x607A && e.name.contains(QStringLiteral("Target position"))) target = true;
        }
        check(cw, QStringLiteral("对象字典含 6040h Controlword"));
        check(target, QStringLiteral("对象字典含 607Ah Target position"));
    }

    // EDS 来源 + 对象编码
    check(edsText().contains(QStringLiteral("[6040]")), QStringLiteral("EDS 文本含 [6040]"));
    {
        const QVector<OdEntry> od = objectDictionary();
        const OdEntry *cw = nullptr;
        for (const OdEntry &e : od) if (e.index == 0x6040) cw = &e;
        check(cw != nullptr, QStringLiteral("对象字典含 6040h"));
        if (cw) {
            check(cw->dataType == QLatin1String("UINT16"), QStringLiteral("6040h 类型 UINT16"));
            check(encodeOdValue(*cw, 6) == QByteArray::fromHex("0600"),
                  QStringLiteral("encodeOdValue(6040h,6) -> 06 00 (UINT16 LE)"));
        }
        const OdEntry *pos = nullptr;
        for (const OdEntry &e : od) if (e.index == 0x607A) pos = &e;
        if (pos) check(encodeOdValue(*pos, 1) == QByteArray::fromHex("01000000"),
                       QStringLiteral("encodeOdValue(607Ah,1) -> 01 00 00 00 (INT32 LE)"));
    }

    // 单位换算（输出轴 → 电机 counts，转换因子 16384）
    {
        check(countsPerRev() == 16384.0, QStringLiteral("转换因子 608Fh:01 = 16384"));
        // 位置：1 转 gear1 → 16384；30° gear1 → 16384/12；90° gear2 → 16384/2
        check(std::llround(posCounts(1.0, 0, 1.0)) == 16384,
              QStringLiteral("posCounts 1 转 gear1 -> 16384"));
        check(std::llround(posCounts(30.0, 1, 1.0)) == 16384 / 12,
              QStringLiteral("posCounts 30° gear1 -> 1365"));
        check(std::llround(posCounts(90.0, 1, 2.0)) == 8192,
              QStringLiteral("posCounts 90° gear2 -> 8192"));
        // 速度：0.125 rev/s gear1 → 2048；3000 rpm gear1 → 50 rev/s → 819200
        check(std::llround(velCounts(0.125, 0, 1.0)) == 2048,
              QStringLiteral("velCounts 0.125 rev/s -> 2048"));
        check(std::llround(velCounts(3000.0, 2, 1.0)) == 819200,
              QStringLiteral("velCounts 3000 rpm -> 819200"));
        // 加速度：10 rev/s^2 gear1 → 163840
        check(std::llround(accelCounts(10.0, 0, 1.0)) == 163840,
              QStringLiteral("accelCounts 10 rev/s^2 -> 163840"));
        // 力矩：1 Nm gear1 → 100；2 Nm gear2 → 100
        check(std::llround(torqueCounts(1.0, 1.0)) == 100,
              QStringLiteral("torqueCounts 1Nm gear1 -> 100"));
        check(std::llround(torqueCounts(2.0, 2.0)) == 100,
              QStringLiteral("torqueCounts 2Nm gear2 -> 100"));
    }
    // 示例输入：默认值复现手册帧；自定义值生效
    {
        const QVector<Example> &ex = examples();
        const Example *vel = nullptr, *pos = nullptr;
        for (const Example &e : ex) {
            if (!e.inputs.isEmpty() && e.inputs.first().kind == CI_VEL &&
                e.ids.contains(0x401)) vel = &e;
            if (!e.inputs.isEmpty() && e.inputs.first().kind == CI_POS &&
                e.ids.size() == 7 && e.ids.last() == 0x301) pos = &e;
        }
        check(vel != nullptr && pos != nullptr, QStringLiteral("速度/位置示例已挂输入槽"));
        if (vel) {
            const QVector<Wire> w = buildExampleFrames(*vel, {0.125}, {0}, 1.0);
            check(w.size() == 7 && w.last().id == 0x401 &&
                  w.last().data == QByteArray::fromHex("0F0000080000"),
                  QStringLiteral("速度示例默认 0.125 rev/s -> 0F 00 00 08 00 00"));
            const QVector<Wire> w2 = buildExampleFrames(*vel, {1.0}, {0}, 1.0);
            // 1 rev/s → 16384 = 0x4000 → 00 00 00 40
            check(w2.last().data == QByteArray::fromHex("0F0000400000"),
                  QStringLiteral("速度示例改 1 rev/s -> 0F 00 00 00 40 00"));
        }
        if (pos) {
            const QVector<Wire> w = buildExampleFrames(*pos, {180.0}, {1}, 1.0);
            // 180° = 0.5 转 → 8192 = 0x2000 → 00 20 00 00
            check(w.last().data == QByteArray::fromHex("0F0000200000"),
                  QStringLiteral("位置示例 180° -> 0F 00 00 20 00 00"));
        }
    }

    // PDO 默认映射（手册）：RPDO4 → 6040 + 6071；RPDO2 → 6040 + 607A
    {
        const QVector<PdoMapEntry> r4 = pdoMapping(F_RPDO4);
        check(r4.size() == 2 && r4.at(0).index == 0x6040 && r4.at(1).index == 0x6071,
              QStringLiteral("RPDO4 默认映射 = 6040 + 6071（设力矩）"));
        const QVector<PdoMapEntry> r2 = pdoMapping(F_RPDO2);
        check(r2.size() == 2 && r2.at(1).index == 0x607A,
              QStringLiteral("RPDO2 默认映射 = 6040 + 607A"));
        check(pdoMappingObjectIndex(F_RPDO4) == 0x1603, QStringLiteral("RPDO4 映射对象 1603h"));
        check(pdoCommObjectIndex(F_RPDO4) == 0x1403, QStringLiteral("RPDO4 通信对象 1403h"));
        // findOd + 自动类型
        const OdEntry *m = findOd(0x6071, 0);
        check(m != nullptr && m->dataType == QLatin1String("INT16"),
              QStringLiteral("findOd(6071h) 类型 INT16"));
        check(sdoTypeFromOd(QStringLiteral("INT32")) == SDO_INT32 &&
              sdoTypeFromOd(QStringLiteral("REAL32")) == SDO_FLOAT32,
              QStringLiteral("sdoTypeFromOd 映射 INT32/REAL32"));
    }

    // PDO 映射修改序列（手册示例：TPDO1 改为 [6064h, 606Ch]）
    {
        QVector<PdoMapEntry> objs;
        objs.append({ 0x6064, 0, 32 });
        objs.append({ 0x606C, 0, 32 });
        const QVector<Wire> w = buildPdoMapChange(1, F_TPDO1, objs);
        const bool ok1 = w.size() == 6 &&
            w.at(0).id == 0x601 && w.at(0).data == QByteArray::fromHex("2300180181010080") &&
            w.at(1).data == QByteArray::fromHex("2F001A0000000000") &&
            w.at(2).data == QByteArray::fromHex("23001A0120006460") &&
            w.at(3).data == QByteArray::fromHex("23001A0220006C60") &&
            w.at(4).data == QByteArray::fromHex("2F001A0002000000") &&
            w.at(5).data == QByteArray::fromHex("2300180181010000");
        check(ok1, QStringLiteral("PDO 映射修改（TPDO1→6064/606C）6 帧匹配手册"));
        check(odBitLen(OdEntry{}) == 32, QStringLiteral("odBitLen 默认 32"));
    }

    if (report) *report = log; else Q_UNUSED(log);
    return ok;
}

} // namespace canopen
