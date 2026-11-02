// ============================================================================
//  dfu_device.cpp — N32 出厂 DFU bootloader 回退烧录（libusb, DfuSe 风格）
//
//  ⚠ 仅作**回退**：主路径是厂商 NsMcuCmdDll.dll（见 src/VendorDfuFlasher.cpp）。
//    本文件要求 DFU 设备绑定 WinUSB/libusb 驱动，与厂商 NationstechDFU 驱动互斥。
//
//  协议与流程对齐权威参考（dfuse/DfuDevice.py）：
//    · 设备：VID 0x19F5 / PID 0x4537（"Nationstech DFU"，DFU 模式）——非 ST 的 0x0483:0xDF11
//    · 请求：DNLOAD=0x01 / CLRSTATUS=0x04 / GETSTATUS=0x03 / ABORT=0x06
//            bmRequestType=0x21(OUT) / 0xA1(IN)
//    · 特殊命令（DNLOAD wValue=block0）：
//        0x21 + addr(LE4)  = Set Address
//        0x41 + addr(LE4)  = Erase（擦除该地址所在扇区）
//        空数据             = Leave（离开并运行应用）
//    · 数据块：wValue 从 2 开始递增，块大小 = gcd(扇区长度, 2048)
//    · 扇区表：从 DFU alt-setting 的 iInterface 串解析
//        "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,03*128Kg"
// ============================================================================
#include "dfu_device.h"

#include <libusb-1.0/libusb.h>
#include <QFile>
#include <QThread>
#include <numeric>
#include <cstring>
#include <cstdlib>

namespace dfu {

// ---- DFU 类/请求 ----
static constexpr uint8_t DFU_CLASS_APP     = 0xFE;
static constexpr uint8_t DFU_SUBCLASS      = 0x01;
static constexpr uint8_t DFU_PROTO_DFUMODE = 0x02;

static constexpr uint8_t REQ_DNLOAD    = 0x01;
static constexpr uint8_t REQ_GETSTATUS = 0x03;
static constexpr uint8_t REQ_CLRSTATUS = 0x04;
static constexpr uint8_t REQ_ABORT     = 0x06;

static constexpr uint8_t BM_OUT = 0x21;   // host->dev, class, interface
static constexpr uint8_t BM_IN  = 0xA1;   // dev->host, class, interface

// DFU bState
static constexpr uint8_t ST_DFU_IDLE        = 2;
static constexpr uint8_t ST_DFU_DNLOAD_SYNC = 3;
static constexpr uint8_t ST_DFU_DNBUSY      = 4;
static constexpr uint8_t ST_DFU_DNLOAD_IDLE = 5;
static constexpr uint8_t ST_DFU_MANIFEST_SYNC = 6;
static constexpr uint8_t ST_DFU_MANIFEST    = 7;
static constexpr uint8_t ST_DFU_ERROR       = 10;

static constexpr int CTRL_TIMEOUT_MS = 5000;
static constexpr int MAX_TRANSFER_SIZE = 2048;

// ---------------------------------------------------------------------------
// 纯函数：DfuSe 命令编码 / 布局解析 / 扇区计算
// ---------------------------------------------------------------------------
QByteArray encodeSetAddress(quint32 addr)
{
    QByteArray b(5, 0);
    b[0] = char(0x21);
    b[1] = char(addr & 0xFF);
    b[2] = char((addr >> 8) & 0xFF);
    b[3] = char((addr >> 16) & 0xFF);
    b[4] = char((addr >> 24) & 0xFF);
    return b;
}

QByteArray encodeErase(quint32 addr)
{
    QByteArray b(5, 0);
    b[0] = char(0x41);
    b[1] = char(addr & 0xFF);
    b[2] = char((addr >> 8) & 0xFF);
    b[3] = char((addr >> 16) & 0xFF);
    b[4] = char((addr >> 24) & 0xFF);
    return b;
}

QList<Sector> parseLayout(const QString &altName, quint32 *baseaddr)
{
    QList<Sector> out;
    if (baseaddr) *baseaddr = 0;
    if (!altName.contains(QStringLiteral("Internal Flash")))
        return out;

    // "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,03*128Kg"
    const QStringList parts = altName.split(QLatin1Char('/'));
    if (parts.size() < 3)
        return out;

    bool ok = false;
    quint32 base = parts[1].trimmed().toUInt(&ok, 0);
    if (!ok)
        return out;
    if (baseaddr) *baseaddr = base;

    const QStringList toks = parts[2].split(QLatin1Char(','), Qt::SkipEmptyParts);
    quint32 addr = base;
    for (const QString &rawTok : toks) {
        const QString tok = rawTok.trimmed();
        const int star = tok.indexOf(QLatin1Char('*'));
        if (star <= 0)
            return {};
        bool okRep = false;
        const int repeat = tok.left(star).toInt(&okRep);
        if (!okRep || repeat <= 0)
            return {};

        QString spec = tok.mid(star + 1);
        int i = 0;
        while (i < spec.size() && spec.at(i).isDigit()) i++;
        if (i == 0)
            return {};
        const quint32 size = spec.left(i).toUInt();
        const QChar mult = (i < spec.size()) ? spec.at(i) : QLatin1Char(' ');
        quint32 mul = 1;
        if (mult == QLatin1Char('K') || mult == QLatin1Char('k')) mul = 1024;
        else if (mult == QLatin1Char('M') || mult == QLatin1Char('m')) mul = 1024 * 1024;

        for (int r = 0; r < repeat; r++) {
            out.append(Sector{ addr, size * mul });
            addr += size * mul;
        }
    }
    return out;
}

quint32 transferSize(quint32 sectorLen)
{
    return static_cast<quint32>(std::gcd<quint32>(sectorLen, MAX_TRANSFER_SIZE));
}

QList<Sector> sectorsCovering(const QList<Sector> &sectors, quint32 addr, quint32 len)
{
    QList<Sector> out;
    const quint32 end = addr + len;
    for (const Sector &s : sectors) {
        const quint32 sEnd = s.addr + s.len;
        if (s.addr < end && sEnd > addr)
            out.append(s);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 设备探测：找到 DFU 接口 + Internal Flash 的 alt setting + 扇区表
// ---------------------------------------------------------------------------
struct Probe {
    bool ok = false;
    bool isDfuClass = false;
    uint8_t iface = 0;
    uint8_t alt = 0;
    quint32 baseaddr = 0;
    QList<Sector> sectors;
    QString product;
    uint16_t vid = 0, pid = 0;
};

static QString readString(libusb_device_handle *h, uint8_t index)
{
    if (index == 0 || !h) return QString();
    unsigned char buf[256] = {0};
    if (libusb_get_string_descriptor_ascii(h, index, buf, sizeof(buf)) > 0)
        return QString::fromUtf8((const char *)buf);
    return QString();
}

// Dump the target DFU device's interfaces / alt-settings / strings (diagnostic)
QString describeDevice()
{
    QString out;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
        return QStringLiteral("libusb init fail\n");
    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;
        if (!(dd.idVendor == kDfuVid && dd.idProduct == kDfuPid))
            continue;
        libusb_device_handle *h = nullptr;
        bool opened = (libusb_open(list[i], &h) == 0);
        out += QStringLiteral("device %1:%2 %3  bcdDevice=0x%4\n")
                   .arg(dd.idVendor, 4, 16, QChar('0')).arg(dd.idProduct, 4, 16, QChar('0'))
                   .arg(opened ? "(opened)" : "(open FAILED)")
                   .arg(dd.bcdDevice, 0, 16);
        out += QStringLiteral("  iProduct='%1'\n").arg(opened ? readString(h, dd.iProduct) : QString());
        if (opened) {
            // DFU 功能描述符 (bDescriptorType=0x21): bmAttributes/wDetachTimeout/wTransferSize/bcdDFU
            unsigned char fd[9] = {0};
            int fr = libusb_get_descriptor(h, 0x21, 0, fd, sizeof(fd));
            if (fr < 9) {
                fr = libusb_control_transfer(h, 0x81, 0x06, 0x2100, 0, fd, sizeof(fd), CTRL_TIMEOUT_MS);
            }
            if (fr >= 9) {
                out += QStringLiteral("  DFU func: bmAttr=0x%1 wDetachTimeout=%2 wTransferSize=%3 bcdDFU=0x%4\n")
                           .arg(fd[2], 2, 16, QChar('0'))
                           .arg(fd[3] | (fd[4] << 8))
                           .arg(fd[5] | (fd[6] << 8))
                           .arg(fd[7] | (fd[8] << 8), 0, 16);
            } else {
                out += QStringLiteral("  DFU func: read failed (%1)\n").arg(fr);
            }
        }
        struct libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_active_config_descriptor(list[i], &cfg) == 0) {
            for (uint8_t c = 0; c < cfg->bNumInterfaces; c++) {
                const struct libusb_interface &itf = cfg->interface[c];
                for (int a = 0; a < itf.num_altsetting; a++) {
                    const struct libusb_interface_descriptor &d = itf.altsetting[a];
                    QString istr = opened ? readString(h, d.iInterface) : QString();
                    out += QStringLiteral("  iface %1 alt %2  cls=%3 sub=%4 proto=%5  iInterface='%6'\n")
                               .arg(d.bInterfaceNumber).arg(d.bAlternateSetting)
                               .arg(d.bInterfaceClass, 2, 16, QChar('0'))
                               .arg(d.bInterfaceSubClass, 2, 16, QChar('0'))
                               .arg(d.bInterfaceProtocol, 2, 16, QChar('0'))
                               .arg(istr);
                    for (uint8_t e = 0; e < d.bNumEndpoints; e++) {
                        const struct libusb_endpoint_descriptor &ep = d.endpoint[e];
                        out += QStringLiteral("      ep 0x%1 attr=0x%2 maxpkt=%3\n")
                                   .arg(ep.bEndpointAddress, 2, 16, QChar('0'))
                                   .arg(ep.bmAttributes, 2, 16, QChar('0'))
                                   .arg(ep.wMaxPacketSize);
                    }
                }
            }
            libusb_free_config_descriptor(cfg);
        }
        if (opened) libusb_close(h);
        break;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

static Probe probeDevice(libusb_device *dev, bool needVidPid)
{
    Probe p;
    struct libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(dev, &dd) != 0)
        return p;
    p.vid = dd.idVendor;
    p.pid = dd.idProduct;

    const bool isTarget = (dd.idVendor == kDfuVid && dd.idProduct == kDfuPid);

    struct libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
        return p;

    // 先判断是否 DFU 类接口，并找 Internal Flash 的 alt setting（需打开设备读字符串）
    bool hasDfuClass = false;
    uint8_t dfuIface = 0, dfuAlt = 0;
    bool haveFlashAlt = false;

    libusb_device_handle *h = nullptr;
    const bool opened = (libusb_open(dev, &h) == 0);

    for (uint8_t ci = 0; ci < cfg->bNumInterfaces; ci++) {
        const struct libusb_interface &itf = cfg->interface[ci];
        for (int a = 0; a < itf.num_altsetting; a++) {
            const struct libusb_interface_descriptor &d = itf.altsetting[a];
            if (d.bInterfaceClass == DFU_CLASS_APP &&
                d.bInterfaceSubClass == DFU_SUBCLASS &&
                d.bInterfaceProtocol == DFU_PROTO_DFUMODE &&
                !hasDfuClass) {
                hasDfuClass = true;
                dfuIface = d.bInterfaceNumber;
                dfuAlt = d.bAlternateSetting;
            }
            if (opened && !haveFlashAlt) {
                const QString s = readString(h, d.iInterface);
                if (s.contains(QStringLiteral("Internal Flash"))) {
                    quint32 base = 0;
                    const QList<Sector> secs = parseLayout(s, &base);
                    if (!secs.isEmpty()) {
                        p.iface = d.bInterfaceNumber;
                        p.alt = d.bAlternateSetting;
                        p.baseaddr = base;
                        p.sectors = secs;
                        haveFlashAlt = true;
                    }
                }
            }
        }
    }

    if (opened) {
        if (p.product.isEmpty() && dd.iProduct)
            p.product = readString(h, dd.iProduct);
        libusb_close(h);
    }
    libusb_free_config_descriptor(cfg);

    p.isDfuClass = hasDfuClass;
    if (!p.product.isEmpty())
        p.product = p.product;

    // 匹配策略：目标 VID/PID 优先；否则退回 DFU 类
    const bool matched = isTarget || (!needVidPid && hasDfuClass);
    if (!matched)
        return Probe{};   // ok=false

    // 若未解析到 Internal Flash（例如驱动未装、读不到字符串），仍视为 DFU 设备，
    // 但接口用 DFU 类的接口；扇区表留空（烧录时再报错）。
    if (!haveFlashAlt) {
        p.iface = dfuIface;
        p.alt = dfuAlt;
    }
    p.ok = true;
    return p;
}

QMutex &libusbGuard()
{
    static QMutex m;
    return m;
}

Info findDevice()
{
    QMutexLocker lock(&libusbGuard());
    Info info;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
        return info;

    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        Probe p = probeDevice(list[i], /*needVidPid=*/false);
        if (!p.ok)
            continue;
        info.found = true;
        info.vid = p.vid;
        info.pid = p.pid;
        info.bus = libusb_get_bus_number(list[i]);
        info.address = libusb_get_device_address(list[i]);
        info.interface = p.iface;
        info.altSetting = p.alt;
        info.product = p.product;
        break;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return info;
}

QString scanDevices()
{
    QString out;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
        return QStringLiteral("libusb 初始化失败\n");

    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        libusb_device *dev = list[i];
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(dev, &dd) != 0)
            continue;

        Probe p = probeDevice(dev, /*needVidPid=*/false);
        QString cls;
        if (p.isDfuClass)
            cls += QStringLiteral(" [DFU]");
        if (dd.idVendor == kDfuVid && dd.idProduct == kDfuPid)
            cls += QStringLiteral(" [N32-BOOTLOADER]");
        if (!p.sectors.isEmpty())
            cls += QStringLiteral(" [Internal Flash: %1 扇区 @0x%2]")
                       .arg(p.sectors.size())
                       .arg(p.baseaddr, 8, 16, QLatin1Char('0'));

        out += QStringLiteral("bus %1 addr %2  %3:%4%5  %6\n")
                   .arg(libusb_get_bus_number(dev))
                   .arg(libusb_get_device_address(dev))
                   .arg(dd.idVendor, 4, 16, QLatin1Char('0'))
                   .arg(dd.idProduct, 4, 16, QLatin1Char('0'))
                   .arg(cls)
                   .arg(p.product);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

// ---------------------------------------------------------------------------
// 打开 DFU 设备（用给定 context，避免跨线程持有句柄）
// ---------------------------------------------------------------------------
static libusb_device_handle *openDfu(libusb_context *ctx, Probe *out, QString *err)
{
    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle *h = nullptr;
    for (ssize_t i = 0; i < n && !h; i++) {
        Probe p = probeDevice(list[i], /*needVidPid=*/false);
        if (!p.ok)
            continue;
        if (libusb_open(list[i], &h) == 0) {
            *out = p;
            break;
        }
    }
    if (list) libusb_free_device_list(list, 1);
    if (!h && err)
        *err = QStringLiteral("未找到可打开的 DFU 设备（0483:DF11）。"
                              "Windows 下需用 Zadig 给 'STM32 BOOTLOADER' 装 WinUSB/libusb 驱动。");
    return h;
}

// ---------------------------------------------------------------------------
// DFU 基础操作
// ---------------------------------------------------------------------------
static int ctrlOut(libusb_device_handle *h, uint8_t req, uint16_t value, uint8_t iface,
                   const unsigned char *data, uint16_t len)
{
    return libusb_control_transfer(h, BM_OUT, req, value, iface,
                                   (unsigned char *)data, len, CTRL_TIMEOUT_MS);
}

// GETSTATUS；等待直到不在 states 列表（含 BUSY 处理）；返回 0 且置 outState
static int waitWhileState(libusb_device_handle *h, uint8_t iface,
                          const QList<uint8_t> &states, QString *err, uint8_t *outState)
{
    for (int guard = 0; guard < 200000; guard++) {
        unsigned char s[6] = {0};
        int r = libusb_control_transfer(h, BM_IN, REQ_GETSTATUS, 0, iface,
                                        s, sizeof(s), CTRL_TIMEOUT_MS);
        if (r < 0) {
            if (err) *err = QStringLiteral("DFU_GETSTATUS 失败: %1").arg(libusb_error_name(r));
            return -1;
        }
        const uint8_t bStatus = s[0];
        const uint32_t poll = (uint32_t)s[1] | ((uint32_t)s[2] << 8) | ((uint32_t)s[3] << 16);
        const uint8_t bState = s[4];
        if (bStatus != 0) {
            if (err) *err = QStringLiteral("DFU 状态错误: bStatus=%1 bState=%2").arg(bStatus).arg(bState);
            return -1;
        }
        if (!states.contains(bState)) {
            if (outState) *outState = bState;
            return 0;
        }
        if (bState == ST_DFU_DNBUSY)
            QThread::msleep(poll ? poll : 1);
    }
    if (err) *err = QStringLiteral("DFU_GETSTATUS 轮询超时");
    return -1;
}

static int dnload(libusb_device_handle *h, uint16_t block, uint8_t iface,
                  const unsigned char *data, uint16_t len)
{
    return ctrlOut(h, REQ_DNLOAD, block, iface, data, len);
}

static int clrStatus(libusb_device_handle *h, uint8_t iface)
{
    return ctrlOut(h, REQ_CLRSTATUS, 0, iface, nullptr, 0);
}

static int abortReq(libusb_device_handle *h, uint8_t iface)
{
    return ctrlOut(h, REQ_ABORT, 0, iface, nullptr, 0);
}

// 设定地址并回到 IDLE（对齐 ODrive set_address_safe）
static bool setAddressSafe(libusb_device_handle *h, uint8_t iface, quint32 addr, QString *err)
{
    const QByteArray cmd = encodeSetAddress(addr);
    if (dnload(h, 0, iface, (const unsigned char *)cmd.constData(), (uint16_t)cmd.size()) < 0) {
        if (err) *err = QStringLiteral("设定地址传输失败");
        return false;
    }
    uint8_t st = 0;
    if (waitWhileState(h, iface, { ST_DFU_DNBUSY }, err, &st) != 0)
        return false;
    if (st != ST_DFU_DNLOAD_IDLE) {
        if (err) *err = QStringLiteral("设定地址后状态异常: %1").arg(st);
        return false;
    }
    abortReq(h, iface);
    if (waitWhileState(h, iface, { ST_DFU_DNLOAD_SYNC }, err, &st) != 0)
        return false;
    if (st != ST_DFU_IDLE) {
        if (err) *err = QStringLiteral("ABORT 后未回到 IDLE: %1").arg(st);
        return false;
    }
    return true;
}

static bool eraseSector(libusb_device_handle *h, uint8_t iface, quint32 addr,
                        quint32 sectorLen, QString *err)
{
    const QByteArray cmd = encodeErase(addr);
    if (dnload(h, 0, iface, (const unsigned char *)cmd.constData(), (uint16_t)cmd.size()) < 0) {
        if (err) *err = QStringLiteral("擦除传输失败");
        return false;
    }
    uint8_t st = 0;
    if (waitWhileState(h, iface, { ST_DFU_DNBUSY }, err, &st) != 0)
        return false;
    if (st != ST_DFU_DNLOAD_IDLE) {
        if (err) *err = QStringLiteral("擦除后状态异常: %1（地址 0x%2）")
                            .arg(st).arg(addr, 8, 16, QLatin1Char('0'));
        return false;
    }
    Q_UNUSED(sectorLen);
    return true;
}

// ---------------------------------------------------------------------------
// 烧录
// ---------------------------------------------------------------------------
bool flashBin(const QString &binPath, quint32 addr, QString *err,
              const std::function<void(int, const QString &)> &progress)
{
    auto report = [&](int pct, const QString &stage) {
        if (progress) progress(pct, stage);
    };

    QFile f(binPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法打开固件文件: %1").arg(binPath);
        return false;
    }
    const QByteArray fw = f.readAll();
    f.close();
    if (fw.isEmpty()) {
        if (err) *err = QStringLiteral("固件文件为空");
        return false;
    }

    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) { if (err) *err = QStringLiteral("libusb 初始化失败"); return false; }

    QString e;
    Probe probe;
    libusb_device_handle *h = openDfu(ctx, &probe, &e);
    if (!h) { if (err) *err = e; libusb_exit(ctx); return false; }

    bool ok = true;
    const uint8_t iface = probe.iface;
    do {
        int r = libusb_claim_interface(h, iface);
        if (r != 0) { ok = false; if (err) *err = QStringLiteral("无法声明 DFU 接口: %1").arg(libusb_error_name(r)); break; }

        // 复位到 IDLE
        clrStatus(h, iface);
        waitWhileState(h, iface, { ST_DFU_DNBUSY }, nullptr, nullptr);

        if (probe.sectors.isEmpty()) {
            // 无 DfuSe 布局串，但器件很可能是 DfuSe 兼容：按 DfuSe 命令序列
            //   block0 = [0x21, addr(LE4)]  设定地址
            //   数据块从 block 2 开始
            const QByteArray seta = encodeSetAddress(addr);
            report(1, QStringLiteral("设定地址 0x%1").arg(addr, 8, 16, QLatin1Char('0')));
            int rr = dnload(h, 0, iface, (const unsigned char *)seta.constData(), (uint16_t)seta.size());
            if (rr < 0) { ok = false; if (err) *err = QStringLiteral("DfuSe 设定地址失败: %1").arg(libusb_error_name(rr)); break; }
            uint8_t st = 0;
            if (waitWhileState(h, iface, { ST_DFU_DNBUSY }, err, &st) != 0) { ok = false; break; }
            abortReq(h, iface);
            waitWhileState(h, iface, { ST_DFU_DNLOAD_SYNC }, nullptr, nullptr);

            const int chunk = 1024;
            int block = 2;
            qint64 sent = 0;
            while (sent < fw.size()) {
                const int len = (int)qMin<qint64>(chunk, fw.size() - sent);
                report(2 + (int)(sent * 96 / fw.size()),
                       QStringLiteral("下载 %1/%2 字节").arg(sent + len).arg(fw.size()));
                rr = dnload(h, (uint16_t)block, iface,
                            (const unsigned char *)(fw.constData() + sent), (uint16_t)len);
                if (rr < 0) {
                    ok = false;
                    if (err) *err = QStringLiteral("DfuSe 下载失败 @%1: %2").arg(sent).arg(libusb_error_name(rr));
                    break;
                }
                if (waitWhileState(h, iface, { ST_DFU_DNBUSY }, err, &st) != 0) { ok = false; break; }
                sent += len;
                block++;
            }
            if (!ok) break;

            report(99, QStringLiteral("重启设备"));
            dnload(h, 0, iface, nullptr, 0);   // block0 空数据 → Leave
            waitWhileState(h, iface, { ST_DFU_DNBUSY }, nullptr, &st);
            libusb_release_interface(h, iface);
            report(100, QStringLiteral("完成"));
            break;
        }

        // 覆盖镜像的扇区
        const QList<Sector> cover = sectorsCovering(probe.sectors, addr, (quint32)fw.size());
        if (cover.isEmpty()) {
            ok = false;
            if (err) *err = QStringLiteral("镜像地址 0x%1 不在内部 Flash 范围内")
                                .arg(addr, 8, 16, QLatin1Char('0'));
            break;
        }

        // 1) 擦除各扇区
        for (const Sector &s : cover) {
            report(1, QStringLiteral("擦除扇区 0x%1").arg(s.addr, 8, 16, QLatin1Char('0')));
            libusb_set_interface_alt_setting(h, iface, probe.alt);
            if (!eraseSector(h, iface, s.addr, s.len, err)) { ok = false; break; }
        }
        if (!ok) break;

        // 2) 逐扇区写入
        qint64 written = 0;
        for (const Sector &s : cover) {
            const quint32 sStart = qMax(probe.baseaddr, s.addr);
            const quint32 sEndAll = s.addr + s.len;
            const quint32 imgEnd = addr + (quint32)fw.size();
            const quint32 sStartData = qMax(s.addr, addr);
            const quint32 sEndData = qMin(sEndAll, imgEnd);
            if (sEndData <= sStartData)
                continue;

            libusb_set_interface_alt_setting(h, iface, probe.alt);
            if (!setAddressSafe(h, iface, sStartData, err)) { ok = false; break; }

            const quint32 ts = transferSize(s.len);
            const char *p = fw.constData() + (sStartData - addr);
            quint32 remain = sEndData - sStartData;
            uint16_t block = 2;
            while (remain > 0) {
                const quint32 len = qMin(ts, remain);
                int rr = dnload(h, block, iface, (const unsigned char *)p, (uint16_t)len);
                if (rr < 0) { ok = false; if (err) *err = QStringLiteral("数据下载失败 @0x%1: %2")
                                        .arg(sEndData - remain, 8, 16, QLatin1Char('0'))
                                        .arg(libusb_error_name(rr)); break; }
                uint8_t st = 0;
                if (waitWhileState(h, iface, { ST_DFU_DNBUSY }, err, &st) != 0) { ok = false; break; }
                if (st != ST_DFU_DNLOAD_IDLE) {
                    ok = false;
                    if (err) *err = QStringLiteral("写入后状态异常: %1").arg(st);
                    break;
                }
                p += len; remain -= len; block++;
                written += len;
                report(2 + (int)(written * 96 / qMax<qint64>(1, fw.size())),
                       QStringLiteral("下载 %1/%2 字节").arg(written).arg(fw.size()));
            }
            if (!ok) break;
            Q_UNUSED(sStart);
        }
        if (!ok) break;

        // 3) 离开并运行应用
        report(99, QStringLiteral("重启设备"));
        dnload(h, 0, iface, nullptr, 0);
        uint8_t st = 0;
        if (waitWhileState(h, iface, { ST_DFU_MANIFEST_SYNC }, err, &st) != 0) {
            // 部分实现离开后不回 MANIFEST，属正常（设备已复位），不判失败
        }

        libusb_release_interface(h, iface);
        report(100, QStringLiteral("完成"));
    } while (0);

    libusb_close(h);
    libusb_exit(ctx);
    return ok;
}

// 逆向探针：按 Nations BOOT 协议（CMD 经 DNLOAD 下发，应答经 GETSTATUS 取回）
QString probeDfu()
{
    QString out;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) return QStringLiteral("libusb init fail\n");
    QString e; Probe probe;
    libusb_device_handle *h = openDfu(ctx, &probe, &e);
    if (!h) { libusb_exit(ctx); return QStringLiteral("open fail: %1\n").arg(e); }
    const uint8_t iface = probe.iface;
    libusb_claim_interface(h, iface);

    auto dn = [&](const unsigned char *d, int n, const char *tag) {
        int r = libusb_control_transfer(h, BM_OUT, 0x01, 0, iface, (unsigned char *)d, (uint16_t)n, CTRL_TIMEOUT_MS);
        out += QStringLiteral("%1 DNLOAD(%2) r=%3\n").arg(tag).arg(n).arg(r);
    };
    auto gs = [&](const char *tag) {
        unsigned char buf[64] = {0};
        int r = libusb_control_transfer(h, BM_IN, 0x03, 0, iface, buf, 64, CTRL_TIMEOUT_MS);
        QString hex;
        for (int i = 0; i < (r > 0 ? r : 0); i++)
            hex += QStringLiteral("%1 ").arg(buf[i], 2, 16, QChar('0'));
        out += QStringLiteral("%1 GETSTATUS r=%2 : %3\n").arg(tag).arg(r).arg(hex);
    };

    gs("idle");

    // CMD_GET_INF: CMD_H=0x10 CMD_L=0x00 LEN=0x0000 Par=0x00000000
    unsigned char getinf[8] = {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    dn(getinf, 8, "CMD_GET_INF");
    unsigned char none[1] = {0};
    dn(none, 0, "DNLOAD(0)");
    gs("after-cmd-zerolen");
    int cr = libusb_control_transfer(h, BM_OUT, 0x04, 0, iface, none, 0, CTRL_TIMEOUT_MS);
    out += QStringLiteral("CLRSTATUS r=%1\n").arg(cr);
    gs("after-clr");
    gs("getinf-rsp");
    for (int len : {6, 32, 57, 64}) {
        unsigned char buf[128] = {0};
        int r = libusb_control_transfer(h, BM_IN, 0x03, 0, iface, buf, len, CTRL_TIMEOUT_MS);
        QString hex;
        for (int i = 0; i < (r > 0 ? r : 0); i++)
            hex += QStringLiteral("%1 ").arg(buf[i], 2, 16, QChar('0'));
        out += QStringLiteral("getinf len=%1 r=%2 : %3\n").arg(len).arg(r).arg(hex);
    }
    // 试 UPLOAD(bRequest=2) 取应答
    for (int len : {8, 57, 64}) {
        unsigned char buf[128] = {0};
        int r = libusb_control_transfer(h, BM_IN, 0x02, 0, iface, buf, len, CTRL_TIMEOUT_MS);
        QString hex;
        for (int i = 0; i < (r > 0 ? r : 0); i++)
            hex += QStringLiteral("%1 ").arg(buf[i], 2, 16, QChar('0'));
        out += QStringLiteral("upload len=%1 r=%2 : %3\n").arg(len).arg(r).arg(hex);
    }

    libusb_release_interface(h, iface);
    libusb_close(h);
    libusb_exit(ctx);
    return out;
}

} // namespace dfu
