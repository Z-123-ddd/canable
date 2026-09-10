// ============================================================================
//  dfu_device.cpp — libusb 实现（DfuSe 风格）
// ============================================================================
#include "dfu_device.h"

#include <libusb-1.0/libusb.h>
#include <QFile>
#include <QThread>
#include <cstring>

namespace dfu {

// ---- DFU 类/请求 ----
static constexpr uint8_t DFU_CLASS_APP   = 0xFE;
static constexpr uint8_t DFU_SUBCLASS    = 0x01;
static constexpr uint8_t DFU_PROTO_DFUMODE = 0x02;

static constexpr uint8_t REQ_DNLOAD   = 0x01;
static constexpr uint8_t REQ_GETSTATUS= 0x03;
static constexpr uint8_t REQ_CLRSTATUS= 0x04;
static constexpr uint8_t REQ_ABORT    = 0x06;

static constexpr uint8_t BM_OUT = 0x21;   // host->dev, class, interface
static constexpr uint8_t BM_IN  = 0xA1;   // dev->host, class, interface

// DFU 状态
static constexpr uint8_t ST_DFU_IDLE    = 2;
static constexpr uint8_t ST_DFU_DNBUSY  = 4;
static constexpr uint8_t ST_DFU_DNLOAD_IDLE = 5;
static constexpr uint8_t ST_DFU_MANIFEST = 7;
static constexpr uint8_t ST_DFU_ERROR   = 10;

static constexpr int CTRL_TIMEOUT_MS = 5000;

// ---------------------------------------------------------------------------
// 设备匹配：VID/PID 优先；否则按 DFU 类描述符
// ---------------------------------------------------------------------------
static int findIfaceOfDevice(libusb_device *dev, QString *product,
                             uint16_t *outVid, uint16_t *outPid, bool *isDfu)
{
    *isDfu = false;
    struct libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(dev, &dd) != 0)
        return -1;

    if (outVid) *outVid = dd.idVendor;
    if (outPid) *outPid = dd.idProduct;

    // VID/PID 指定时先过滤
    bool vidpidSet = (kDfuVid != 0 || kDfuPid != 0);
    if (vidpidSet && (dd.idVendor != kDfuVid || dd.idProduct != kDfuPid))
        return -1;

    // 读产品字符串
    if (product && dd.iProduct) {
        libusb_device_handle *h = nullptr;
        if (libusb_open(dev, &h) == 0) {
            unsigned char buf[256] = {0};
            if (libusb_get_string_descriptor_ascii(h, dd.iProduct, buf, sizeof(buf)) > 0)
                *product = QString::fromUtf8((const char *)buf);
            libusb_close(h);
        }
    }

    struct libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
        return -1;

    int iface = -1;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && iface < 0; i++) {
        const struct libusb_interface &itf = cfg->interface[i];
        for (int a = 0; a < itf.num_altsetting; a++) {
            const struct libusb_interface_descriptor &d = itf.altsetting[a];
            if (d.bInterfaceClass == DFU_CLASS_APP &&
                d.bInterfaceSubClass == DFU_SUBCLASS &&
                d.bInterfaceProtocol == DFU_PROTO_DFUMODE) {
                iface = d.bInterfaceNumber;
                *isDfu = true;
                break;
            }
        }
    }
    libusb_free_config_descriptor(cfg);

    // 指定了 VID/PID 但类不是 DFU 也接受（有些 bootloader 用厂商类）
    if (iface < 0 && vidpidSet)
        *isDfu = true, iface = 0;

    return iface;
}

Info findDevice()
{
    Info info;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
        return info;

    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        uint16_t vid = 0, pid = 0;
        bool isDfu = false;
        QString product;
        int iface = findIfaceOfDevice(list[i], &product, &vid, &pid, &isDfu);
        if (iface >= 0 && isDfu) {
            info.found = true;
            info.vid = vid;
            info.pid = pid;
            info.bus = libusb_get_bus_number(list[i]);
            info.address = libusb_get_device_address(list[i]);
            info.interface = (uint8_t)iface;
            info.product = product;
            break;
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return info;
}

// 枚举所有 USB 设备，列出 总线/地址 VID:PID 类 描述（读 DFU VID/PID 用）
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

        QString product;
        if (dd.iProduct) {
            libusb_device_handle *h = nullptr;
            if (libusb_open(dev, &h) == 0) {
                unsigned char buf[256] = {0};
                if (libusb_get_string_descriptor_ascii(h, dd.iProduct, buf, sizeof(buf)) > 0)
                    product = QString::fromUtf8((const char *)buf);
                libusb_close(h);
            }
        }

        // 记录接口类（若有 DFU 类则标注）
        QString cls;
        struct libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_active_config_descriptor(dev, &cfg) == 0) {
            for (uint8_t c = 0; c < cfg->bNumInterfaces; c++) {
                const struct libusb_interface &itf = cfg->interface[c];
                for (int a = 0; a < itf.num_altsetting; a++) {
                    const struct libusb_interface_descriptor &d = itf.altsetting[a];
                    if (d.bInterfaceClass == DFU_CLASS_APP &&
                        d.bInterfaceSubClass == DFU_SUBCLASS &&
                        d.bInterfaceProtocol == DFU_PROTO_DFUMODE)
                        cls += QStringLiteral(" [DFU]");
                }
            }
            libusb_free_config_descriptor(cfg);
        }

        out += QStringLiteral("bus %1 addr %2  %3:%4%5  %6\n")
                   .arg(libusb_get_bus_number(dev))
                   .arg(libusb_get_device_address(dev))
                   .arg(dd.idVendor, 4, 16, QLatin1Char('0'))
                   .arg(dd.idProduct, 4, 16, QLatin1Char('0'))
                   .arg(cls)
                   .arg(product);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

// ---------------------------------------------------------------------------
// 打开匹配到的设备（重新枚举，避免跨线程持有句柄）
// ---------------------------------------------------------------------------
static libusb_device_handle *openDfu(uint8_t *ifaceOut, QString *err)
{
    libusb_device **list = nullptr;
    libusb_device_handle *h = nullptr;
    ssize_t n = libusb_get_device_list(nullptr, &list);   // 用默认 context

    for (ssize_t i = 0; i < n; i++) {
        bool isDfu = false;
        int iface = findIfaceOfDevice(list[i], nullptr, nullptr, nullptr, &isDfu);
        if (iface < 0 || !isDfu)
            continue;
        if (libusb_open(list[i], &h) == 0) {
            *ifaceOut = (uint8_t)iface;
            break;
        }
    }
    if (list) libusb_free_device_list(list, 1);
    if (!h && err) *err = QStringLiteral("未找到可打开的 DFU 设备");
    return h;
}

// ---------------------------------------------------------------------------
// DFU 基础操作
// ---------------------------------------------------------------------------
static int dfuGetStatus(libusb_device_handle *h, uint8_t iface,
                        QString *err, uint8_t *stateOut = nullptr)
{
    for (int guard = 0; guard < 10000; guard++) {
        unsigned char s[6] = {0};
        int r = libusb_control_transfer(h, BM_IN, REQ_GETSTATUS, 0, iface,
                                        s, sizeof(s), CTRL_TIMEOUT_MS);
        if (r < 0) { if (err) *err = QStringLiteral("DFU_GETSTATUS 失败: %1").arg(libusb_error_name(r)); return r; }
        uint8_t status = s[0];
        uint32_t poll = s[1] | (s[2] << 8) | (s[3] << 16);
        uint8_t state = s[4];
        if (stateOut) *stateOut = state;
        if (status != 0) {           // 非 0 = 出错
            if (err) *err = QStringLiteral("DFU 状态错误: bStatus=%1 bState=%2").arg(status).arg(state);
            return -1;
        }
        if (state == ST_DFU_DNBUSY) {           // 忙：等待
            QThread::msleep(poll ? poll : 1);
            // 读取一次即可解除 busy
            unsigned char t[6];
            libusb_control_transfer(h, BM_IN, REQ_GETSTATUS, 0, iface, t, sizeof(t), CTRL_TIMEOUT_MS);
            continue;
        }
        if (state == ST_DFU_ERROR) {            // 清错误
            libusb_control_transfer(h, BM_OUT, REQ_CLRSTATUS, 0, iface, nullptr, 0, CTRL_TIMEOUT_MS);
            if (err) *err = QStringLiteral("DFU 进入错误态，已清除");
            return -1;
        }
        return 0;   // dfuIDLE / dfuDNLOAD-IDLE / dfuMANIFEST 等
    }
    if (err) *err = QStringLiteral("DFU_GETSTATUS 轮询超时");
    return -1;
}

static int dfuDnload(libusb_device_handle *h, uint16_t block, uint8_t iface,
                     const unsigned char *data, uint16_t len)
{
    return libusb_control_transfer(h, BM_OUT, REQ_DNLOAD, block, iface,
                                   (unsigned char *)data, len, CTRL_TIMEOUT_MS);
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
    uint8_t iface = 0;
    libusb_device_handle *h = openDfu(&iface, &e);
    if (!h) { if (err) *err = e; libusb_exit(ctx); return false; }

    bool ok = true;
    do {
        // 声明接口
        int r = libusb_claim_interface(h, iface);
        if (r != 0) { ok = false; *err = QStringLiteral("无法声明 DFU 接口: %1").arg(libusb_error_name(r)); break; }

        // 复位到 IDLE
        libusb_control_transfer(h, BM_OUT, REQ_CLRSTATUS, 0, iface, nullptr, 0, CTRL_TIMEOUT_MS);
        dfuGetStatus(h, iface, nullptr);

        // 设定地址（DfuSe：block 0，命令字节 0x21 + 地址 LE）
        {
            unsigned char cmd[5] = { 0x21,
                (unsigned char)(addr & 0xFF), (unsigned char)((addr >> 8) & 0xFF),
                (unsigned char)((addr >> 16) & 0xFF), (unsigned char)((addr >> 24) & 0xFF) };
            report(0, QStringLiteral("设定地址 0x%1").arg(addr, 8, 16, QLatin1Char('0')));
            r = dfuDnload(h, 0, iface, cmd, sizeof(cmd));
            if (r != (int)sizeof(cmd)) { ok = false; *err = QStringLiteral("设定地址失败: %1").arg(libusb_error_name(r)); break; }
            if (dfuGetStatus(h, iface, err) != 0) { ok = false; break; }
        }

        // 擦除（DfuSe：block 0，命令字节 0x41 + 地址 + 长度=0 全擦）
        {
            unsigned char cmd[5] = { 0x41,
                (unsigned char)(addr & 0xFF), (unsigned char)((addr >> 8) & 0xFF),
                (unsigned char)((addr >> 16) & 0xFF), (unsigned char)((addr >> 24) & 0xFF) };
            report(2, QStringLiteral("擦除"));
            r = dfuDnload(h, 0, iface, cmd, sizeof(cmd));
            if (r != (int)sizeof(cmd)) { ok = false; *err = QStringLiteral("擦除失败: %1").arg(libusb_error_name(r)); break; }
            if (dfuGetStatus(h, iface, err) != 0) { ok = false; break; }
        }

        // 分块下载（block 从 2 开始）
        const int chunk = 1024;
        uint16_t block = 2;
        qint64 sent = 0;
        while (sent < fw.size()) {
            int len = (int)qMin<qint64>(chunk, fw.size() - sent);
            report(2 + (int)(sent * 96 / fw.size()), QStringLiteral("下载 %1/%2 字节").arg(sent + len).arg(fw.size()));
            r = dfuDnload(h, block, iface,
                          (const unsigned char *)(fw.constData() + sent), (uint16_t)len);
            if (r < 0) { ok = false; *err = QStringLiteral("下载失败 @%1: %2").arg(sent).arg(libusb_error_name(r)); break; }
            if (dfuGetStatus(h, iface, err) != 0) { ok = false; break; }
            sent += len;
            block++;
        }
        if (!ok) break;

        // 离开（block 0，零长度）→ 设备重启运行新固件
        report(99, QStringLiteral("重启设备"));
        dfuDnload(h, 0, iface, nullptr, 0);
        uint8_t st = 0;
        dfuGetStatus(h, iface, nullptr, &st);

        libusb_release_interface(h, iface);
        report(100, QStringLiteral("完成"));
    } while (0);

    libusb_close(h);
    libusb_exit(ctx);
    return ok;
}

} // namespace dfu
