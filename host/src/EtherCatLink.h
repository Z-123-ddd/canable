// ============================================================================
//  EtherCatLink — EtherCAT 主站传输（基于内嵌 SOEM v2.0.0，Win32/WinPcap）
//
//  与串口/USB 不同：EtherCAT 不使用 AA AA/AA AB 字节协议，而是由 SOEM 直接
//  在网卡上收发 EtherCAT 帧。因此本类继承 Link 仅为统一“设备”抽象，
//  send()/字节协议解析不参与；对外提供主站操作：扫描网卡 / 扫描从站 /
//  过程数据收发 / CoE(SDO) 读写。
//
//  运行前提：主机需安装 Npcap（或 WinPcap，WinPcap-API 兼容模式）。
// ============================================================================
#ifndef ETHERCATLINK_H
#define ETHERCATLINK_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QVector>
#include "Link.h"

// 一张网卡（pcap 设备）
struct EcAdapterInfo {
    QString name;   // pcap 设备名（\Device\NPF_{GUID}），用于 ecx_init
    QString desc;   // 人类可读描述
};

// 一个从站
struct EcSlaveInfo {
    int      pos = 0;          // 1..slavecount
    QString  name;             // 由厂商/产品 ID 组成的可读名
    quint16  state = 0;        // 当前状态（EC_STATE_*）
    quint16  alStatus = 0;     // AL 状态码
    quint16  configAddr = 0;   // 配置地址
    quint32  vendorId = 0;
    quint32  productId = 0;
    quint16  obits = 0, ibits = 0;   // 输出/输入位
};

// 一条 PDO 映射项（CoE 对象索引/子索引 + 位长 + 在 PDO 内的位偏移）
struct EcPdoEntry {
    quint16 index = 0;    // 对象索引（如 0x6040 控制字）
    quint8  sub = 0;      // 子索引
    int     bitLen = 0;   // 位长（16/32/8...）
    int     bitOffset = 0;// 该对象在 PDO 内的位偏移（按映射顺序累加）
};

class EtherCatLink : public Link
{
    Q_OBJECT
public:
    // 从站状态（与 SOEM EC_STATE_* 对齐；供 UI 侧引用而不暴露 SOEM 头文件）
    enum EcState { ECST_INIT = 0x01, ECST_PREOP = 0x02, ECST_BOOT = 0x03,
                   ECST_SAFEOP = 0x04, ECST_OP = 0x08 };

    explicit EtherCatLink(QObject *parent = nullptr);
    ~EtherCatLink() override;

    // 扫描主机网卡（ec_find_adapters）
    static QVector<EcAdapterInfo> scanAdapters();
    // 抓包驱动（wpcap.dll/Packet.dll）是否可用
    static bool captureAvailable();

    QString kind() const override { return QStringLiteral("EtherCAT"); }
    bool    open(const QString &arg = QString()) override;   // arg = pcap 设备名
    void    close() override;
    bool    isOpen() const override { return m_open; }
    QString name() const override { return m_adapterName; }
    void    send(const QByteArray &bytes) override;          // 不使用（EtherCAT 无字节流）
    void    flush() override {}

    // ---- 主站操作 ----
    int  scanSlaves();                       // ecx_config_init → 返回从站数
    int  slaveCount() const { return m_slaveCount; }
    EcSlaveInfo slave(int pos) const;        // pos: 1..slaveCount
    bool mapProcessData();                   // ecx_config_map_group
    // 映射后配置过程数据：IgH 式 LWR+LRD（blockLRW）+ DC SYNC0（若从站支持）
    bool configureDc(QString *log = nullptr);
    void setUseDc(bool b) { m_useDc = b; }   // 便于排查：关闭 DC（SM/自由运行）
    quint16 slaveAlStatus(int pos) const;    // 从站 AL 状态码（寄存器 0x0134）
    bool requestStateOp(int timeoutMs = 5000);  // 请求并等待 OP

    int  sendProcessData();                  // ecx_send_processdata → 发送的字节数
    int  receiveProcessData(int timeoutUs);  // → WKC（工作计数器）
    int  expectedWkc() const { return m_expectedWkc; }
    const QByteArray &ioMap() const { return m_iomap; }  // 过程数据镜像

    // ---- CoE (SDO) ----
    bool sdoRead(int slave, quint16 index, quint8 sub,
                 QByteArray *out, QString *err, int timeoutUs = 1000000);
    bool sdoWrite(int slave, quint16 index, quint8 sub,
                  const QByteArray &data, QString *err, int timeoutUs = 1000000);

    // ---- EtherCAT 状态机（按规范：INIT->PRE-OP->SAFE-OP->OP；切 OP 期间需持续泵过程数据）----
    bool    requestState(quint16 target, int timeoutMs = 5000);
    quint16 currentState() const;
    quint16 slaveState(int pos) const;

    // ---- CoE PDO 映射解析（0x1C12/0x1C13 + 0x16xx/0x1Axx）----
    bool readPdoEntries(int slave, bool output, QVector<EcPdoEntry> *entries, QString *err);
    // 读/写一个 CoE 对象的无符号整数（1/2/4 字节，小端）；供 PDO 分配检查与写入
    bool sdoReadUInt(int slave, quint16 index, quint8 sub, int nbytes, quint32 *out, QString *err);
    bool sdoWriteUInt(int slave, quint16 index, quint8 sub, int nbytes, quint32 value, QString *err);

    // ---- 过程数据镜像访问（映射后有效）----
    bool writeRxBytes(int slave, int pdoByteOffset, const QByteArray &bytes);
    QByteArray readTxBytes(int slave, int pdoByteOffset, int len) const;
    int  rxPdoSizeBytes(int slave) const;
    int  txPdoSizeBytes(int slave) const;

    static QString stateName(quint16 state);

private:
    void *m_ctx = nullptr;         // ecx_contextt*
    bool  m_open = false;
    QString m_adapterName;
    int   m_slaveCount = 0;
    int   m_expectedWkc = 0;
    QByteArray m_iomap;            // 过程数据镜像（IOmap）
    bool    m_useDc = true;
    quint32 m_dcCycleNs = 2000000;   // SYNC0 周期（本关节默认 2ms）
    qint32  m_dcShiftNs = 250000;    // SYNC0 相移（本关节默认 250us）
    bool    m_mapped = false;
};

#endif // ETHERCATLINK_H
