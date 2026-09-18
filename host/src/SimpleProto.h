// ============================================================================
//  SimpleProto — ODrive "CAN Simple" 协议（手册 Simple 章节）
//
//  帧：11-bit 标准数据帧，8 字节数据，**小端**。
//      CAN ID = (node_id << 5) | cmd_id          （node_id 5bit 内）
//      float32 为 IEEE-754 小端；int16/uint32 小端。
//
//  本模块只做「命令表 + 编码」，供上位机 Simple 页构建数据帧；不涉及 UI。
// ============================================================================
#ifndef SIMPLEPROTO_H
#define SIMPLEPROTO_H

#include <QByteArray>
#include <QString>
#include <QVector>

namespace simple {

// 参数在 8 字节 payload 里的编码类型
enum ParamType {
    P_F32,   // float32 小端
    P_I16,   // int16 小端（带单位步长 scale，raw = round(value/scale)）
    P_U32,   // uint32 小端
    P_U8,    // uint8
    P_U16,   // uint16 小端
    P_RESV,  // 保留字节（写 0，UI 不显示）
};

// 参数的可选单位类别（决定 UI 是否出现单位下拉，以及换算到电机侧的路径）
//   位置/速度一律按「输出轴」理解；换算到电机侧时乘以用户减速比 gear。
//   MIT 的位置/速度按手册：线上已是「输出轴弧度/弧度每秒」，固件内部再换算。
enum UnitKind {
    U_NONE,     // 无单位选择（Nm / A / 增益 / 状态等）
    U_POS,      // 输出轴位置：转 / 角度 / 弧度  → 电机轴转数（×gear）
    U_VEL,      // 输出轴速度：转每秒 / 弧度每秒 / 转每分 → 电机轴转每秒（×gear）
    U_MIT_POS,  // MIT 位置：输出轴 → 输出轴弧度（不再 ×gear）
    U_MIT_VEL,  // MIT 速度：输出轴 → 输出轴弧度每秒
};

struct Param {
    QString  name;          // 名称（如 Input_Pos）
    ParamType type;         // 编码类型
    double   scale = 1.0;   // 单位步长（如 0.001rev/s → 0.001）
    QString  unit;          // 固定单位文字（Nm / A …；kind≠U_NONE 时由下拉决定）
    UnitKind kind = U_NONE; // 单位类别
    int      defaultUnit = 0; // 默认单位索引（见 positionUnitNames/velocityUnitNames）
};

struct Cmd {
    quint8  id;          // CMD ID
    QString name;        // 名称（Set_Input_Pos …）
    QString desc;        // 中文说明
    bool    hostToMotor; // true=主机->电机（可发送）
    QVector<Param> params;
};

// 全部命令（按 CMD ID 升序）
const QVector<Cmd> &commands();

// 一条线上帧（id + payload）
struct Wire { quint32 id; QByteArray data; };

// 11-bit CAN ID = (node_id<<5) | cmd_id
quint32 canId(int nodeId, quint8 cmdId);

// SDO 写值的类型（RxSdo 写模式下选择）
enum SdoType {
    SDO_INT8, SDO_UINT8, SDO_INT16, SDO_UINT16,
    SDO_INT24, SDO_UINT24, SDO_INT32, SDO_UINT32, SDO_FLOAT,
};
const QVector<QString> &sdoTypeNames();

// SDO 值 → 4 字节（小端；窄类型高位补 0，int 为二进制补码）
QByteArray sdoValueBytes(double v, int sdoType);

// RxSdo 帧：opcode(0读/1写) + Endpoint_ID(LE) + 保留 + Value(4B，仅写模式有效)
Wire buildRxSdo(int nodeId, int opcode, quint16 endpoint, double value, int sdoType);

// 位置/速度可选单位名（下拉显示）
const QVector<QString> &positionUnitNames();   // 转 / 角度(°) / 弧度(rad)
const QVector<QString> &velocityUnitNames();   // 转每秒 / 弧度每秒 / 转每分

// 输出轴值 + 所选单位 → 输出轴转数（位置）
double posToOutputTurns(double v, int unitIdx);
// 输出轴值 + 所选单位 → 输出轴转每秒（速度）
double velToOutputTurnsPerSec(double v, int unitIdx);

// 用户输入值 + 所选单位 → 写入线上的（电机侧）值
double toWire(const Param &p, double v, int unitIdx, double gear);

// MIT(0x08) 位打包（输出轴侧单位；使用手册 Simple 章节的默认上限）。
//   pos[rad], vel[rad/s], kp[Nm/rad], kd[Nm·s/rad], torque[Nm]
QByteArray encodeMit(double pos, double vel, double kp, double kd, double torque);

// 生成一条命令的 8 字节 payload（user 值按 Param 的单位/类型编码；
// unitIdx 为每个参数所选单位索引，缺省用 Param.defaultUnit）。
QByteArray encode(const Cmd &cmd, const QVector<double> &userValues, double gear,
                  const QVector<int> &unitIdx = QVector<int>());

// 组帧：Simple 线上帧 = 11-bit 标准数据帧，8 字节。返回可交给
// proto::buildDataFrame 的 (id, data)；此处直接返回 (id, 8B payload)。
Wire buildWire(int nodeId, const Cmd &cmd, const QVector<double> &userValues, double gear,
               const QVector<int> &unitIdx = QVector<int>());

// 示例固定帧中的可配置输入项（原始 uint32 小端，写入指定帧 payload 的偏移）
struct SInput {
    QString name;         // 显示名（如「输入模式」）
    int     frameIndex = 0;   // 固定帧序号（进入 cmdIds/payloads）
    int     offset = 0;       // payload 内偏移
    int     byteLen = 4;      // 字节数（1/2/4）
    quint32 defValue = 0;     // 默认值（复现手册示例）
};

// 手册「CAN 协议实战」示例：名称 + 固定前置帧 + 可选用户输入主命令
struct Example {
    QString name;                 // 显示名（如「位置控制」）
    QString desc;                 // 说明
    quint8  mainCmd = 0;          // 用户输入参数所用的命令 ID（0=全固定帧，无用户输入）
    QVector<quint8>     cmdIds;   // 固定前置帧的命令 ID
    QVector<QByteArray> payloads; // 固定前置帧的 8 字节 payload
    QVector<SInput>     inputs;   // 固定帧的可配置输入项（可为空）
};
const QVector<Example> &examples();

// 生成示例固定帧（含输入项覆盖 + 按节点注入 CAN ID）；主命令帧由调用方另加
QVector<Wire> buildExampleFixedFrames(const Example &e, const QVector<quint32> &values, int node);

// 离线自检（编码正确性）；报告写入 report，返回 true 表示全过
bool selfTest(QString *report);

} // namespace simple

#endif // SIMPLEPROTO_H
