#include "UsbLink.h"

#include <libusb-1.0/libusb.h>
#include <QMetaObject>
#include <QDebug>
#include <algorithm>

static constexpr int    EP_OUT = 0x01;    // 设备数据 OUT（host -> device）
static constexpr int    EP_IN  = 0x81;    // 设备数据 IN （device -> host）
static constexpr int    TIMEOUT = 200;    // ms

// 读取设备的字符串描述符（尽力而为，失败返回空）
static QString readString(libusb_device_handle *h, uint8_t index)
{
    if (index == 0 || !h)
        return QString();
    unsigned char buf[256];
    int n = libusb_get_string_descriptor_ascii(h, index, buf, sizeof(buf));
    if (n <= 0)
        return QString();
    return QString::fromUtf8((const char *)buf, n);
}

// 找到含 Bulk 端点的接口号（CDC data 接口通常为 1）
static int findBulkInterface(libusb_device *dev, int *outIfaces, int maxIfaces)
{
    struct libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
        return -1;
    int count = 0;
    for (uint8_t c = 0; c < cfg->bNumInterfaces; c++) {
        const struct libusb_interface &it = cfg->interface[c];
        for (int a = 0; a < it.num_altsetting; a++) {
            const struct libusb_interface_descriptor &d = it.altsetting[a];
            bool bulk = false;
            for (uint8_t e = 0; e < d.bNumEndpoints; e++)
                if ((d.endpoint[e].bmAttributes & 0x03) == 0x02) bulk = true;
            if (bulk && count < maxIfaces)
                outIfaces[count++] = d.bInterfaceNumber;
        }
    }
    libusb_free_config_descriptor(cfg);
    return count;
}

// ---------------------------------------------------------------------------
// UsbReader（后台接收线程）
// ---------------------------------------------------------------------------
UsbReader::UsbReader(UsbLink *owner, libusb_device_handle *h)
    : m_owner(owner), m_h(h) {}

void UsbReader::run()
{
    unsigned char buf[4096];
    while (!m_stop) {
        int transferred = 0;
        int r = libusb_bulk_transfer(m_h, EP_IN, buf, sizeof(buf), &transferred, TIMEOUT);
        if (r == 0 && transferred > 0) {
            QByteArray data((const char *)buf, transferred);
            QMetaObject::invokeMethod(m_owner, [this, data]() { m_owner->onBulkIn(data); },
                                      Qt::QueuedConnection);
        } else if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) {
            break;   // 设备拔出/断链
        }
        // 超时/其它：继续
    }
}

// ---------------------------------------------------------------------------
// UsbLink
// ---------------------------------------------------------------------------
UsbLink::UsbLink(QObject *parent) : Link(parent) {}

UsbLink::~UsbLink() { close(); }

QList<UsbDeviceInfo> UsbLink::scanDevices()
{
    QList<UsbDeviceInfo> out;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
        return out;

    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;

        // 仅保留含 Bulk 端点者（可作数据链路），过滤根 hub/HID/摄像头等
        int ifaces[8];
        if (findBulkInterface(list[i], ifaces, 8) <= 0)
            continue;

        UsbDeviceInfo info;
        info.bus = libusb_get_bus_number(list[i]);
        info.address = libusb_get_device_address(list[i]);
        info.vid = dd.idVendor;
        info.pid = dd.idProduct;

        // 读取字符串需打开设备（失败则只留 VID:PID）
        libusb_device_handle *h = nullptr;
        if (libusb_open(list[i], &h) == 0) {
            info.product = readString(h, dd.iProduct);
            info.serial = readString(h, dd.iSerialNumber);
            libusb_close(h);
        }
        out.append(info);
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);

    // 把本应用固件设备(16D0:117E)排到最前，方便默认选中
    std::stable_sort(out.begin(), out.end(), [](const UsbDeviceInfo &a, const UsbDeviceInfo &b) {
        const bool ka = (a.vid == UsbLink::kVid && a.pid == UsbLink::kPid);
        const bool kb = (b.vid == UsbLink::kVid && b.pid == UsbLink::kPid);
        return ka && !kb;
    });
    return out;
}

bool UsbLink::devicePresent()
{
    for (const UsbDeviceInfo &d : scanDevices())
        if (d.vid == kVid && d.pid == kPid)
            return true;
    return false;
}

bool UsbLink::open(const QString &arg)
{
    if (m_open)
        return true;

    if (libusb_init(&m_ctx) != 0) {
        emit errorOccurred(QStringLiteral("libusb 初始化失败"));
        return false;
    }

    QString target = arg.trimmed();   // "bus:addr"，空则取第一个

    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(m_ctx, &list);
    libusb_device_handle *handle = nullptr;
    int iface = 1;
    QString openedName;
    for (ssize_t i = 0; i < n && !handle; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;

        const QString id = QStringLiteral("%1:%2")
                               .arg(libusb_get_bus_number(list[i]))
                               .arg(libusb_get_device_address(list[i]));
        if (!target.isEmpty() && id != target)
            continue;

        libusb_device_handle *h = nullptr;
        if (libusb_open(list[i], &h) != 0)
            continue;

        // 逐个含 Bulk 端点的接口尝试 claim（允许自动分离内核驱动）
        int ifaces[8];
        int cnt = findBulkInterface(list[i], ifaces, 8);
        libusb_set_auto_detach_kernel_driver(h, 1);
        bool claimed = false;
        for (int k = 0; k < cnt && !claimed; k++) {
            if (libusb_claim_interface(h, ifaces[k]) == 0) {
                iface = ifaces[k];
                claimed = true;
            }
        }
        if (!claimed) {
            libusb_close(h);
            continue;
        }

        handle = h;
        QString prod = readString(h, dd.iProduct);
        openedName = QStringLiteral("%1:%2").arg(dd.idVendor, 4, 16, QChar('0'))
                                             .arg(dd.idProduct, 4, 16, QChar('0'));
        if (!prod.isEmpty())
            openedName += QStringLiteral("  ") + prod;
        openedName += QStringLiteral("  [%1]").arg(id);
    }
    if (list) libusb_free_device_list(list, 1);

    if (!handle) {
        emit errorOccurred(target.isEmpty()
                               ? QStringLiteral("未找到可用的 USB(libusb) 设备")
                               : QStringLiteral("无法打开 USB 设备 [%1]（驱动需为 libusb）").arg(target));
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    m_handle = handle;
    m_iface = iface;
    m_name = openedName;
    resetParser();
    m_open = true;
    m_reader = new UsbReader(this, m_handle);
    m_reader->start();

    emit opened(m_name);
    return true;
}

void UsbLink::close()
{
    if (m_reader) {
        m_reader->stop();
        m_reader->wait(1000);
        delete m_reader;
        m_reader = nullptr;
    }
    if (m_handle) {
        libusb_release_interface(m_handle, m_iface);
        libusb_close(m_handle);
        m_handle = nullptr;
    }
    if (m_ctx) {
        libusb_exit(m_ctx);
        m_ctx = nullptr;
    }
    if (m_open) {
        m_open = false;
        emit closed();
    }
}

void UsbLink::send(const QByteArray &bytes)
{
    if (!m_open || !m_handle || bytes.isEmpty())
        return;

    QMutexLocker lock(&m_txMutex);
    int offset = 0;
    const int total = bytes.size();
    while (offset < total) {
        int chunk = qMin(4096, total - offset);
        int transferred = 0;
        int r = libusb_bulk_transfer(m_handle, EP_OUT,
                                     (unsigned char *)(bytes.constData() + offset),
                                     chunk, &transferred, TIMEOUT);
        if (r < 0) {
            emit errorOccurred(QStringLiteral("USB 发送失败: %1").arg(libusb_error_name(r)));
            break;
        }
        offset += transferred;
    }
    emit rawTx(bytes);
}

void UsbLink::onBulkIn(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    feedParser(data);
}
