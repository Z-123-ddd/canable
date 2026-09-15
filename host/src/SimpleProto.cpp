#include "SimpleProto.h"

#include <cmath>
#include <cstring>

namespace simple {

// ---- 小端写助手 ----
static void putF32(QByteArray &b, int off, double v)
{
    float f = (float)v;
    quint32 u; memcpy(&u, &f, 4);
    b[off + 0] = char( u        & 0xFF);
    b[off + 1] = char((u >>  8) & 0xFF);
    b[off + 2] = char((u >> 16) & 0xFF);
    b[off + 3] = char((u >> 24) & 0xFF);
}
static void putI16(QByteArray &b, int off, int v)
{
    quint16 u = (quint16)(qint16)v;
    b[off + 0] = char( u       & 0xFF);
    b[off + 1] = char((u >> 8) & 0xFF);
}
static void putU16(QByteArray &b, int off, quint16 v)
{
    b[off + 0] = char( v       & 0xFF);
    b[off + 1] = char((v >> 8) & 0xFF);
}
static void putU32(QByteArray &b, int off, quint32 u)
{
    b[off + 0] = char( u        & 0xFF);
    b[off + 1] = char((u >>  8) & 0xFF);
    b[off + 2] = char((u >> 16) & 0xFF);
    b[off + 3] = char((u >> 24) & 0xFF);
}
static int clampInt(double v, int lo, int hi)
{
    double r = std::round(v);
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    return (int)r;
}

// ---- 命令表（手册 Simple 章节，CMD ID 0x01–0x1F）----
static QVector<Cmd> g_cmds;
static void init()
{
    if (!g_cmds.isEmpty()) return;
    auto add = [](quint8 id, const char *name, const char *desc, bool h2m,
                  std::initializer_list<Param> ps) {
        Cmd c; c.id = id; c.name = QString::fromUtf8(name);
        c.desc = QString::fromUtf8(desc); c.hostToMotor = h2m;
        for (const auto &p : ps) c.params.append(p);
        g_cmds.append(c);
    };
    auto F  = [](const char *n, const char *u){ return Param{QString::fromUtf8(n), P_F32, 1.0, QString::fromUtf8(u), U_NONE, 0}; };
    auto Fk = [](const char *n, UnitKind k, int def){ return Param{QString::fromUtf8(n), P_F32, 1.0, QString(), k, def}; };
    auto I16= [](const char *n, double s, const char *u){ return Param{QString::fromUtf8(n), P_I16, s, QString::fromUtf8(u), U_NONE, 0}; };
    auto I16k=[&](const char *n, double s, UnitKind k, int def){ return Param{QString::fromUtf8(n), P_I16, s, QString(), k, def}; };
    auto U32= [](const char *n, const char *u){ return Param{QString::fromUtf8(n), P_U32, 1.0, QString::fromUtf8(u), U_NONE, 0}; };
    auto U8 = [](const char *n, const char *u){ return Param{QString::fromUtf8(n), P_U8, 1.0, QString::fromUtf8(u), U_NONE, 0}; };
    auto U16= [](const char *n, const char *u){ return Param{QString::fromUtf8(n), P_U16, 1.0, QString::fromUtf8(u), U_NONE, 0}; };
    auto RESV= [](){ return Param{QStringLiteral("(reserved)"), P_RESV, 1.0, QString(), U_NONE, 0}; };

    add(0x01, "Heartbeat", "心跳（电机→主机，可空发请求）", false, {});
    add(0x02, "Estop", "紧急停止（主机→电机）", true, {});
    add(0x03, "Get_Error", "获取详细错误（电机→主机）", false, { U8("Error_Type","0/1/3/4") });
    add(0x04, "RxSdo", "读写参数(SDO)：opcode(0读/1写) + Endpoint_ID + 写值(类型可选)", true, {
        U8("opcode","0读/1写"), U16("Endpoint_ID","id(LE)") });
    add(0x05, "TxSdo", "SDO 读取回复（电机→主机，可空发请求）", false, {
        U8("opcode","固定0"), U16("Endpoint_ID","id(LE)"), RESV(), F("Value","值") });
    add(0x06, "Set_Axis_Node_ID", "设置节点 ID", true, { U32("Axis_Node_ID","id") });
    add(0x07, "Set_Axis_State", "设置请求状态 (1空闲/3校准/8闭环)", true, { U32("Axis_Requested_State","state") });
    add(0x08, "Mit_Control", "MIT 力位混合控制（输出轴侧单位）", true, {
        Fk("Position", U_MIT_POS, 2), Fk("Velocity", U_MIT_VEL, 1), F("Kp","Nm/rad"),
        F("Kd","Nm·s/rad"), F("Torque","Nm") });
    add(0x09, "Get_Encoder_Estimates", "获取编码器估算 (电机→主机)", false, {});
    add(0x0A, "Get_Encoder_Count", "获取编码器计数 (电机→主机)", false, {});
    add(0x0B, "Set_Controller_Mode", "设置控制/输入模式", true, {
        U32("Control_Mode","0电压/1力矩/2速度/3位置"), U32("Input_Mode","0闲置/1直接/2速度斜坡/3位置滤波/5梯形/6力矩斜坡/9MIT") });
    add(0x0C, "Set_Input_Pos", "设定输入位置", true, {
        Fk("Input_Pos", U_POS, 1), I16k("Vel_FF",0.001,U_VEL,0), I16("Torque_FF",0.001,"Nm") });
    add(0x0D, "Set_Input_Vel", "设定输入速度", true, { Fk("Input_Vel", U_VEL, 0), F("Torque_FF","Nm") });
    add(0x0E, "Set_Input_Torque", "设定输入扭矩", true, { F("Input_Torque","Nm") });
    add(0x0F, "Set_Limits", "设定速度与电流极限", true, { Fk("Velocity_Limit", U_VEL, 0), F("Current_Limit","A") });
    add(0x10, "Start_Anticogging", "启动纹波校准", true, {});
    add(0x11, "Set_Traj_Vel_Limit", "梯形轨迹速度限值", true, { Fk("Traj_Vel_Limit", U_VEL, 0) });
    add(0x12, "Set_Traj_Accel_Limits", "梯形轨迹加减速限值", true, {
        F("Traj_Accel_Limit","rev/s^2"), F("Traj_Decel_Limit","rev/s^2") });
    add(0x13, "Set_Traj_Inertia", "梯形轨迹惯量", true, { F("Traj_Inertia","Nm/(rev/s^2)") });
    add(0x14, "Get_Iq", "获取 Iq (电机→主机)", false, {});
    add(0x15, "Get_Thermistor_Temperature", "获取温度 (电机→主机)", false, {});
    add(0x16, "Reboot", "重启电机", true, {});
    add(0x17, "Get_Bus_Voltage_Current", "获取母线电压/电流 (电机→主机)", false, {});
    add(0x18, "Clear_Errors", "清除错误", true, {});
    add(0x19, "Set_Move_Incremental", "增量移动（相对当前位置）", true, { Fk("Displacement", U_POS, 1) });
    add(0x1A, "Set_Pos_Gain", "设置位置环 Kp", true, { F("Pos_Gain","(rev/s)/rev") });
    add(0x1B, "Set_Vel_Gains", "设置速度环 Kp/Ki", true, { F("Vel_Gain","Nm/(rev/s)"), F("Vel_Integrator_Gain","Nm/rev") });
    add(0x1C, "Get_Torques", "获取力矩 (电机→主机)", false, {});
    add(0x1D, "Get_Powers", "获取功率 (电机→主机)", false, {});
    add(0x1E, "Disable_Can", "禁用 CAN 并重启", true, {});
    add(0x1F, "Save_Configuration", "保存配置并重启", true, {});
}

const QVector<Cmd> &commands()
{
    init();
    return g_cmds;
}

quint32 canId(int nodeId, quint8 cmdId)
{
    return ((quint32)(nodeId & 0x1F) << 5) | (cmdId & 0x1F);
}

static const double kTwoPi = 6.283185307179586476925286766559;

const QVector<QString> &positionUnitNames()
{
    static const QVector<QString> n = {
        QStringLiteral("转"), QStringLiteral("角度(°)"), QStringLiteral("弧度(rad)") };
    return n;
}
const QVector<QString> &velocityUnitNames()
{
    static const QVector<QString> n = {
        QStringLiteral("转每秒(rev/s)"), QStringLiteral("弧度每秒(rad/s)"), QStringLiteral("转每分(rpm)") };
    return n;
}

// 输出轴值 → 输出轴转数（位置）
double posToOutputTurns(double v, int unitIdx)
{
    switch (unitIdx) {
    case 1: return v / 360.0;    // 角度 → 转
    case 2: return v / kTwoPi;   // 弧度 → 转
    case 0:
    default: return v;           // 转
    }
}

// 输出轴值 → 输出轴转每秒（速度）
double velToOutputTurnsPerSec(double v, int unitIdx)
{
    switch (unitIdx) {
    case 1: return v / kTwoPi;   // 弧度每秒 → 转每秒
    case 2: return v / 60.0;     // 转每分 → 转每秒
    case 0:
    default: return v;           // 转每秒
    }
}

// 用户输入值 + 所选单位 → 写入线上的（电机侧）值
double toWire(const Param &p, double v, int unitIdx, double gear)
{
    switch (p.kind) {
    case U_POS:     return posToOutputTurns(v, unitIdx) * gear;        // 输出转 → 电机转
    case U_VEL:     return velToOutputTurnsPerSec(v, unitIdx) * gear;  // 输出转/s → 电机转/s
    case U_MIT_POS: return posToOutputTurns(v, unitIdx) * kTwoPi;      // → 输出弧度
    case U_MIT_VEL: return velToOutputTurnsPerSec(v, unitIdx) * kTwoPi;// → 输出弧度/s
    case U_NONE:
    default:        return v;
    }
}

// ---- MIT 位打包（手册 Simple 4.x：mit_max_pos=12.5, vel=65, kp=500, kd=5, torque=50）----
static int packBits(double v, double vmin, double vmax, int bits)
{
    double span = (vmax - vmin);
    double n = (span == 0.0) ? 0.0 : ((v - vmin) / span);
    double maxi = (double)((1u << bits) - 1u);
    return clampInt(n * maxi, 0, (int)maxi);
}

QByteArray encodeMit(double pos, double vel, double kp, double kd, double torque)
{
    const double MAX_POS = 12.5, MAX_VEL = 65.0, MAX_KP = 500.0, MAX_KD = 5.0, MAX_T = 50.0;
    int p  = packBits(pos,    -MAX_POS, MAX_POS, 16);
    int ve = packBits(vel,    -MAX_VEL, MAX_VEL, 12);
    int kpp= packBits(kp,      0.0,     MAX_KP,  12);
    int kdd= packBits(kd,      0.0,     MAX_KD,  12);
    int t  = packBits(torque, -MAX_T,   MAX_T,   12);

    QByteArray b(8, '\0');
    b[0] = char((p >> 8) & 0xFF);
    b[1] = char(p & 0xFF);
    b[2] = char((ve >> 4) & 0xFF);
    b[3] = char(((ve & 0x0F) << 4) | ((kpp >> 8) & 0x0F));
    b[4] = char(kpp & 0xFF);
    b[5] = char((kdd >> 4) & 0xFF);
    b[6] = char(((kdd & 0x0F) << 4) | ((t >> 8) & 0x0F));
    b[7] = char(t & 0xFF);
    return b;
}

QByteArray encode(const Cmd &cmd, const QVector<double> &vals, double gear,
                  const QVector<int> &unitIdx)
{
    auto unitAt = [&](int i) {
        return (i < unitIdx.size()) ? unitIdx.at(i) : cmd.params.at(i).defaultUnit;
    };

    // MIT 走专用位打包（位置/速度先折算到输出轴弧度）
    if (cmd.id == 0x08) {
        auto at = [&](int i) { return (i < vals.size()) ? vals.at(i) : 0.0; };
        auto wr = [&](int i) { return toWire(cmd.params.at(i), at(i), unitAt(i), gear); };
        return encodeMit(wr(0), wr(1), wr(2), wr(3), wr(4));
    }

    QByteArray b(8, '\0');
    int off = 0;
    for (int i = 0; i < cmd.params.size(); ++i) {
        const Param &p = cmd.params.at(i);
        double v = toWire(p, (i < vals.size()) ? vals.at(i) : 0.0, unitAt(i), gear);
        switch (p.type) {
        case P_F32: putF32(b, off, v); off += 4; break;
        case P_I16: putI16(b, off, p.scale != 0.0 ? (int)std::lround(v / p.scale) : (int)v); off += 2; break;
        case P_U32: putU32(b, off, (quint32)std::lround(v)); off += 4; break;
        case P_U8:  if (off < 8) b[off] = char((int)std::lround(v) & 0xFF); off += 1; break;
        case P_U16: putU16(b, off, (quint16)std::lround(v)); off += 2; break;
        case P_RESV: if (off < 8) b[off] = '\0'; off += 1; break;
        }
        if (off >= 8) break;
    }
    return b;
}

const QVector<QString> &sdoTypeNames()
{
    static const QVector<QString> n = {
        QStringLiteral("int8"), QStringLiteral("uint8"),
        QStringLiteral("int16"), QStringLiteral("uint16"),
        QStringLiteral("int24"), QStringLiteral("uint24"),
        QStringLiteral("int32"), QStringLiteral("uint32"),
        QStringLiteral("float"),
    };
    return n;
}

QByteArray sdoValueBytes(double v, int sdoType)
{
    QByteArray b(4, '\0');   // 小端，窄类型高位补 0
    switch (sdoType) {
    case SDO_INT8:   { qint8  x = (qint8)std::lround(v);  b[0] = char(x & 0xFF); break; }
    case SDO_UINT8:  { quint8 x = (quint8)std::llround(v); b[0] = char(x & 0xFF); break; }
    case SDO_INT16:  { qint16 x = (qint16)std::lround(v); putI16(b, 0, x); break; }
    case SDO_UINT16: { quint16 x = (quint16)std::llround(v); putU16(b, 0, x); break; }
    case SDO_INT24:  { qint32 x = (qint32)std::lround(v); b[0]=char(x&0xFF); b[1]=char((x>>8)&0xFF); b[2]=char((x>>16)&0xFF); break; }
    case SDO_UINT24: { quint32 x = (quint32)std::llround(v); b[0]=char(x&0xFF); b[1]=char((x>>8)&0xFF); b[2]=char((x>>16)&0xFF); break; }
    case SDO_INT32:
    case SDO_UINT32: { putU32(b, 0, (quint32)std::llround(v)); break; }
    case SDO_FLOAT:
    default:         { putF32(b, 0, v); break; }
    }
    return b;
}

Wire buildRxSdo(int nodeId, int opcode, quint16 endpoint, double value, int sdoType)
{
    QByteArray b(8, '\0');
    b[0] = char(opcode & 0xFF);
    putU16(b, 1, endpoint);
    b[3] = '\0';   // 保留
    // Value 仅写模式(opcode=1)生效；读模式填 0
    QByteArray vb = (opcode == 1) ? sdoValueBytes(value, sdoType) : QByteArray(4, '\0');
    for (int i = 0; i < 4; ++i) b[4 + i] = vb.at(i);
    Wire w; w.id = canId(nodeId, 0x04); w.data = b;
    return w;
}

Wire buildWire(int nodeId, const Cmd &cmd, const QVector<double> &userValues, double gear,
               const QVector<int> &unitIdx)
{
    Wire w;
    w.id = canId(nodeId, cmd.id);
    w.data = encode(cmd, userValues, gear, unitIdx);
    return w;
}

// ---- 手册「CAN 协议实战」示例（node_id=1；payload 为 8 字节固定帧）----
static QVector<Example> g_examples;
static void initExamples()
{
    if (!g_examples.isEmpty()) return;
    auto hex = [](const char *s) { return QByteArray::fromHex(s); };

    // 上电校准：Set_Axis_State(0x07)=4 电机校准 → =7 编码器校准
    {
        Example e;
        e.name = QStringLiteral("上电校准（实战）");
        e.desc = QStringLiteral("Set_Axis_State=4 电机校准，再 Set_Axis_State=7 编码器校准");
        e.mainCmd = 0;
        e.cmdIds = { 0x07, 0x07 };
        e.payloads = { hex("0400000000000000"), hex("0700000000000000") };
        g_examples.append(e);
    }
    // 速度控制：Set_Controller_Mode(0x0B)=2/2 → Set_Axis_State(0x07)=8 → Set_Input_Vel(0x0D) 由用户输入
    {
        Example e;
        e.name = QStringLiteral("速度控制（实战）");
        e.desc = QStringLiteral("Set_Controller_Mode=2/2（速度/速度斜坡）→ Set_Axis_State=8 闭环 → Set_Input_Vel 由用户输入");
        e.mainCmd = 0x0D;
        e.cmdIds = { 0x0B, 0x07 };
        e.payloads = { hex("0200000002000000"), hex("0800000000000000") };
        g_examples.append(e);
    }
    // 位置控制：Set_Controller_Mode(0x0B)=3/3 → Set_Axis_State(0x07)=8 → Set_Input_Pos(0x0C) 由用户输入
    {
        Example e;
        e.name = QStringLiteral("位置控制（实战）");
        e.desc = QStringLiteral("Set_Controller_Mode=3/3（位置/位置滤波）→ Set_Axis_State=8 闭环 → Set_Input_Pos 由用户输入");
        e.mainCmd = 0x0C;
        e.cmdIds = { 0x0B, 0x07 };
        e.payloads = { hex("0300000003000000"), hex("0800000000000000") };
        g_examples.append(e);
    }
    // RxSdo 实战：读母线电压 → 写减速比 9.25 → 读减速比
    {
        Example e;
        e.name = QStringLiteral("RxSdo 读写（实战）");
        e.desc = QStringLiteral("读母线电压(0x0002) → 写减速比 9.25(0x00F2) → 读减速比");
        e.mainCmd = 0;
        e.cmdIds = { 0x04, 0x04, 0x04 };
        e.payloads = { hex("0002000000000000"), hex("01F2000000001441"), hex("00F2000000000000") };
        g_examples.append(e);
    }
}

const QVector<Example> &examples()
{
    initExamples();
    return g_examples;
}

// ---- 离线自检 ----
static const Cmd *find(quint8 id)
{
    for (const Cmd &c : commands()) if (c.id == id) return &c;
    return nullptr;
}

bool selfTest(QString *report)
{
    QString log; bool ok = true;
    auto check = [&](bool cond, const QString &msg) {
        log += QStringLiteral("[%1] Simple %2\n").arg(cond ? "PASS" : "FAIL", msg);
        ok = ok && cond;
    };

    // CAN ID = (node<<5)|cmd：node=1, cmd=0x0C → 0x2C
    check(canId(1, 0x0C) == 0x2C, QStringLiteral("CAN ID (node=1,cmd=0x0C) == 0x2C"));

    // 位置单位换算（到输出轴转数）
    check(std::fabs(posToOutputTurns(360.0, 1) - 1.0) < 1e-9, QStringLiteral("360° → 1 转"));
    check(std::fabs(posToOutputTurns(90.0, 1) - 0.25) < 1e-9, QStringLiteral("90° → 0.25 转"));
    check(std::fabs(posToOutputTurns(kTwoPi, 2) - 1.0) < 1e-9, QStringLiteral("2π rad → 1 转"));
    check(std::fabs(posToOutputTurns(2.0, 0) - 2.0) < 1e-9, QStringLiteral("2 转 → 2 转"));
    // 速度单位换算（到输出轴转每秒）
    check(std::fabs(velToOutputTurnsPerSec(3000.0, 2) - 50.0) < 1e-9, QStringLiteral("3000 rpm → 50 rev/s"));
    check(std::fabs(velToOutputTurnsPerSec(kTwoPi, 1) - 1.0) < 1e-9, QStringLiteral("2π rad/s → 1 rev/s"));
    check(std::fabs(velToOutputTurnsPerSec(3.0, 0) - 3.0) < 1e-9, QStringLiteral("3 rev/s → 3 rev/s"));
    // toWire：输出轴值 ×gear → 电机侧（MIT 不加 gear）
    if (const Cmd *c = find(0x0C)) {
        check(std::fabs(toWire(c->params.at(0), 30.0, 0 /*转*/, 2.0) - 60.0) < 1e-9,
              QStringLiteral("toWire U_POS 30 转 ×gear2 → 60 电机转"));
    }
    if (const Cmd *c = find(0x08)) {
        check(std::fabs(toWire(c->params.at(0), 1.0, 2 /*rad*/, 2.0) - 1.0) < 1e-9,
              QStringLiteral("toWire U_MIT_POS 1 rad → 1 输出弧度（不乘 gear）"));
    }

    // Set_Input_Pos: Input_Pos=30°, Vel_FF=1rev/s, Torque_FF=5Nm, gear=1
    if (const Cmd *c = find(0x0C)) {
        Wire w = buildWire(1, *c, {30.0, 1.0, 5.0}, 1.0);
        // Input_Pos = 1/12 = 0x3DAAAAAB LE = AB AA AA 3D
        // Vel_FF raw=1000=0x03E8 LE = E8 03 ; Torque_FF raw=5000=0x1388 LE = 88 13
        const QByteArray expect = QByteArray::fromHex("ABAAAA3DE8038813");
        check(w.id == 0x2C, QStringLiteral("Set_Input_Pos CAN ID == 0x2C"));
        check(w.data == expect,
              QStringLiteral("Set_Input_Pos 8B = AB AA AA 3D E8 03 88 13 (got %1)")
                  .arg(QString::fromLatin1(w.data.toHex(' ').toUpper())));
    } else { check(false, QStringLiteral("命令 0x0C 存在")); }

    // Set_Input_Vel: 10 rev/s, 0 Nm → 0x41200000 LE = 00 00 20 41
    if (const Cmd *c = find(0x0D)) {
        Wire w = buildWire(1, *c, {10.0, 0.0}, 1.0);
        const QByteArray expect = QByteArray::fromHex("0000204100000000");
        check(w.data == expect, QStringLiteral("Set_Input_Vel 10/0 → 00 00 20 41 00 00 00 00"));
        // 单位：3000 rpm → 50 rev/s → 0x42480000 LE
        Wire wr = buildWire(1, *c, {3000.0, 0.0}, 1.0, {2, 0});
        check(wr.data == QByteArray::fromHex("0000484200000000"),
              QStringLiteral("Set_Input_Vel 3000 rpm → 50 rev/s (00 00 48 42)"));
    }

    // Set_Input_Pos 单位「转」+ gear2：30 转 → 60 电机转 → 0x42700000 LE
    if (const Cmd *c = find(0x0C)) {
        Wire w = buildWire(1, *c, {30.0, 0.0, 0.0}, 2.0, {0, 0, 0});
        check(w.data == QByteArray::fromHex("0000704200000000"),
              QStringLiteral("Set_Input_Pos 30 转 ×gear2 → 60 电机转 (00 00 70 42)"));
    }

    // RxSdo 写 gear_ratio=9.25（float）：opcode=1, endpoint=0x00F2
    check(find(0x04) != nullptr, QStringLiteral("命令 0x04 RxSdo 存在"));
    {
        Wire w = buildRxSdo(1, 1, 0x00F2, 9.25, SDO_FLOAT);
        check(w.id == ((1u << 5) | 0x04), QStringLiteral("RxSdo CAN ID == 0x24"));
        const QByteArray expect = QByteArray::fromHex("01F2000000001441");
        check(w.data == expect,
              QStringLiteral("RxSdo 写 float 9.25 → 01 F2 00 00 00 00 14 41 (got %1)")
                  .arg(QString::fromLatin1(w.data.toHex(' ').toUpper())));
    }
    // RxSdo 读（opcode=0）：Value 不生效（全 0）
    {
        Wire w = buildRxSdo(1, 0, 0x0002, 123.0, SDO_FLOAT);
        const QByteArray expect = QByteArray::fromHex("0002000000000000");
        check(w.data == expect, QStringLiteral("RxSdo 读 → 00 02 00 00 00 00 00 00 (Value 忽略)"));
    }
    // RxSdo 写 int16：value=1000 → E8 03 00 00（小端，高位补 0）
    {
        Wire w = buildRxSdo(1, 1, 0x0010, 1000.0, SDO_INT16);
        check(w.data == QByteArray::fromHex("01100000E8030000"),
              QStringLiteral("RxSdo 写 int16 1000 → 01 10 00 00 E8 03 00 00"));
    }

    // 实战示例表：位置控制/速度控制的主命令与固定前置帧
    {
        const QVector<Example> &ex = examples();
        check(ex.size() == 4, QStringLiteral("实战示例 4 条（上电校准/速度/位置/RxSdo）"));
        bool posOk = false, velOk = false;
        for (const Example &e : ex) {
            if (e.mainCmd == 0x0C && e.cmdIds.size() == 2 &&
                e.payloads.at(0) == QByteArray::fromHex("0300000003000000") &&
                e.payloads.at(1) == QByteArray::fromHex("0800000000000000")) posOk = true;
            if (e.mainCmd == 0x0D && e.cmdIds.size() == 2 &&
                e.payloads.at(0) == QByteArray::fromHex("0200000002000000")) velOk = true;
        }
        check(posOk, QStringLiteral("位置控制示例：0x0B=3/3 + 0x07=8 + 用户输入 0x0C"));
        check(velOk, QStringLiteral("速度控制示例：0x0B=2/2 + 0x07=8 + 用户输入 0x0D"));
    }

    if (report) *report = log; else Q_UNUSED(log);
    return ok;
}

} // namespace simple
