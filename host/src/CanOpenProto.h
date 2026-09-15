// ============================================================================
//  CanOpenProto — CANopen（CiA301/302/402）调试帧构造 + 对象字典（EDS）
//
//  帧：11-bit 标准数据帧，8 字节数据，**小端**。
//      COB-ID = 功能码(4bit)<<7 | node_id      （预定义连接集）
//
//  本模块只做「帧构造 + EDS 解析」，供上位机 Open（CANopen）页使用；不涉及 UI。
//  对象字典来自内嵌资源 :/eds/ds402.eds（厂商 EDS 文件）。
// ============================================================================
#ifndef CANOPENPROTO_H
#define CANOPENPROTO_H

#include <QByteArray>
#include <QString>
#include <QVector>

namespace canopen {

// CANopen 预定义连接集功能（本驱动器默认映射，源自手册）
enum Function {
    F_NMT = 0,      // 0x000        网络管理（主站→从站）
    F_SYNC,         // 0x080        同步
    F_EMCY,         // 0x080+node   紧急报文（从站→主站，只读）
    F_TPDO1,        // 0x180+node
    F_RPDO1,        // 0x200+node
    F_TPDO2,        // 0x280+node
    F_RPDO2,        // 0x300+node
    F_TPDO3,        // 0x380+node
    F_RPDO3,        // 0x400+node
    F_TPDO4,        // 0x480+node
    F_RPDO4,        // 0x500+node
    F_TSDO,         // 0x580+node   服务器→客户端（SDO 应答，只读）
    F_RSDO,         // 0x600+node   客户端→服务器（SDO 请求）
    F_HEARTBEAT,    // 0x700+node   NMT 心跳
    F_COUNT
};

QVector<QString> functionNames();

// 功能码 + node → 11-bit COB-ID
quint32 cobId(Function f, int node);

// 帧体构造（返回 8 字节 payload，长度按需；COB-ID 由 cobId 给出）
quint32 nmtId();                                   // 0x000
QByteArray buildNmt(int node, quint8 cmd);         // [cmd, node]
quint32 syncId();                                  // 0x080
QByteArray buildSdoWrite(int node, quint16 index, quint8 sub, double value, int type);
QByteArray buildSdoRead(int node, quint16 index, quint8 sub);
QByteArray buildHeartbeat(int node, quint8 state); // [state]

// 一条线上帧（id + payload）
struct Wire { quint32 id; QByteArray data; };

// SDO 写数据类型（加速 SDO，<=4 字节）
enum SdoType {
    SDO_INT8, SDO_INT16, SDO_INT32,
    SDO_UINT8, SDO_UINT16, SDO_UINT32, SDO_FLOAT32,
};
QVector<QString> sdoTypeNames();
int sdoTypeBytes(int type);                        // 该类型字节数（1/2/4）

// 目标转矩/速度/位置等 RPDO 快捷帧：RPDO 数据 0..8 字节
Wire buildPdo(Function f, int node, const QByteArray &data);

// ---- 实战示例的用户可配置输入（位置/速度/力矩）----
//   一律以「输出轴」为单位；换算到电机侧 counts 时乘用户减速比 gear（力矩为除）。
//   位置/速度/加速度的换算因子 = EDS 608Fh:01（编码器增量/电机转，默认 16384）。
enum CoInputKind {
    CI_POS,      // 输出轴位置：转 / 角度(°) / 弧度(rad)
    CI_VEL,      // 输出轴速度：转每秒 / 弧度每秒 / 转每分
    CI_ACCEL,    // 输出轴加速度：转每二次方秒 / 弧度每二次方秒
    CI_TORQUE,   // 输出轴力矩：Nm（无单位下拉）
};

// 单位名（下拉显示）；CI_TORQUE 返回空
QVector<QString> coUnitNames(CoInputKind kind);

// 位置/速度的换算因子 counts/电机转（读 EDS 608Fh:01，缺失回退 16384）
double countsPerRev();

// 输出轴值 + 单位 + 减速比 → 写入 PDO/SDO 的 counts（小端将按 byteLen 截取）
double posCounts(double v, int unitIdx, double gear);    // 位置
double velCounts(double v, int unitIdx, double gear);    // 速度
double accelCounts(double v, int unitIdx, double gear);  // 加速度
double torqueCounts(double nm, double gear);             // 力矩 Nm → counts（÷gear ×100）

// 示例中的一个用户输入槽（定位到生成序列中某固定帧 payload 的数值段）
struct ExampleInput {
    QString     name;       // 显示名（如「目标位置」）
    CoInputKind kind = CI_POS;
    int         frameIndex = 0;   // 固定帧序号（进入 Example::ids/payloads）
    int         offset = 4;       // 该帧 payload 内数值起始偏移
    int         byteLen = 4;      // 数值字节数（1/2/4）
    double      defValue = 0.0;   // 默认值（复现手册示例）
    int         defUnit = 0;      // 默认单位索引
};

// 手册「CANopen 协议实战」示例：名称 + 逐帧固定序列（node_id=1）
struct Example {
    QString name;
    QString desc;
    QVector<quint32>     ids;       // 每帧 COB-ID
    QVector<QByteArray>  payloads;  // 每帧 payload（默认值模板）
    QVector<ExampleInput> inputs;   // 用户可配置输入（可为空）
};
const QVector<Example> &examples();

// 按用户输入值/单位/减速比，生成某示例的完整帧序列（含固定帧与重算后的数值）
QVector<Wire> buildExampleFrames(const Example &e, const QVector<double> &values,
                                 const QVector<int> &units, double gear);

// ---- 对象字典（EDS）----
struct OdEntry {
    quint16 index = 0;   // 对象索引（如 0x6040）
    int     sub   = -1;  // 子索引；-1 表示无子索引（单值对象）
    QString name;        // ParameterName
    QString access;      // ro / rw / ...
    QString dataType;    // UINT32 / REAL32 / ...
    QString dflt;        // DefaultValue
    bool    mappable = false; // EDS 中 PDOMapping=1（可映射到 PDO）
};

// 对象显示文本（下拉用），如 "0x6071  Target Torque (INT16)"
QString odDisplay(const OdEntry &e);
// 按对象数据类型编码数值为小端字节（1/2/4 字节）
QByteArray encodeOdValue(const OdEntry &e, double value);
// 在对象字典中查找 (index, sub)；sub<0 视为 0。找不到返回 nullptr。
const OdEntry *findOd(quint16 index, int sub);
// EDS 数据类型（如 INT16/UINT32/REAL32）→ SDO 写类型（canopen::SdoType）
int sdoTypeFromOd(const QString &dataType);

// PDO 映射/通信参数对象基址（手册）：RPDO1-4 → 映射 1600-1603 / 通信 1400-1403；
// TPDO1-4 → 映射 1A00-1A03 / 通信 1800-1803。非 PDO 返回 -1。
int pdoMappingObjectIndex(Function f);
int pdoCommObjectIndex(Function f);

// EDS 默认 PDO 映射的一条：目标对象 (index, sub) 及其位长
struct PdoMapEntry { quint16 index = 0; int sub = 0; int bits = 0; };
// 读取 EDS 中该 PDO 的默认映射（映射对象索引 1600-1603/1A00-1A03 的子索引 1..8）
QVector<PdoMapEntry> pdoMapping(Function f);

// 映射对象位长（由数据类型推导：8/16/32）
int odBitLen(const OdEntry &e);

// 生成「修改 PDO 映射」的 SDO 序列（预操作态下）：
//   无效该 PDO(COB-ID 最高位=1) → 清空映射数(子索引0=0) → 逐条写入映射(子索引1..N)
//   → 写入映射数(子索引0=N) → 有效该 PDO(COB-ID 最高位=0)
QVector<Wire> buildPdoMapChange(int node, Function pdo, const QVector<PdoMapEntry> &objects);

QVector<OdEntry> objectDictionary();   // 解析当前 EDS（首解析后缓存）
QString edsText();                     // 当前 EDS 原始文本（空=读取失败）
QString edsSourceName();               // 当前 EDS 来源描述（内置 / 外部文件路径）

// 外部 EDS：运行时优先使用 <exe目录>/eds/ds402.eds（若存在），否则内嵌资源。
QString externalEdsPath();             // 外部 EDS 文件路径
bool importEdsFile(const QString &srcPath, QString *err);  // 复制到外部路径并重载
void resetToEmbeddedEds();             // 删除外部文件并回退到内嵌
void reloadEds();                      // 清缓存并重读

// 离线自检；报告写入 report，返回 true 表示全过
bool selfTest(QString *report);

} // namespace canopen

#endif // CANOPENPROTO_H
