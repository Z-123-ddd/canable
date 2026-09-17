// ============================================================================
//  FrameParse.cpp — 见 FrameParse.h
//
//  解析规则严格按《守护兽驱动协议手册》：
//    · CyberBeast 协议  — 29-bit ID: Pri[28:26] MsgType[25:18] Dest[17:10] SA[9:2] Seq[1:0]
//                         payload Big-Endian（多字节），MIT 为 bit-packed。
//    · CAN Simple 协议  — 11-bit ID: node_id[10:5] cmd_id[4:0]，payload Little-Endian（IEEE754）。
//    · CANopen 协议     — COB-ID 功能段：NMT 0x000 / SYNC 0x080 / EMCY 0x080+n /
//                         TPDO 0x180/0x280/0x380/0x480+n / RPDO 0x200/0x300/0x400/0x500+n /
//                         SDO 0x580+n(响应) 0x600+n(请求) / 心跳 0x700+n。
//    · RS485 协议       — Modbus RTU：addr | func | data | CRC16(LE)。
// ============================================================================
#include "FrameParse.h"
#include <QStringList>
#include <QtGlobal>
#include <cmath>
#include <cstring>

namespace {

// ---- 大端 / 小端 读取（越界返回 0）----
inline bool have(const QByteArray &d, int off, int n) { return off >= 0 && off + n <= d.size(); }

inline quint16 u16be(const QByteArray &d, int o) {
    if (!have(d, o, 2)) return 0;
    return (quint16)(((quint8)d[o] << 8) | (quint8)d[o + 1]);
}
inline qint16 i16be(const QByteArray &d, int o) { return (qint16)u16be(d, o); }
inline quint32 u32be(const QByteArray &d, int o) {
    if (!have(d, o, 4)) return 0;
    return ((quint32)(quint8)d[o] << 24) | ((quint32)(quint8)d[o + 1] << 16) |
           ((quint32)(quint8)d[o + 2] << 8) | (quint32)(quint8)d[o + 3];
}
inline qint32 i32be(const QByteArray &d, int o) { return (qint32)u32be(d, o); }
inline float f32be(const QByteArray &d, int o) {
    quint32 v = u32be(d, o);
    float f; std::memcpy(&f, &v, 4); return f;
}
inline quint16 u16le(const QByteArray &d, int o) {
    if (!have(d, o, 2)) return 0;
    return (quint16)((quint8)d[o] | ((quint8)d[o + 1] << 8));
}
inline qint16 i16le(const QByteArray &d, int o) { return (qint16)u16le(d, o); }
inline quint32 u32le(const QByteArray &d, int o) {
    if (!have(d, o, 4)) return 0;
    return (quint32)(quint8)d[o] | ((quint32)(quint8)d[o + 1] << 8) |
           ((quint32)(quint8)d[o + 2] << 16) | ((quint32)(quint8)d[o + 3] << 24);
}
inline qint32 i32le(const QByteArray &d, int o) { return (qint32)u32le(d, o); }
inline float f32le(const QByteArray &d, int o) {
    quint32 v = u32le(d, o);
    float f; std::memcpy(&f, &v, 4); return f;
}
inline quint64 u64be(const QByteArray &d, int o) {
    return ((quint64)u32be(d, o) << 32) | (quint64)u32be(d, o + 4);
}

inline QString num(double v, int prec = 3) {
    if (std::isnan(v) || std::isinf(v)) return QStringLiteral("nan");
    return QString::number(v, 'g', prec);
}
inline QString hexb(const QByteArray &d) { return QString::fromLatin1(d.toHex(' ').toUpper()); }

const char *directionText(bool ext) { return ext ? "扩展" : "标准"; }

// ===========================================================================
//  CyberBeast
// ===========================================================================
QString cbPriorityName(int p) {
    static const char *n[] = {"CRITICAL", "EMERGENCY", "HIGH_CTRL", "CTRL",
                              "CONFIG", "QUERY", "STATUS", "LOW"};
    return QString::fromLatin1(n[p & 7]);
}
QString cbMsgName(int m) {
    switch (m) {
    case 0x00: return "MIT_CONTROL";
    case 0x01: return "POS_CONTROL";
    case 0x02: return "VEL_CONTROL";
    case 0x03: return "TORQUE_CONTROL";
    case 0x04: return "CURRENT_CONTROL";
    case 0x20: return "PARAM_READ";
    case 0x21: return "PARAM_WRITE";
    case 0x22: return "CONFIG_SAVE";
    case 0x23: return "CONFIG_RESET";
    case 0x24: return "JSON_DESC_READ";
    case 0x25: return "JSON_DESC_DATA";
    case 0x40: return "QUERY_STATUS";
    case 0x41: return "QUERY_POS_VEL";
    case 0x42: return "QUERY_CURRENT";
    case 0x43: return "QUERY_TEMPERATURE";
    case 0x44: return "QUERY_BUS";
    case 0x45: return "QUERY_ERROR";
    case 0x46: return "QUERY_DEVICE_INFO";
    case 0x47: return "QUERY_POWER";
    case 0x48: return "HEARTBEAT";
    case 0x49: return "STATUS_FEEDBACK";
    case 0x60: return "SET_NODE_ID";
    case 0x61: return "SET_ZERO";
    case 0x62: return "START_MOTOR";
    case 0x63: return "STOP_MOTOR";
    case 0x64: return "RESET_DEVICE";
    case 0x65: return "CLEAR_ERRORS";
    case 0x80: return "MIT_CONTROL_BCAST";
    case 0x81: return "POS_CONTROL_BCAST";
    case 0x82: return "VEL_CONTROL_BCAST";
    case 0x83: return "TORQUE_CONTROL_BCAST";
    case 0xC0: return "ESTOP";
    case 0xC1: return "FAULT_ALERT";
    default:   return QStringLiteral("Reserved");
    }
}
QString cbErrName(int e) {
    static const char *n[] = {"无错误", "电机故障", "编码器故障", "控制器故障",
                              "欠压故障", "过温故障", "过流故障", "堵转故障",
                              "CAN通信超时", "保留", "保留", "保留", "保留", "保留", "保留",
                              "多错误"};
    return QString::fromLatin1(n[e & 0xF]);
}
QString cbModeName(int m) {
    static const char *n[] = {"复位/未初始化", "标定中", "IDLE", "CLOSED_LOOP",
                              "MIT力位混合", "位置模式", "速度模式", "力矩模式"};
    return QString::fromLatin1(n[m & 0x7]);
}

// CANSimple MIT 量产帧解码（命令 / 响应同构，见手册 4.1 / CAN Simple 0x08）
//   pos: int16 BE, [offset=-max_pos, span=2*max_pos]
//   vel: 12bit,    [offset=-max_vel, span=2*max_vel]
//   kp : 12bit,    [0, max_kp]
//   kd : 12bit,    [0, max_kd]
//   tq : 12bit,    [offset=-max_torque, span=2*max_torque]
QString mitDecode(const QByteArray &d, bool asResponse) {
    if (d.size() < 8) return QStringLiteral("(MIT 数据不足 %1B)").arg(d.size());
    const double maxPos = 4.0 * M_PI, maxVel = 30.0, maxKp = 500.0, maxKd = 100.0, maxTq = 18.0;
    const int posRaw = (qint16)(((quint8)d[0] << 8) | (quint8)d[1]);
    const int vel12 = ((quint8)d[2] << 4) | ((quint8)d[3] >> 4);
    const int a = (quint8)d[3] & 0xF, b = (quint8)d[4];
    const int kp12 = (a << 8) | b;
    const int kd12 = ((quint8)d[5] << 4) | ((quint8)d[6] >> 4);
    const int c = (quint8)d[6] & 0xF, e = (quint8)d[7];
    const double pos = posRaw / 65535.0 * (2 * maxPos) - maxPos;
    const double vel = vel12 / 4095.0 * (2 * maxVel) - maxVel;
    if (asResponse) {
        const int cur12 = ((quint8)d[4] << 4) | ((quint8)d[5] >> 4);
        const int err4 = (quint8)d[3] & 0xF;
        const int mode4 = (quint8)d[5] & 0xF;
        const double cur = cur12 / 4095.0 * (2 * 40.0) - 40.0;   // 默认 ±40A
        return QStringLiteral("Pos=%1 rad Vel=%2 rad/s Cur=%3 A Err=%4 Mode=%5 MtrTmp=%6°C MosTmp=%7°C")
            .arg(num(pos)).arg(num(vel)).arg(num(cur)).arg(cbErrName(err4))
            .arg(cbModeName(mode4)).arg((int)(quint8)d[6] - 50).arg((int)(quint8)d[7] - 50);
    }
    const int tq12 = (c << 8) | e;
    const double kp = kp12 / 4095.0 * maxKp;
    const double kd = kd12 / 4095.0 * maxKd;
    const double tq = tq12 / 4095.0 * (2 * maxTq) - maxTq;
    return QStringLiteral("Pos=%1 rad Vel=%2 rad/s Kp=%3 Kd=%4 TqFF=%5 N·m")
        .arg(num(pos)).arg(num(vel)).arg(num(kp)).arg(num(kd)).arg(num(tq));
}

QString cbHeartbeat(const QByteArray &d) {
    if (d.size() >= 18) {
        const int life = ((quint8)d[0] >> 5) & 0x7, flags = (quint8)d[0] & 0x1F;
        const int state = ((quint8)d[1] >> 4) & 0xF, mode = (quint8)d[1] & 0xF;
        return QStringLiteral("Life=%1 ErrFlags=0x%2 State=%3 Mode=%4 MtrTmp=%5°C MosTmp=%6°C "
                              "Vbus=%7V Ibus=%8A MtrPos=%9 MtrVel=%10 Iq=%11A")
            .arg(life).arg(flags, 2, 16, QLatin1Char('0')).arg(state).arg(cbModeName(mode))
            .arg((int)(quint8)d[2] - 50).arg((int)(quint8)d[3] - 50)
            .arg(num(u16be(d, 4) * 0.1)).arg(num(i16be(d, 6) * 0.01))
            .arg(num(i32be(d, 8) / 10000.0)).arg(num(i32be(d, 12) / 10000.0))
            .arg(num(i16be(d, 16) * 0.01));
    }
    if (d.size() >= 8) {
        const int life = ((quint8)d[0] >> 5) & 0x7, flags = (quint8)d[0] & 0x1F;
        const int state = ((quint8)d[1] >> 4) & 0xF, mode = (quint8)d[1] & 0xF;
        return QStringLiteral("(精简) Life=%1 ErrFlags=0x%2 State=%3 Mode=%4 MtrTmp=%5°C "
                              "MtrPos=%6 MtrVel=%7 Iq=%8A")
            .arg(life).arg(flags, 2, 16, QLatin1Char('0')).arg(state).arg(cbModeName(mode))
            .arg((int)(quint8)d[2] - 50)
            .arg(num(i16be(d, 3) / 100.0)).arg(num(i16be(d, 5) / 100.0)).arg(num((qint8)d[7] * 0.5));
    }
    return QStringLiteral("(心跳长度异常 %1B)").arg(d.size());
}

} // namespace

QString frameParse::cyberBeast(const proto::RxFrame &f)
{
    const quint32 id = f.id;
    const int pri = (id >> 26) & 0x7, msg = (id >> 18) & 0xFF;
    const int dst = (id >> 10) & 0xFF, src = (id >> 2) & 0xFF, seq = id & 0x3;
    const QByteArray &d = f.data;

    QString head = QStringLiteral("0x%1 | Pri=%2 Msg=0x%3 %4 Dest=%5 Src=%6 Seq=%7")
        .arg(id, 8, 16, QLatin1Char('0')).toUpper()
        .arg(cbPriorityName(pri))
        .arg(msg, 2, 16, QLatin1Char('0')).toUpper()
        .arg(cbMsgName(msg)).arg(dst).arg(src).arg(seq);

    QString body;
    switch (msg & 0xFF) {
    case 0x00: case 0x80: body = mitDecode(d, /*asResponse=*/true); break;  // 收到的多为响应
    case 0x01: case 0x81: {
        if (d.size() >= 12)
            body = QStringLiteral("Pos=%1° VelLimit=%2 RPM CurLimit=%3 A (FD)")
                       .arg(num(f32be(d, 0))).arg(num(f32be(d, 4))).arg(num(f32be(d, 8)));
        else if (d.size() >= 8)
            body = QStringLiteral("Pos=%1° VelLimit=%2 RPM CurLimit=%3 A (Classic)")
                       .arg(num(f32be(d, 0))).arg(i16be(d, 4)).arg(num(i16be(d, 6) * 0.1));
        break; }
    case 0x02: body = QStringLiteral("Vel=%1 RPM CurLimit=%2 A").arg(num(f32be(d, 0))).arg(num(f32be(d, 4))); break;
    case 0x03: body = QStringLiteral("Torque=%1 N·m").arg(num(f32be(d, 0))); break;
    case 0x04: body = QStringLiteral("Current=%1 A").arg(num(f32be(d, 0))); break;
    case 0x20: case 0x21: {
        const int flags = d.size() > 0 ? (quint8)d[0] : 0;
        if (d.size() >= 4)
            body = QStringLiteral("Flags=0x%1 Endpoint=0x%2 DataLen=%3 Val=%4")
                .arg(flags, 2, 16, QLatin1Char('0')).arg(u16be(d, 1), 4, 16, QLatin1Char('0'))
                .arg((int)(quint8)d[3]).arg(hexb(d.mid(4)));
        break; }
    case 0x40: body = mitDecode(d, true); break;
    case 0x41: body = QStringLiteral("Pos=%1 turns Vel=%2 turns/s").arg(num(f32be(d, 0))).arg(num(f32be(d, 4))); break;
    case 0x42: body = QStringLiteral("Iq=%1 A Id=%2 A").arg(num(f32be(d, 0))).arg(num(f32be(d, 4))); break;
    case 0x43: body = QStringLiteral("MotorTmp=%1°C FETTmp=%2°C").arg(num(f32be(d, 0))).arg(num(f32be(d, 4))); break;
    case 0x44: body = QStringLiteral("Vbus=%1 V Ibus=%2 A").arg(num(f32be(d, 0))).arg(num(f32be(d, 4))); break;
    case 0x45: body = QStringLiteral("ErrType=%1 ErrVal=0x%2")
                         .arg(d.size() > 0 ? (int)(quint8)d[0] : 0)
                         .arg(d.size() >= 8 ? u32be(d, 4) : 0, 8, 16, QLatin1Char('0')); break;
    case 0x46:
        if (d.size() >= 16)
            body = QStringLiteral("HW=0x%1 FW=0x%2 SN=%3")
                       .arg(u32be(d, 0), 6, 16, QLatin1Char('0')).arg(u32be(d, 4), 6, 16, QLatin1Char('0'))
                       .arg(u64be(d, 8));
        else if (d.size() >= 8)
            body = QStringLiteral("HW=0x%1 FW=0x%2 (Classic)")
                       .arg(u32be(d, 0), 6, 16, QLatin1Char('0')).arg(u32be(d, 4), 6, 16, QLatin1Char('0'));
        break;
    case 0x47: body = QStringLiteral("ElecPwr=%1 W MechPwr=%2 W").arg(num(f32be(d, 0))).arg(num(f32be(d, 4))); break;
    case 0x48: body = cbHeartbeat(d); break;
    case 0x49: body = mitDecode(d, true); break;
    case 0xC0: body = d.isEmpty() ? QStringLiteral("ESTOP") : QStringLiteral("ESTOP Flags=0x%1").arg((int)(quint8)d[0], 2, 16, QLatin1Char('0')); break;
    default: break;
    }
    if (body.isEmpty())
        body = d.isEmpty() ? QStringLiteral("(无数据)") : hexb(d);
    return head + QStringLiteral(" || ") + body;
}

// ===========================================================================
//  CAN Simple
// ===========================================================================
namespace {
QString csCmdName(int c) {
    switch (c) {
    case 0x01: return "Heartbeat";
    case 0x02: return "Estop";
    case 0x03: return "Get_Error";
    case 0x04: return "RxSdo";
    case 0x05: return "TxSdo";
    case 0x06: return "Set_Axis_Node_ID";
    case 0x07: return "Set_Axis_State";
    case 0x08: return "Mit_Control";
    case 0x09: return "Get_Encoder_Estimates";
    case 0x0A: return "Get_Encoder_Count";
    case 0x0B: return "Set_Controller_Mode";
    case 0x0C: return "Set_Input_Pos";
    case 0x0D: return "Set_Input_Vel";
    case 0x0E: return "Set_Input_Torque";
    case 0x0F: return "Set_Limits";
    case 0x10: return "Start_Anticogging";
    case 0x11: return "Set_Traj_Vel_Limit";
    case 0x12: return "Set_Traj_Accel_Limits";
    case 0x13: return "Set_Traj_Inertia";
    case 0x14: return "Get_Iq";
    case 0x16: return "Reboot";
    case 0x17: return "Get_Bus_Voltage_Current";
    case 0x18: return "Clear_Errors";
    case 0x19: return "Set_Move_Incremental";
    case 0x1A: return "Set_Pos_Gain";
    case 0x1B: return "Set_Vel_Gains";
    case 0x1C: return "Get_Torques";
    case 0x1D: return "Get_Powers";
    case 0x1E: return "Disable_Can";
    case 0x1F: return "Save_Configuration";
    default:   return QStringLiteral("cmd_0x%1").arg(c, 2, 16, QLatin1Char('0'));
    }
}
} // namespace

QString frameParse::simple(const proto::RxFrame &f)
{
    const quint32 id = f.id & 0x7FF;
    const int node = (id >> 5) & 0x3F, cmd = id & 0x1F;
    const QByteArray &d = f.data;
    QString head = QStringLiteral("0x%1 | node=%2 cmd=0x%3 %4")
        .arg(id, 3, 16, QLatin1Char('0')).toUpper()
        .arg(node).arg(cmd, 2, 16, QLatin1Char('0')).toUpper().arg(csCmdName(cmd));

    QString body;
    switch (cmd) {
    case 0x01:
        if (d.size() >= 8)
            body = QStringLiteral("Axis_Error=0x%1 State=%2 Flags=0x%3 Temp=%4°C Life=%5")
                       .arg(u32le(d, 0), 8, 16, QLatin1Char('0')).arg((int)(quint8)d[4])
                       .arg((int)(quint8)d[5], 2, 16, QLatin1Char('0')).arg((int)(qint8)d[6]).arg((int)(quint8)d[7]);
        break;
    case 0x09:
        if (d.size() >= 8)
            body = QStringLiteral("Pos_Estimate=%1 Vel_Estimate=%2").arg(num(f32le(d, 0))).arg(num(f32le(d, 4)));
        break;
    case 0x0A:
        if (d.size() >= 8)
            body = QStringLiteral("Shadow_Count=%1 Count_In_Cpr=%2").arg(i32le(d, 0)).arg(i32le(d, 4));
        break;
    case 0x0C:
        if (d.size() >= 8)
            body = QStringLiteral("Input_Pos=%1 Vel_FF=%2 r/s Torque_FF=%3 Nm")
                       .arg(num(f32le(d, 0))).arg(num(i16le(d, 4) / 1000.0)).arg(num(i16le(d, 6) / 1000.0));
        break;
    case 0x0D:
        if (d.size() >= 8)
            body = QStringLiteral("Input_Vel=%1 r/s Torque_FF=%2 Nm").arg(num(f32le(d, 0))).arg(num(f32le(d, 4)));
        break;
    case 0x0E:
        if (d.size() >= 4) body = QStringLiteral("Input_Torque=%1 Nm").arg(num(f32le(d, 0)));
        break;
    case 0x07:
        if (d.size() >= 4) body = QStringLiteral("Axis_Requested_State=%1").arg(u32le(d, 0));
        break;
    case 0x03:
        if (d.size() >= 1) body = QStringLiteral("Error_Type=%1").arg((int)(quint8)d[0]);
        break;
    case 0x08: body = mitDecode(d, /*asResponse=*/false); break;
    case 0x14:
        if (d.size() >= 8) body = QStringLiteral("Iq_Setpoint=%1 Iq_Measured=%2").arg(num(f32le(d, 0))).arg(num(f32le(d, 4)));
        break;
    case 0x17:
        if (d.size() >= 8) body = QStringLiteral("Bus_Voltage=%1 V Bus_Current=%2 A").arg(num(f32le(d, 0))).arg(num(f32le(d, 4)));
        break;
    case 0x1C:
        if (d.size() >= 8) body = QStringLiteral("Torque_Setpoint=%1 Torque=%2").arg(num(f32le(d, 0))).arg(num(f32le(d, 4)));
        break;
    case 0x1D:
        if (d.size() >= 8) body = QStringLiteral("Elec_Power=%1 W Mech_Power=%2 W").arg(num(f32le(d, 0))).arg(num(f32le(d, 4)));
        break;
    default: break;
    }
    if (body.isEmpty()) {
        // 兜底：8 字节按两个 float32(LE) + 原始 hex
        if (d.size() == 8)
            body = QStringLiteral("f32[0]=%1 f32[1]=%2  | %3").arg(num(f32le(d, 0))).arg(num(f32le(d, 4))).arg(hexb(d));
        else
            body = d.isEmpty() ? QStringLiteral("(无数据)") : hexb(d);
    }
    return head + QStringLiteral(" || ") + body;
}

// ===========================================================================
//  CANopen
// ===========================================================================
QString frameParse::canopen(const proto::RxFrame &f)
{
    const quint32 id = f.id & 0x7FF;
    const QByteArray &d = f.data;
    QString head, body;

    auto nmtName = [](int c) -> QString {
        switch (c & 0xFF) {
        case 0x01: return "Start";
        case 0x02: return "Stop";
        case 0x80: return "Pre-operational";
        case 0x81: return "Reset Node";
        case 0x82: return "Reset Communication";
        default:   return QStringLiteral("cmd=0x%1").arg(c & 0xFF, 2, 16, QLatin1Char('0'));
        }
    };

    if (id == 0x000) {                       // NMT
        head = QStringLiteral("0x000 | NMT");
        body = d.size() >= 2
                   ? QStringLiteral("%1 Node=%2").arg(nmtName((quint8)d[0])).arg((int)(quint8)d[1] == 0 ? QStringLiteral("全部") : QString::number((int)(quint8)d[1]))
                   : hexb(d);
    } else if (id == 0x080) {                // SYNC
        head = QStringLiteral("0x080 | SYNC");
        body = d.isEmpty() ? QStringLiteral("(空)") : hexb(d);
    } else if (id >= 0x081 && id <= 0x0FF) { // EMCY
        const int node = id - 0x080;
        head = QStringLiteral("0x%1 | EMCY node=%2").arg(id, 3, 16, QLatin1Char('0')).toUpper().arg(node);
        body = d.size() >= 3 ? QStringLiteral("ErrCode=0x%1 Reg=0x%2 Data=%3")
                                   .arg(u16le(d, 0), 4, 16, QLatin1Char('0'))
                                   .arg((int)(quint8)d[2], 2, 16, QLatin1Char('0')).arg(hexb(d.mid(3)))
                             : hexb(d);
    } else if (id >= 0x180 && id <= 0x4FF) { // PDO
        int node = 0; QString kind;
        auto setPdo = [&](int base, const char *nm, int idx) {
            head = QStringLiteral("0x%1 | %2%3 node=%4").arg(id, 3, 16, QLatin1Char('0')).toUpper()
                       .arg(QString::fromLatin1(nm)).arg(idx).arg(id - base);
            node = id - base; (void)node;
        };
        if (id >= 0x180 && id <= 0x1FF) setPdo(0x180, "TPDO", 1);
        else if (id >= 0x200 && id <= 0x2FF) setPdo(0x200, "RPDO", 1);
        else if (id >= 0x280 && id <= 0x2FF) setPdo(0x280, "TPDO", 2);
        else if (id >= 0x300 && id <= 0x3FF) setPdo(0x300, "RPDO", 2);
        else if (id >= 0x380 && id <= 0x3FF) setPdo(0x380, "TPDO", 3);
        else if (id >= 0x400 && id <= 0x4FF) setPdo(0x400, "RPDO", 3);
        else if (id >= 0x480 && id <= 0x4FF) setPdo(0x480, "TPDO", 4);
        else if (id >= 0x500 && id <= 0x5FF) setPdo(0x500, "RPDO", 4);
        else head = QStringLiteral("0x%1 | PDO").arg(id, 3, 16, QLatin1Char('0')).toUpper();
        body = QStringLiteral("Data=%1 (映射见对象字典)").arg(hexb(d));
    } else if (id >= 0x580 && id <= 0x67F) { // SDO
        const bool resp = (id < 0x600);
        const int node = resp ? (id - 0x580) : (id - 0x600);
        head = QStringLiteral("0x%1 | SDO %2 node=%3").arg(id, 3, 16, QLatin1Char('0')).toUpper()
                   .arg(resp ? QStringLiteral("响应") : QStringLiteral("请求")).arg(node);
        if (d.size() >= 4) {
            const int cs = (quint8)d[0], idx = u16le(d, 1), sub = (quint8)d[3];
            body = QStringLiteral("CS=0x%1 Index=0x%2:%3 Data=%4")
                       .arg(cs, 2, 16, QLatin1Char('0')).arg(idx, 4, 16, QLatin1Char('0')).arg(sub).arg(hexb(d.mid(4)));
        } else body = hexb(d);
    } else if (id >= 0x700 && id <= 0x77F) { // 心跳 / 节点守护
        const int node = id - 0x700;
        head = QStringLiteral("0x%1 | 心跳 node=%2").arg(id, 3, 16, QLatin1Char('0')).toUpper().arg(node);
        QString st = QStringLiteral("?");
        if (d.size() >= 1) {
            switch ((quint8)d[0]) {
            case 0: st = "Boot-up"; break;
            case 4: st = "Stopped"; break;
            case 5: st = "Operational"; break;
            case 127: st = "Pre-operational"; break;
            default: st = QStringLiteral("0x%1").arg((int)(quint8)d[0], 2, 16, QLatin1Char('0'));
            }
        }
        body = QStringLiteral("State=%1").arg(st);
    } else {
        head = QStringLiteral("0x%1 | 未知功能").arg(id, 3, 16, QLatin1Char('0')).toUpper();
        body = hexb(d);
    }
    return head + QStringLiteral(" || ") + body;
}

// ===========================================================================
//  RS485 / Modbus RTU
// ===========================================================================
namespace {
quint16 modbusCrc(const QByteArray &d) {
    quint16 crc = 0xFFFF;
    for (int i = 0; i < d.size(); ++i) {
        crc ^= (quint8)d[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (quint16)((crc >> 1) ^ 0xA001) : (quint16)(crc >> 1);
    }
    return crc;
}
QString mbFuncName(int fc) {
    switch (fc) {
    case 0x03: return "读保持寄存器";
    case 0x04: return "读输入寄存器";
    case 0x06: return "写单个保持寄存器";
    case 0x10: return "写多个保持寄存器";
    default: return QStringLiteral("功能码 0x%1").arg(fc, 2, 16, QLatin1Char('0'));
    }
}
} // namespace

QString frameParse::rs485(const proto::RxFrame &f)
{
    const QByteArray &d = f.data;
    if (d.size() < 4)
        return QStringLiteral("Modbus(?) %1").arg(hexb(d));
    const int addr = (quint8)d[0], fc = (quint8)d[1];
    const quint16 crcCalc = modbusCrc(d.left(d.size() - 2));
    const quint16 crcGot = (quint16)((quint8)d[d.size() - 2] | ((quint8)d[d.size() - 1] << 8));
    const bool crcOk = (crcCalc == crcGot || d.size() < 4);
    QString head = QStringLiteral("addr=%1 func=0x%2 %3 | CRC %4")
        .arg(addr).arg(fc, 2, 16, QLatin1Char('0')).arg(mbFuncName(fc))
        .arg(crcOk ? QStringLiteral("OK") : QStringLiteral("ERR"));

    QString body;
    if (fc == 0x03 || fc == 0x04) {
        if (d.size() == 8) {   // 请求
            body = QStringLiteral("(请求) 起始地址=0x%1 寄存器数=%2")
                       .arg(u16be(d, 2), 4, 16, QLatin1Char('0')).arg(u16be(d, 4));
        } else if (d.size() >= 5) {   // 响应
            const int n = (quint8)d[2];
            body = QStringLiteral("(响应) 字节数=%1").arg(n);
            QStringList regs;
            for (int i = 0; i + 1 < n; i += 2)
                regs << QStringLiteral("0x%1").arg(u16be(d, 3 + i), 4, 16, QLatin1Char('0'));
            if (!regs.isEmpty()) body += QStringLiteral(" 寄存器=[%1]").arg(regs.join(", "));
            if (n == 4) body += QStringLiteral(" float=%1").arg(num(f32be(d, 3)));
        }
    } else if (fc == 0x06) {
        if (d.size() >= 6)
            body = QStringLiteral("寄存器=0x%1 值=0x%2 (%3)").arg(u16be(d, 2), 4, 16, QLatin1Char('0'))
                       .arg(u16be(d, 4), 4, 16, QLatin1Char('0')).arg(u16be(d, 4));
    } else if (fc == 0x10) {
        if (d.size() >= 7)
            body = QStringLiteral("起始=0x%1 数量=%2 字节数=%3 数据=%4")
                       .arg(u16be(d, 2), 4, 16, QLatin1Char('0')).arg(u16be(d, 4))
                       .arg((int)(quint8)d[6]).arg(hexb(d.mid(7)));
    }
    if (body.isEmpty()) body = hexb(d);
    return head + QStringLiteral(" || ") + body;
}

// ===========================================================================
//  通用
// ===========================================================================
QString frameParse::generic(const proto::RxFrame &f)
{
    QString t = QString::fromLatin1(directionText(f.ext));
    if (f.fd) t += f.brs ? " FD加速" : " FD";
    if (f.rtr) t += " RTR";
    return QStringLiteral("0x%1 | %2 长度=%3 数据=%4")
        .arg(f.id, f.ext ? 8 : 3, 16, QLatin1Char('0')).toUpper()
        .arg(t).arg(f.data.size()).arg(hexb(f.data));
}
