#include "Protocol.h"

namespace proto {

// ============================================================================
//  DLC 表
// ============================================================================
int dlcToBytes(int dlc)
{
    static const int tbl[16] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
    if (dlc < 0 || dlc > 15) return -1;
    return tbl[dlc];
}

int bytesToDlc(int bytes)
{
    if (bytes < 0 || bytes > 64) return -1;
    if (bytes <= 8) return bytes;
    switch (bytes) {
        case 12: return 9;
        case 16: return 10;
        case 20: return 11;
        case 24: return 12;
        case 32: return 13;
        case 48: return 14;
        case 64: return 15;
    }
    return -1;   // 非规范长度
}

// ============================================================================
//  CRC 引擎（与固件 crc_msb_width 完全一致）
// ============================================================================
static uint32_t crcMsbWidth(const uint8_t *p, int n, int width, uint32_t poly)
{
    uint32_t crc = 0;
    const uint32_t top = 1u << (width - 1);
    const int shift = width - 8;
    for (int i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << shift;
        for (int b = 0; b < 8; b++) {
            if (crc & top) crc = (crc << 1) ^ poly;
            else           crc <<= 1;
        }
    }
    return crc & ((1u << width) - 1u);
}

uint32_t crc15(const uint8_t *p, int n) { return crcMsbWidth(p, n, 15, 0x4599u); }
uint32_t crc17(const uint8_t *p, int n) { return crcMsbWidth(p, n, 17, 0x1685Bu); }
uint32_t crc21(const uint8_t *p, int n) { return crcMsbWidth(p, n, 21, 0x102899u); }

uint32_t crcForFrame(bool fd, int dataBytes, const uint8_t *p, int n)
{
    if (fd)
        return (dataBytes <= 16) ? crc17(p, n) : crc21(p, n);
    return crc15(p, n);
}

// AA AD 警告/报错帧的 CRC：CRC-8, poly 0x07, init 0，覆盖 ASCII 正文（不含 AA AD / DA AA）
static uint8_t crc8(const uint8_t *p, int n)
{
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

// ============================================================================
//  组帧
// ============================================================================
QByteArray buildCommand(Cmd cmd, uint32_t value)
{
    QByteArray b;
    b.append(char(SOF1));
    b.append(char(CMD_SOF2));
    b.append(char(cmd));
    b.append(char(value & 0xFF));
    b.append(char((value >> 8) & 0xFF));
    b.append(char((value >> 16) & 0xFF));
    b.append(char((value >> 24) & 0xFF));
    return b;
}

QByteArray buildDataFrame(uint32_t id, bool ext, bool fd, bool brs, bool rtr,
                          int dlc, const QByteArray &data)
{
    QByteArray b;
    b.append(char(SOF1));
    b.append(char(DATA_SOF2));

    // ID 大端：标准 2 字节 / 扩展 4 字节
    if (ext) {
        b.append(char((id >> 24) & 0xFF));
        b.append(char((id >> 16) & 0xFF));
    }
    b.append(char((id >> 8) & 0xFF));
    b.append(char(id & 0xFF));

    uint8_t ctrl = 0;
    if (fd)  ctrl |= CTRL_FDF;
    if (fd && brs) ctrl |= CTRL_BRS;
    if (!fd && rtr) ctrl |= CTRL_RTR;
    ctrl |= (uint8_t)(dlc & CTRL_DLC_M);
    b.append(char(ctrl));

    // DATA
    int nbytes = dlcToBytes(dlc);
    if (nbytes < 0) nbytes = 0;
    for (int i = 0; i < nbytes; i++)
        b.append(i < data.size() ? data[i] : char(0));

    // CRC over ID+CTRL+DATA（跳过 2 字节帧头）
    int cov = b.size() - 2;
    uint32_t crc = crcForFrame(fd, nbytes, (const uint8_t *)b.constData() + 2, cov);
    if (fd) {
        b.append(char((crc >> 16) & 0xFF));
        b.append(char((crc >> 8) & 0xFF));
        b.append(char(crc & 0xFF));
    } else {
        b.append(char((crc >> 8) & 0xFF));
        b.append(char(crc & 0xFF));
    }
    return b;
}

// ============================================================================
//  字节流解析器
// ----------------------------------------------------------------------------
// 说明：AA AB 接收帧的 ID 宽度（标准 2B / 扩展 4B）随实际帧变化，报文中没有
//       显式标志位，因此这里对同一段字节分别按 2B 与 4B 解释，取 CRC 校验通过者。
// ============================================================================
void Parser::reset()
{
    m_state = IDLE;
    m_buf.clear();
    m_text.clear();
    m_need = 0;
}

// 尝试把 m_buf 当作 AA AB 数据帧解析（ID 宽度 2 或 4）。成功则发消息并清空。
bool Parser::tryParseBinary(int &outConsumed)
{
    (void)outConsumed;
    int n = m_buf.size();
    const uint8_t *p = (const uint8_t *)m_buf.constData();
    for (int idb = 2; idb <= 4; idb += 2) {
        if (n < idb + 1) continue;
        uint8_t ctrl = p[idb];
        bool fd = (ctrl & CTRL_FDF) != 0;
        int dl = dlcToBytes(ctrl & CTRL_DLC_M);
        if (dl < 0) continue;
        bool rtr = (!fd && (ctrl & CTRL_RTR));
        int dataBytes = rtr ? 0 : dl;
        int crcBytes = fd ? 3 : 2;
        int total = idb + 1 + dataBytes + crcBytes;
        if (n < total) continue;
        uint32_t expect = crcForFrame(fd, dataBytes, p, idb + 1 + dataBytes);
        uint32_t got = 0;
        for (int i = 0; i < crcBytes; i++)
            got = (got << 8) | p[idb + 1 + dataBytes + i];
        if (got != expect) continue;
        emitFrame(idb, fd, dataBytes);
        m_buf.clear();
        return true;
    }
    return false;
}

void Parser::emitFrame(int idBytes, bool fd, int dataBytes)
{
    const uint8_t *p = (const uint8_t *)m_buf.constData();
    uint32_t id; int off;
    if (idBytes == 4) {
        id = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        off = 4;
    } else {
        id = (uint32_t(p[0]) << 8) | p[1];
        off = 2;
    }
    uint8_t ctrl = p[off];
    QByteArray data((const char *)(p + off + 1), dataBytes);

    Message m;
    m.type = Message::CanFrame;
    m.frame.id = id;
    m.frame.ext = (idBytes == 4);
    m.frame.fd = fd;
    m.frame.brs = fd && (ctrl & CTRL_BRS);
    m.frame.rtr = (!fd) && (ctrl & CTRL_RTR);
    m.frame.dlc = ctrl & CTRL_DLC_M;
    m.frame.data = data;
    m.crcOk = true;               // tryParseBinary 已通过 CRC 校验
    m_msgs.append(m);
}

void Parser::feed(const QByteArray &bytes)
{
    for (int k = 0; k < bytes.size(); k++) {
        const uint8_t by = (uint8_t)bytes[k];
        switch (m_state) {
        case IDLE:
            if (by == SOF1) m_state = HDR;
            else if (by >= 0x20 && by < 0x7F) { m_text = char(by); m_state = TEXT; }
            break;

        case HDR:
            if (by == CMD_SOF2)       { m_state = CMD;      m_need = 5; }
            else if (by == DATA_SOF2) { m_state = DATA_ACC; m_buf.clear(); }
            else if (by == RSP_SOF2)  { m_state = RSP_ID;   m_buf.clear(); m_need = 1; }
            else if (by == LOG_SOF2)  { m_state = LOG_TEXT; m_buf.clear(); }
            else if (by == SOF1)      { /* 保持等待 */ }
            else if (by >= 0x20 && by < 0x7F) { m_text = char(by); m_state = TEXT; }
            else m_state = IDLE;
            break;

        case CMD:                                  // 命令帧只用于下行，回读忽略
            if (--m_need == 0) m_state = IDLE;
            break;

        case DATA_ACC: {
            m_buf.append(char(by));
            int consumed = 0;
            if (tryParseBinary(consumed)) m_state = IDLE;
            else if (m_buf.size() >= 74) { m_buf.clear(); m_state = IDLE; }  // 超长丢弃
            break;
        }

        case RSP_ID:
            m_rspId = by;
            if (by == RSP_BITRATE_LOCK) { m_buf.clear(); m_state = RSP_PAYLOAD; m_need = 4; }
            else if (by == RSP_VERSION) { m_buf.clear(); m_text.clear(); m_state = RSP_VER_LEN; m_need = 1; }
            else { finishResponse(); m_state = IDLE; }
            break;

        case RSP_VER_LEN:                          // RSP_VERSION：先读 1 字节长度
            m_need = by;
            if (m_need == 0) { finishResponse(); m_state = IDLE; }
            else m_state = RSP_VER_TEXT;
            break;

        case RSP_VER_TEXT:                         // 再读 LEN 字节 ASCII
            m_text.append(char(by));
            if (--m_need == 0) { finishResponse(); m_state = IDLE; }
            break;

        case RSP_PAYLOAD:
            m_buf.append(char(by));
            if (--m_need == 0) { finishResponse(); m_state = IDLE; }
            break;

        case TEXT:
            if (by == 0x0D || by == 0x0A) {
                Message m; m.type = Message::StatusLine; m.text = m_text.trimmed();
                m_msgs.append(m);
                m_text.clear();
                m_state = IDLE;
            } else {
                m_text.append(char(by));
            }
            break;

        case LOG_TEXT:                             // 收集 AA AD 正文（ASCII），直到尾标 DA AA
            if (by == 0xDA) { m_state = LOG_TAIL; }
            else if (m_buf.size() < 512) { m_buf.append(char(by)); }
            else { m_buf.clear(); m_state = IDLE; }   // 超长丢弃
            break;

        case LOG_TAIL:                             // 期待尾标 DA AA，随后是 1 字节 CRC
            if (by == 0xAA) { m_state = LOG_CRC; }
            else {
                if (m_buf.size() < 510) { m_buf.append(char(0xDA)); m_buf.append(char(by)); }
                m_state = LOG_TEXT;
            }
            break;

        case LOG_CRC:                              // CRC 位于 DA AA 之后，仅覆盖 ASCII 正文
            {
                Message m; m.type = Message::Log;
                m.crcOk = (crc8((const uint8_t *)m_buf.constData(), m_buf.size()) == (uint8_t)by);
                m.text = QString::fromLatin1(m_buf);
                m_msgs.append(m);
                m_buf.clear();
                m_state = IDLE;
            }
            break;
        }
    }
}

// m_buf = payload[4] LE, m_rspId 已记录
void Parser::finishResponse()
{
    Message m;
    m.type = Message::Response;
    m.response.id = m_rspId;
    uint32_t v = 0;
    for (int i = 0; i < m_buf.size() && i < 4; i++)
        v |= (uint32_t)(uint8_t)m_buf[i] << (8 * i);
    m.response.value = v;
    m.response.text = m_text;   // RSP_VERSION 的 ASCII（其余为空）
    m_msgs.append(m);
    m_buf.clear();
    m_text.clear();
}

// ============================================================================
//  显示辅助
// ============================================================================
QString formatCanFrame(const RxFrame &f, const char *dir)
{
    QString idStr = QStringLiteral("0x") +
        QString::number(f.id, 16).toUpper().rightJustified(f.ext ? 8 : 3, QLatin1Char('0'));
    QString type = f.ext ? QStringLiteral("扩展") : QStringLiteral("标准");
    if (f.fd)  type += f.brs ? QStringLiteral(" FD加速") : QStringLiteral(" FD");
    if (f.rtr) type += QStringLiteral(" RTR");
    return QStringLiteral("%1 id=%2 %3 len=%4 data=[%5]")
        .arg(QString::fromLatin1(dir), idStr, type)
        .arg(f.data.size())
        .arg(QString::fromLatin1(f.data.toHex(' ').toUpper()));
}

bool decodeDataFrame(const QByteArray &wire, RxFrame *out)
{
    if (wire.size() < 4 || (uint8_t)wire[0] != SOF1 || (uint8_t)wire[1] != DATA_SOF2)
        return false;
    const uint8_t *p = (const uint8_t *)wire.constData();
    const int n = wire.size();
    const int H = 2;                     // AA AB 帧头
    for (int idb = 2; idb <= 4; idb += 2) {
        if (n < H + idb + 1) continue;
        const uint8_t *id = p + H;
        uint8_t ctrl = p[H + idb];
        bool fd = (ctrl & CTRL_FDF) != 0;
        int dl = dlcToBytes(ctrl & CTRL_DLC_M);
        if (dl < 0) continue;
        bool rtr = (!fd && (ctrl & CTRL_RTR));
        int dataBytes = rtr ? 0 : dl;
        int crcBytes = fd ? 3 : 2;
        int total = H + idb + 1 + dataBytes + crcBytes;
        if (n < total) continue;
        uint32_t expect = crcForFrame(fd, dataBytes, id, idb + 1 + dataBytes);
        uint32_t got = 0;
        for (int i = 0; i < crcBytes; i++) got = (got << 8) | p[H + idb + 1 + dataBytes + i];
        if (got != expect) continue;
        if (out) {
            out->id = (idb == 4) ? ((uint32_t(id[0]) << 24) | (uint32_t(id[1]) << 16) | (uint32_t(id[2]) << 8) | id[3])
                                 : ((uint32_t(id[0]) << 8) | id[1]);
            out->ext = (idb == 4); out->fd = fd; out->brs = fd && (ctrl & CTRL_BRS);
            out->rtr = rtr; out->dlc = ctrl & CTRL_DLC_M;
            out->data = QByteArray((const char *)(p + H + idb + 1), dataBytes);
        }
        return true;
    }
    return false;
}

// ============================================================================
//  自检
// ============================================================================
static QString hex(const QByteArray &b)
{
    return QString::fromLatin1(b.toHex(' '));
}

bool selfTest(QString *report)
{
    QString log;
    bool ok = true;
    auto check = [&](bool cond, const QString &name) {
        log += (cond ? "  [PASS] " : "  [FAIL] ") + name + "\n";
        if (!cond) ok = false;
    };

    // 1. DLC 表
    check(dlcToBytes(0) == 0 && dlcToBytes(8) == 8 && dlcToBytes(9) == 12 &&
          dlcToBytes(15) == 64 && dlcToBytes(16) == -1, "DLC 表 (0..15)");
    check(bytesToDlc(8) == 8 && bytesToDlc(12) == 9 && bytesToDlc(48) == 14 &&
          bytesToDlc(7) == 7, "字节->DLC 表");

    // 2. 命令帧格式 AA AA | cmd | value(LE)
    QByteArray c = buildCommand(CMD_SET_BITRATE, 500000);
    check(c.size() == 7 && (uint8_t)c[0] == 0xAA && (uint8_t)c[1] == 0xAA &&
          (uint8_t)c[2] == 1 && (uint8_t)c[3] == 0x20 && (uint8_t)c[4] == 0xA1 &&
          (uint8_t)c[5] == 0x07 && (uint8_t)c[6] == 0x00,
          "命令帧 500000Hz -> AA AA 01 20 A1 07 00");

    // 3. 经典帧已知 CRC（独立 Python 基准 = 0x05B1）
    QByteArray f = buildDataFrame(0x123, false, false, false, false, 2,
                                  QByteArray::fromHex("DEAD"));
    uint8_t covC[] = {0x01,0x23,0x02,0xDE,0xAD};
    check(crc15(covC, 5) == 0x05B1, "CRC-15 已知答案 0x05B1");
    check(f.size() == 9 && (uint8_t)f[2] == 0x01 && (uint8_t)f[3] == 0x23 &&
          (uint8_t)f[4] == 0x02 && (uint8_t)f[5] == 0xDE && (uint8_t)f[6] == 0xAD &&
          (uint8_t)f[7] == 0x05 && (uint8_t)f[8] == 0xB1,
          "经典帧组帧 -> AA AB 01 23 02 DE AD 05 B1");

    // 4. FD<=16 帧已知 CRC-17 (=0x168EC)
    QByteArray d8; for (int i=1;i<=8;i++) d8.append(char(i));
    QByteArray f17 = buildDataFrame(0x123, false, true, false, false, 8, d8);
    check(crc17((const uint8_t*)f17.constData()+2, f17.size()-2-3) == 0xBFA9,
          "CRC-17 已知答案 0xBFA9");
    check(f17.size() == 2+2+1+8+3, "FD(8B) 帧长 = 16 字节");

    // 5. FD>16 帧已知 CRC-21 (=0x15A6F7)
    QByteArray d20; for (int i=1;i<=20;i++) d20.append(char(i));
    QByteArray f21 = buildDataFrame(0x123, false, true, false, false, 11, d20);
    check(crc21((const uint8_t*)f21.constData()+2, f21.size()-2-3) == 0x15A6F7,
          "CRC-21 已知答案 0x15A6F7");

    // 6. 往返解析：经典帧
    {
        Parser ps; ps.feed(f);
        auto msgs = ps.takeMessages();
        bool good = msgs.size() == 1 && msgs[0].type == Message::CanFrame &&
                    msgs[0].frame.id == 0x123 && !msgs[0].frame.ext &&
                    !msgs[0].frame.fd && msgs[0].frame.dlc == 2 &&
                    msgs[0].frame.data == QByteArray::fromHex("DEAD") && msgs[0].crcOk;
        check(good, "解析经典帧往返 (id/data/crc)");
    }

    // 7. 往返解析：扩展 + FD + BRS
    {
        QByteArray fb = buildDataFrame(0x1ABCDEF, true, true, true, false, 9, QByteArray(12, char(0x5A)));
        Parser ps; ps.feed(fb);
        auto msgs = ps.takeMessages();
        bool good = msgs.size() == 1 && msgs[0].type == Message::CanFrame &&
                    msgs[0].frame.id == 0x1ABCDEF && msgs[0].frame.ext &&
                    msgs[0].frame.fd && msgs[0].frame.brs && msgs[0].crcOk;
        check(good, "解析扩展/FD/BRS 帧往返");
    }

    // 8. 分片喂入（跨包边界）
    {
        Parser ps;
        ps.feed(f.left(3));
        ps.feed(f.mid(3));
        auto msgs = ps.takeMessages();
        check(msgs.size() == 1 && msgs[0].crcOk, "分片喂入跨包解析");
    }

    // 9. AA AC 响应（波特率锁定 500000 = 0x0007A120）
    {
        QByteArray r; r.append(char(0xAA)); r.append(char(0xAC)); r.append(char(1));
        uint32_t hz = 500000;
        r.append(char(hz&0xFF)); r.append(char((hz>>8)&0xFF));
        r.append(char((hz>>16)&0xFF)); r.append(char((hz>>24)&0xFF));
        Parser ps; ps.feed(r);
        auto msgs = ps.takeMessages();
        bool good = msgs.size()==1 && msgs[0].type==Message::Response &&
                    msgs[0].response.id==RSP_BITRATE_LOCK && msgs[0].response.value==500000;
        check(good, "解析 AA AC 波特率锁定响应");
    }

    // 10. 状态行
    {
        Parser ps; ps.feed(QByteArray("ST:bus=1 mode=S job=0 tmr=0 cur=1 pend=0 act=0 err=00000000\r"));
        auto msgs = ps.takeMessages();
        bool good = msgs.size()==1 && msgs[0].type==Message::StatusLine &&
                    msgs[0].text.startsWith("ST:bus=1");
        check(good, "解析 ASCII 状态行");
    }

    // 11. CRC 错误帧应被丢弃（ID 宽度有歧义，仅在 CRC 校验通过时上报）
    {
        QByteArray bad = f; bad[bad.size()-1] = char((uint8_t)bad[bad.size()-1] ^ 0xFF);
        Parser ps; ps.feed(bad);
        auto msgs = ps.takeMessages();
        bool anyFrame = false;
        for (auto &mm : msgs) if (mm.type == Message::CanFrame) anyFrame = true;
        check(!anyFrame, "CRC 错误帧被丢弃（不上报）");
    }

    // 12. AA AD 警告/报错帧（正常 / 坏 CRC / 跨包）
    {
        QByteArray body = QByteArray("E001");
        uint8_t cb = 0; for (int i = 0; i < body.size(); i++) {
            cb ^= (uint8_t)body[i];
            for (int b = 0; b < 8; b++) cb = (cb & 0x80) ? (uint8_t)((cb << 1) ^ 0x07) : (uint8_t)(cb << 1);
        }
        QByteArray log1; log1.append(char(0xAA)); log1.append(char(0xAD));
        log1 += body; log1.append(char(0xDA)); log1.append(char(0xAA)); log1.append(char(cb));
        {
            Parser ps; ps.feed(log1);
            auto msgs = ps.takeMessages();
            check(msgs.size() == 1 && msgs[0].type == Message::Log &&
                  msgs[0].text == QStringLiteral("E001") && msgs[0].crcOk,
                  "解析 AA AD 警告帧（正常 + CRC 通过）");
        }
        {
            QByteArray bad = log1; bad[bad.size() - 1] = char(0x00);   // 破坏 CRC
            Parser ps; ps.feed(bad);
            auto msgs = ps.takeMessages();
            check(msgs.size() == 1 && msgs[0].type == Message::Log &&
                  msgs[0].text == QStringLiteral("E001") && !msgs[0].crcOk,
                  "AA AD CRC 错误仍上报（crcOk=false）");
        }
        {
            Parser ps; ps.feed(log1.left(3)); ps.feed(log1.mid(3));
            auto msgs = ps.takeMessages();
            check(msgs.size() == 1 && msgs[0].type == Message::Log &&
                  msgs[0].text == QStringLiteral("E001"),
                  "AA AD 跨包分片解析");
        }
    }

    // 13. MCU 真实错误名上报：AA AD | ERR_USB_CRC | DA AA | CRC8（与固件 usb_proto_send_error 一致）
    {
        const QByteArray name = QByteArrayLiteral("ERR_USB_CRC");
        uint8_t cb = 0; for (int i = 0; i < name.size(); i++) {
            cb ^= (uint8_t)name[i];
            for (int b = 0; b < 8; b++) cb = (cb & 0x80) ? (uint8_t)((cb << 1) ^ 0x07) : (uint8_t)(cb << 1);
        }
        QByteArray fr; fr.append(char(0xAA)); fr.append(char(0xAD));
        fr += name; fr.append(char(0xDA)); fr.append(char(0xAA)); fr.append(char(cb));
        Parser ps; ps.feed(fr);
        auto msgs = ps.takeMessages();
        // 逐字节校验帧头/尾标（避免十六进制转义吞并：用数值比较）
        const bool hdrTailOk = fr.size() == 2 + name.size() + 3 &&
            (uint8_t)fr[0] == 0xAA && (uint8_t)fr[1] == 0xAD &&
            (uint8_t)fr[2 + name.size()] == 0xDA &&
            (uint8_t)fr[3 + name.size()] == 0xAA &&
            (uint8_t)fr[4 + name.size()] == cb;
        check(hdrTailOk, "AA AD 组帧: AA AD <text> DA AA <crc8>");
        check(msgs.size() == 1 && msgs[0].type == Message::Log &&
              msgs[0].text == QStringLiteral("ERR_USB_CRC") && msgs[0].crcOk,
              "MCU 错误名 ASCII 上报并被解析 (ERR_USB_CRC)");
    }

    // 13. AA AC 版本响应（id=2）：AA AC 02 | LEN | TEXT，分片喂入
    {
        QByteArray ver;
        const QByteArray vt = QByteArray("v20.0.2");
        ver.append(char(0xAA)); ver.append(char(0xAC)); ver.append(char(0x02));
        ver.append(char(vt.size())); ver += vt;
        Parser ps; ps.feed(ver.left(2)); ps.feed(ver.mid(2));
        auto msgs = ps.takeMessages();
        check(msgs.size() == 1 && msgs[0].type == Message::Response &&
              msgs[0].response.id == RSP_VERSION &&
              msgs[0].response.text == QStringLiteral("v20.0.2"),
              "AA AC 版本响应解析 (id=2)");
    }

    if (report) *report = log;
    return ok;
}

} // namespace proto
