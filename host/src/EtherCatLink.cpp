// ============================================================================
//  EtherCatLink — EtherCAT 主站（SOEM）实现
// ============================================================================
#include "EtherCatLink.h"

#include <cstring>
#include <cstdio>
#include <QThread>
#include <QElapsedTimer>

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

    // 1) 回到 PRE-OP：PDO 分配与 DC 只能在 PRE-OP 配置
    ctx->slavelist[0].state = EC_STATE_PRE_OP;
    ecx_writestate(ctx, 0);
    ecx_statecheck(ctx, 0, EC_STATE_PRE_OP, 500000);

    // 2) 在 PRE-OP 先武装 DC SYNC0（部分驱动要求 SAFE_OP 请求前 SYNC0 已就绪）
    bool anyDc = false;
    for (int i = 1; i <= m_slaveCount; ++i) if (ctx->slavelist[i].hasdc) anyDc = true;
    if (m_useDc && anyDc) {
        for (int i = 1; i <= m_slaveCount; ++i) {
            if (!ctx->slavelist[i].hasdc) continue;
            ecx_dcsync0(ctx, (uint16)i, TRUE, (uint32)m_dcCycleNs, (int32)m_dcShiftNs);
            (void)ecx_FPWRw(&ctx->port, ctx->slavelist[i].configadr, 0x0980,
                            htoes(0x0300), EC_TIMEOUTRET);
        }
    }

    // 3) 映射过程数据
    m_iomap.fill('\0');
    ecx_config_map_group(ctx, m_iomap.data(), 0);
    m_expectedWkc = ctx->grouplist[0].outputsWKC * 2 + ctx->grouplist[0].inputsWKC;

    // 4) IgH 式分离读写（LWR+LRD）：部分关节 ESC 固件错误处理 LRW
    for (int i = 1; i <= m_slaveCount; ++i) ctx->slavelist[i].blockLRW = 1;
    ctx->slavelist[0].blockLRW = 1;
    ctx->grouplist[0].blockLRW = 1;

    // 5) DC 链 + SyncType（映射后）
    configureDc(nullptr);

    m_mapped = true;
    return true;
}

// 映射后配置 DC：ecx_configdc + 每从站 ecx_dcsync0 + AssignActivate(0x0980) + SyncType(0x1C32/0x1C33)
// 顺序须为：configdc -> dcsync0 -> 写 1C32 SyncType（否则 AL 0x0030）。仅在工作线程调用（阻塞 SDO）。
bool EtherCatLink::configureDc(QString *log)
{
    if (!m_open || m_slaveCount <= 0) return false;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);

    bool anyDc = false;
    for (int i = 1; i <= m_slaveCount; ++i) if (ctx->slavelist[i].hasdc) anyDc = true;

    if (!anyDc || !m_useDc) {
        // 非 DC：显式清零 AssignActivate，避免残留 0x0300 让从站一直等 SYNC0
        for (int i = 1; i <= m_slaveCount; ++i)
            (void)ecx_FPWRw(&ctx->port, ctx->slavelist[i].configadr, 0x0980, htoes(0x0000), EC_TIMEOUTRET);
        if (log) *log = QStringLiteral("无 DC 从站（AssignActivate=0）");
        return true;
    }

    if (!ecx_configdc(ctx)) {
        if (log) *log = QStringLiteral("ecx_configdc 失败（无 DC 能力从站）");
        return false;
    }
    for (int i = 1; i <= m_slaveCount; ++i) {
        if (!ctx->slavelist[i].hasdc) continue;
        ecx_dcsync0(ctx, (uint16)i, TRUE, (uint32)m_dcCycleNs, (int32)m_dcShiftNs);
        (void)ecx_FPWRw(&ctx->port, ctx->slavelist[i].configadr, 0x0980, htoes(0x0300), EC_TIMEOUTRET);
        QString e;
        // SyncType = 2 (DC SYNC0)；先 arm SYNC0（上面）再写 SyncType
        sdoWriteUInt(i, 0x1C32, 0x01, 2, 2, &e);
        sdoWriteUInt(i, 0x1C33, 0x01, 2, 2, &e);
        sdoWriteUInt(i, 0x1C32, 0x02, 4, (quint32)m_dcCycleNs, &e);
    }
    if (log) *log = QStringLiteral("DC 已配置：cycle=%1ns shift=%2ns").arg(m_dcCycleNs).arg(m_dcShiftNs);
    return true;
}

quint16 EtherCatLink::slaveAlStatus(int pos) const
{
    if (!m_ctx || !m_open || pos < 1 || pos > m_slaveCount) return 0;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    return ctx->slavelist[pos].ALstatuscode;
}

bool EtherCatLink::requestStateOp(int timeoutMs)
{
    return requestState(EC_STATE_OPERATIONAL, timeoutMs);
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


// ---- 状态机：按 EtherCAT 规范切换；切 SAFE-OP/OP 期间持续泵过程数据以促成迁移 ----
bool EtherCatLink::requestState(quint16 target, int timeoutMs)
{
    if (!m_open || m_slaveCount <= 0) return false;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);

    // 若尚未映射，先映射并配置 DC（进入 OP 的前提）
    if (!m_mapped) mapProcessData();

    // 按真实经过时间限界（而非固定迭代次数），并在此期间持续泵过程数据：
    // 从站需要过程数据更新才能从 SAFE-OP 迁移到 OP。
    auto pump = [&](quint16 st, int toMs) -> bool {
        ctx->slavelist[0].state = st;              // 广播（0=全部从站）
        ecx_writestate(ctx, 0);
        QElapsedTimer t; t.start();
        while (t.elapsed() <= toMs) {
            ecx_send_processdata(ctx);
            ecx_receive_processdata(ctx, 1000);
            ecx_statecheck(ctx, 0, st, 1000);
            if ((ctx->slavelist[0].state & 0x0F) == (st & 0x0F)) return true;
            QThread::msleep(2);
        }
        return (ctx->slavelist[0].state & 0x0F) == (st & 0x0F);
    };

    if (target == EC_STATE_OPERATIONAL) {
        if ((ctx->slavelist[0].state & 0x0F) == EC_STATE_OPERATIONAL) return true;
        if ((ctx->slavelist[0].state & 0x0F) != EC_STATE_SAFE_OP) {
            pump(EC_STATE_SAFE_OP, timeoutMs / 2);   // 规范顺序：先 SAFE-OP
            // SAFE-OP 下先跑若干周期“预热”过程数据（DC/驱动器需要几个周期才就绪）
            for (int k = 0; k < 20; ++k) {
                ecx_send_processdata(ctx);
                ecx_receive_processdata(ctx, 1000);
                QThread::msleep(1);
            }
        }
        return pump(EC_STATE_OPERATIONAL, timeoutMs); // 再 OP
    }
    return pump(target, timeoutMs);
}

quint16 EtherCatLink::currentState() const
{
    if (!m_ctx || !m_open) return 0;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    return ctx->slavelist[0].state;
}

quint16 EtherCatLink::slaveState(int pos) const
{
    if (!m_ctx || !m_open || pos < 1 || pos > m_slaveCount) return 0;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    return ctx->slavelist[pos].state;
}

// 读一个 CoE 对象的无符号整数（小端）
static bool sdoReadUIntRaw(ecx_contextt *ctx, int slave, quint16 index, quint8 sub,
                        int nbytes, quint32 *out, QString *err)
{
    char buf[8] = {0};
    int size = nbytes;
    int rc = ecx_SDOread(ctx, (uint16)slave, (uint16)index, (uint8)sub, FALSE, &size, buf, 1000000);
    if (rc <= 0) {
        if (err) *err = QStringLiteral("SDO 读 0x%1:%2 失败(rc=%3)")
                            .arg(index, 4, 16, QLatin1Char('0')).arg(sub).arg(rc);
        return false;
    }
    quint32 v = 0;
    for (int i = 0; i < size && i < 4; ++i)
        v |= (quint32)(quint8)buf[i] << (8 * i);
    *out = v;
    return true;
}


bool EtherCatLink::sdoReadUInt(int slave, quint16 index, quint8 sub, int nbytes, quint32 *out, QString *err)
{
    if (!m_open || slave < 1 || slave > m_slaveCount) { if (err) *err = QStringLiteral("从站号越界"); return false; }
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    return sdoReadUIntRaw(ctx, slave, index, sub, nbytes, out, err);
}

bool EtherCatLink::sdoWriteUInt(int slave, quint16 index, quint8 sub, int nbytes, quint32 value, QString *err)
{
    if (!m_open || slave < 1 || slave > m_slaveCount) { if (err) *err = QStringLiteral("从站号越界"); return false; }
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    QByteArray b(nbytes, '\0');
    for (int i = 0; i < nbytes; ++i) b[i] = char((value >> (8 * i)) & 0xFF);
    const int rc = ecx_SDOwrite(ctx, (uint16)slave, (uint16)index, (uint8)sub, FALSE,
                                b.size(), b.constData(), 1000000);
    if (rc <= 0) {
        if (err) *err = QStringLiteral("SDO 写 0x%1:%2 失败(rc=%3)")
                            .arg(index, 4, 16, QLatin1Char('0')).arg(sub).arg(rc);
        return false;
    }
    return true;
}

// 解析 RxPDO(output=true)/TxPDO(output=false)：0x1C12/0x1C13 分配 + 0x16xx/0x1Axx 映射
bool EtherCatLink::readPdoEntries(int slave, bool output, QVector<EcPdoEntry> *entries, QString *err)
{
    if (entries) entries->clear();
    if (!m_open || slave < 1 || slave > m_slaveCount) { if (err) *err = QStringLiteral("从站号越界"); return false; }
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);

    const quint16 assignIndex = output ? 0x1C12 : 0x1C13;
    quint32 count = 0;
    if (!sdoReadUIntRaw(ctx, slave, assignIndex, 0, 1, &count, err)) return false;
    if (count == 0) {
        if (err) *err = QStringLiteral("未分配 PDO（0x%1:00=0）").arg(assignIndex, 4, 16, QLatin1Char('0'));
        return false;
    }

    int bitOffset = 0;
    bool any = false;
    for (quint32 i = 1; i <= count; ++i) {
        quint32 pdoIdx = 0;
        if (!sdoReadUIntRaw(ctx, slave, assignIndex, (quint8)i, 2, &pdoIdx, err)) return false;
        if (pdoIdx == 0) continue;

        quint32 n = 0;
        if (!sdoReadUIntRaw(ctx, slave, (quint16)pdoIdx, 0, 1, &n, err)) return false;
        for (quint32 k = 1; k <= n; ++k) {
            quint32 entry = 0;
            if (!sdoReadUIntRaw(ctx, slave, (quint16)pdoIdx, (quint8)k, 4, &entry, err)) return false;
            const quint16 oidx = (quint16)(entry >> 16);
            const quint8  osub = (quint8)((entry >> 8) & 0xFF);
            const int     blen = (int)(entry & 0xFF);
            if (oidx == 0 && osub == 0 && blen == 0) continue;   // 占位/填充
            EcPdoEntry e;
            e.index = oidx; e.sub = osub; e.bitLen = blen; e.bitOffset = bitOffset;
            if (entries) entries->append(e);
            bitOffset += blen;
            any = true;
        }
    }
    if (!any && err) *err = QStringLiteral("PDO 映射为空");
    return any;
}

int EtherCatLink::rxPdoSizeBytes(int slave) const
{
    if (!m_ctx || slave < 1 || slave > m_slaveCount) return 0;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    return (ctx->slavelist[slave].Obits + 7) / 8;
}

int EtherCatLink::txPdoSizeBytes(int slave) const
{
    if (!m_ctx || slave < 1 || slave > m_slaveCount) return 0;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    return (ctx->slavelist[slave].Ibits + 7) / 8;
}

bool EtherCatLink::writeRxBytes(int slave, int off, const QByteArray &bytes)
{
    if (!m_open || slave < 1 || slave > m_slaveCount) return false;
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    uint8 *base = ctx->slavelist[slave].outputs;
    if (!base) return false;
    const int total = rxPdoSizeBytes(slave);
    if (off < 0 || off + bytes.size() > total) return false;
    std::memcpy(base + off, bytes.constData(), bytes.size());
    return true;
}

QByteArray EtherCatLink::readTxBytes(int slave, int off, int len) const
{
    if (!m_open || slave < 1 || slave > m_slaveCount) return QByteArray();
    ecx_contextt *ctx = static_cast<ecx_contextt *>(m_ctx);
    const uint8 *base = ctx->slavelist[slave].inputs;
    if (!base) return QByteArray();
    const int total = txPdoSizeBytes(slave);
    if (off < 0 || len <= 0 || off + len > total) return QByteArray();
    return QByteArray(reinterpret_cast<const char *>(base + off), len);
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
