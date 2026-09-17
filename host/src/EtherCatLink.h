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
    quint16  obits = 0, ibits = 0;
};

class EtherCatLink : public Link
{
    Q_OBJECT
public:
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

    static QString stateName(quint16 state);

private:
    void *m_ctx = nullptr;         // ecx_contextt*
    bool  m_open = false;
    QString m_adapterName;
    int   m_slaveCount = 0;
    int   m_expectedWkc = 0;
    QByteArray m_iomap;            // 过程数据镜像（IOmap）
};

#endif // ETHERCATLINK_H
