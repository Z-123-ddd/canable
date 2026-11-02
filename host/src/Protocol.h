// ============================================================================
//  Protocol — CANable2-N32 上位机侧协议编解码（与固件 usb_proto.c 逐字节对齐）
//
//  下行（host -> MCU）：
//    命令帧  AA AA | CMD(1) | VALUE(4, LE)
//    数据帧  AA AB | CTRL(1) | ID(2/4, BE) | DATA(0..64) | CRC(2/3)
//            CTRL.bit4 IDE：0=标准(2B ID) / 1=扩展(4B ID)
//  上行（MCU -> host）：同上（接收上报）
//    响应帧  AA AC | RSPID(1) | PAYLOAD
//            id1 波特率锁定：hz[4] LE
//            id2 版本：LEN(1)+TEXT
//            id3 错误帧累计：TEC_u32 LE | REC_u32 LE | CRC8(8B)  （MCU 主动上报）
//    状态行  ASCII "ST:..."（GET_STATUS 回包）
//
//  CRC：经典 CRC-15 / FD<=16 CRC-17 / FD>16 CRC-21，MSB-first, init 0,
//       xorout 0, 无反射，覆盖 CTRL+ID+DATA。
// ============================================================================
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>

namespace proto {

// ---- 帧头 ----
constexpr uint8_t SOF1      = 0xAA;
constexpr uint8_t CMD_SOF2  = 0xAA;   // 命令帧
constexpr uint8_t DATA_SOF2 = 0xAB;   // 数据帧
constexpr uint8_t RSP_SOF2  = 0xAC;   // 响应/事件帧 (MCU->host)
constexpr uint8_t LOG_SOF2  = 0xAD;   // 警告/报错帧 (MCU->host): AA AD | TEXT(ASCII) | DA AA | CRC

// ---- CTRL 位 ----
constexpr uint8_t CTRL_FDF   = 0x80;  // CAN FD
constexpr uint8_t CTRL_BRS   = 0x40;  // 位速率切换 (FD)
constexpr uint8_t CTRL_RTR   = 0x20;  // 远程帧 (经典)
constexpr uint8_t CTRL_IDE   = 0x10;  // 1=扩展 ID(4B) / 0=标准 ID(2B)
constexpr uint8_t CTRL_DLC_M = 0x0F;  // DLC 低半字节

// ---- 命令码 ----
enum Cmd : uint8_t {
    CMD_SET_BITRATE = 1,   // value = 仲裁段波特率(Hz)
    CMD_TX_START    = 2,   // value: 0=单次 1=连续
    CMD_TX_STOP     = 3,
    CMD_GET_STATUS  = 4,
    CMD_ENTER_DFU   = 5,
    CMD_SET_INTERVAL= 6,   // value = 周期(ms), 1..1000
    CMD_SET_MODE    = 7,   // value: 0=单帧 1=列表
    CMD_SET_DATA_BITRATE = 8,  // value = CAN FD 数据域波特率(Hz): 2000000/5000000
    CMD_SET_WORK_MODE    = 9,  // value: 0=CAN 1=RS485（切换调试器工作模式）
    CMD_GET_VERSION      = 10, // 查询固件版本（回 AA AC RSP_VERSION）
    CMD_SET_PAUSE        = 11, // value: 1=暂停接收（停 CAN 收发中断+清未完成帧） 0=恢复
    CMD_CLEAR_ERRORS     = 12, // 清除固件内部错误寄存器（无 value）
};

// 发送模式
constexpr uint32_t MODE_SINGLE    = 0;
constexpr uint32_t MODE_FRAMELIST = 1;

// 周期范围 (ms)
constexpr uint32_t INTERVAL_MIN_MS = 1;
constexpr uint32_t INTERVAL_MAX_MS = 1000;

// 响应 id
constexpr uint8_t RSP_BITRATE_LOCK = 1;
constexpr uint8_t RSP_VERSION      = 2;   // 版本：AA AC 02 | LEN(1) | TEXT(ASCII)
constexpr uint8_t RSP_ERR_CNT      = 3;   // 错误帧累计：AA AC 03 | TEC_u32 LE | REC_u32 LE | CRC8

// ---- DLC <-> 字节数 ----
int  dlcToBytes(int dlc);     // 0..15 -> 0..64  (无效返回 -1)
int  bytesToDlc(int bytes);   // 0..64 -> 规范 DLC 码 (无效返回 -1)

// ---- 组帧 ----
QByteArray buildCommand(Cmd cmd, uint32_t value);
QByteArray buildDataFrame(uint32_t id, bool ext, bool fd, bool brs, bool rtr,
                          int dlc, const QByteArray &data);

// ---- CRC ----
// (CRC15/17/21 已移除：AA AB 数据帧不再带应用层 CRC，见 Protocol.cpp)


// ---- 解析 ----
struct RxFrame {                 // AA AB（MCU 上报的接收帧）
    uint32_t id = 0;
    bool ext = false, fd = false, brs = false, rtr = false;
    int dlc = 0;
    QByteArray data;
};

struct RxResponse {              // AA AC
    uint8_t id = 0;
    uint32_t value = 0;
    QString text;                // RSP_VERSION 的 ASCII 负载（其余为空）
    uint32_t tec = 0;            // RSP_ERR_CNT：累计发送错误帧数（固件已按 TEC÷8 累计）
    uint32_t rec = 0;            // RSP_ERR_CNT：累计接收错误帧数
    bool crcOk = true;           // RSP_ERR_CNT 的 CRC8；其它 id 为 true
};

struct LogMessage {              // AA AD（警告/报错）
    QString text;               // ASCII 正文（错误码）
    bool crcOk = true;          // CRC 校验结果（不符仍显示）
};

struct Message {
    enum Type { CanFrame, Response, StatusLine, Text, Log };
    Type type = Text;
    RxFrame frame;
    RxResponse response;
    QString text;                // StatusLine / Text
    bool crcOk = true;           // CanFrame 的 CRC 校验结果
};

// 字节流解析器（USB 包边界无关）
class Parser {
public:
    void feed(const QByteArray &bytes);
    void reset();
    QVector<Message> takeMessages() { auto m = m_msgs; m_msgs.clear(); return m; }
private:
    bool tryParseBinary(int &outConsumed);
    void emitFrame(int idBytes, bool fd, int dataBytes);
    void finishResponse();
    enum State { IDLE, HDR, CMD, DATA_ACC, RSP_ID, RSP_PAYLOAD, RSP_VER_LEN, RSP_VER_TEXT, TEXT, LOG_TEXT, LOG_TAIL, LOG_CRC };
    State m_state = IDLE;
    QByteArray m_buf;      // 二进制帧累积
    QString m_text;        // ASCII 状态行累积
    int m_need = 0;
    uint8_t m_rspId = 0;
    QVector<Message> m_msgs;
};

// ---- 显示辅助 ----
// 把一帧 CAN 帧格式化为一行文本（dir = "RX"/"TX"），供滚动视图复用
QString formatCanFrame(const RxFrame &f, const char *dir);
// 解码一帧完整的 AA AB 线上帧（用于 TX 回显）；成功返回 true
bool decodeDataFrame(const QByteArray &wire, RxFrame *out);

// ---- 自检：返回 true 表示全部通过，报告写入 report ----
bool selfTest(QString *report);

} // namespace proto

#endif // PROTOCOL_H
