// ============================================================================
//  EtherCatLink — EtherCAT 主站（SOEM）实现
// ============================================================================
#include "EtherCatLink.h"

#include <cstring>
#include <cstdio>

// SOEM（C）——只在本 .cpp 内引入，避免污染 Qt 侧头文件
extern "C" {
#include "soem/soem.h"
}

// 过程数据镜像大小（字节）
static const int kIoMapSize = 4096;

EtherCatLink::EtherCatLink(QObject *parent) : Link(parent)
{
    m_iomap.resize(kIoMapSize);
    m_iomap.fill('\0');
}

EtherCatLink::~EtherCatLink()
{
    close();
    if (m_ctx) { delete static_cast<ecx_contextt *>(m_ctx); m_ctx = nullptr; }
}

// ---- 抓包驱动是否可用（尝试枚举；失败或为空即视为不可用/未安装）----
bool EtherCatLink::captureAvailable()
{
    ec_adaptert *head = ec_find_adapters();
    const bool any = (head != nullptr);
    ec_free_adapters(head);
    return any;
}

QVector<EcAdapterInfo> EtherCatLink::scanAdapters()
{
    QVector<EcAdapterInfo> out;
    ec_adaptert *head = ec_find_adapters();
    for (ec_adaptert *a = head; a != nullptr; a = a->next) {
        EcAdapterInfo i;
        i.name = QString::fromLocal8Bit(a->name);
        i.desc = QString::fromLocal8Bit(a->desc);
        out.append(i);
    }
    ec_free_adapters(head);
    return out;
}

bool EtherCatLink::open(const QString &arg)
{
    if (m_open) return true;
    if (!m_ctx) m_ctx = new (std::nothrow) ecx_contextt;
    if (!m_ctx) { emit errorOccurred(QStringLiteral("内存不足：无法分配 SOEM 上下文")); return false; }

    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    std::memset(ctx, 0, sizeof(*ctx));

    const QByteArray ifname = arg.toLocal8Bit();
    if (ecx_init(ctx, ifname.constData()) <= 0) {
        emit errorOccurred(QStringLiteral("打开网卡失败：%1（需安装 Npcap/WinPcap 并以管理员运行）")
                               .arg(arg));
        return false;
    }
    m_adapterName = arg;
    m_open = true;
    m_slaveCount = 0;
    m_expectedWkc = 0;
    emit opened(arg);
    return true;
}

void EtherCatLink::close()
{
    if (!m_open) return;
    if (m_ctx) {
        ecx_close(static_cast<ecx_contextt *>(m_ctx));
        std::memset(m_ctx, 0, sizeof(ecx_contextt));
    }
    m_open = false;
    m_slaveCount = 0;
    m_expectedWkc = 0;
    emit closed();
}

void EtherCatLink::send(const QByteArray &bytes)
{
    // EtherCAT 不使用字节流协议；忽略（保留接口以统一 Link 抽象）
    Q_UNUSED(bytes);
}

int EtherCatLink::scanSlaves()
{
    if (!m_open) return -1;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    m_slaveCount = ecx_config_init(ctx);
    if (m_slaveCount < 0) m_slaveCount = 0;
    return m_slaveCount;
}

QString EtherCatLink::stateName(quint16 state)
{
    switch (state & 0x0F) {
    case EC_STATE_INIT:         return QStringLiteral("INIT");
    case EC_STATE_PRE_OP:       return QStringLiteral("PRE-OP");
    case EC_STATE_BOOT:         return QStringLiteral("BOOT");
    case EC_STATE_SAFE_OP:      return QStringLiteral("SAFE-OP");
    case EC_STATE_OPERATIONAL:  return QStringLiteral("OP");
    default:                    return QStringLiteral("?");
    }
}

EcSlaveInfo EtherCatLink::slave(int pos) const
{
    EcSlaveInfo s;
    if (!m_ctx || pos < 1 || pos > m_slaveCount) return s;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    const ec_slavet &sl = ctx->slavelist[pos];
    s.pos = pos;
    s.name = QString::fromLocal8Bit(sl.name);
    if (s.name.isEmpty())
        s.name = QStringLiteral("Vendor 0x%1 / Product 0x%2")
                     .arg(sl.eep_man, 8, 16, QLatin1Char('0'))
                     .arg(sl.eep_id, 8, 16, QLatin1Char('0'))
                     .toUpper();
    s.state = sl.state;
    s.alStatus = sl.ALstatuscode;
    s.configAddr = sl.configadr;
    s.vendorId = sl.eep_man;
    s.productId = sl.eep_id;
    s.obits = sl.Obits;
    s.ibits = sl.Ibits;
    return s;
}

bool EtherCatLink::mapProcessData()
{
    if (!m_open || m_slaveCount <= 0) return false;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    m_iomap.fill('\0');
    ecx_config_map_group(ctx, m_iomap.data(), 0);
    // 期望 WKC = 从站数（每站 +1），组映射后由 SOEM 给出 grouplist[0].outputsWKC 等
    m_expectedWkc = ctx->grouplist[0].outputsWKC * 2 + ctx->grouplist[0].inputsWKC;
    return true;
}

bool EtherCatLink::requestStateOp(int timeoutMs)
{
    if (!m_open || m_slaveCount <= 0) return false;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    ctx->slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(ctx, 0);
    ecx_statecheck(ctx, 0, EC_STATE_OPERATIONAL, timeoutMs * 1000);
    return ctx->slavelist[0].state == EC_STATE_OPERATIONAL;
}

int EtherCatLink::sendProcessData()
{
    if (!m_open) return -1;
    return ecx_send_processdata(static_cast<ecx_contextt *>(m_ctx));
}

int EtherCatLink::receiveProcessData(int timeoutUs)
{
    if (!m_open) return -1;
    return ecx_receive_processdata(static_cast<ecx_contextt *>(m_ctx), timeoutUs);
}

bool EtherCatLink::sdoRead(int slaveNo, quint16 index, quint8 sub,
                           QByteArray *out, QString *err, int timeoutUs)
{
    if (!m_open) { if (err) *err = QStringLiteral("未打开网卡"); return false; }
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    if (slaveNo < 1 || slaveNo > m_slaveCount) { if (err) *err = QStringLiteral("从站号越界"); return false; }
    char buf[256] = {0};
    int size = sizeof(buf);
    int rc = ecx_SDOread(ctx, (uint16)slaveNo, (uint16)index, (uint8)sub,
                         FALSE, &size, buf, timeoutUs);
    if (rc <= 0) { if (err) *err = QStringLiteral("SDO 读失败（rc=%1）").arg(rc); return false; }
    if (out) *out = QByteArray(buf, size);
    return true;
}

bool EtherCatLink::sdoWrite(int slaveNo, quint16 index, quint8 sub,
                            const QByteArray &data, QString *err, int timeoutUs)
{
    if (!m_open) { if (err) *err = QStringLiteral("未打开网卡"); return false; }
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    if (slaveNo < 1 || slaveNo > m_slaveCount) { if (err) *err = QStringLiteral("从站号越界"); return false; }
    int rc = ecx_SDOwrite(ctx, (uint16)slaveNo, (uint16)index, (uint8)sub,
                          FALSE, data.size(), data.constData(), timeoutUs);
    if (rc <= 0) { if (err) *err = QStringLiteral("SDO 写失败（rc=%1）").arg(rc); return false; }
    return true;
}
