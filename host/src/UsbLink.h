// ============================================================================
//  UsbLink — USB(libusb) 传输
//    设备以 libusb 驱动接管（非 CDC 串口）时使用。走 Bulk 端点：
//      OUT = 0x01（主机->设备），IN = 0x81（设备->主机）
//    接收：后台线程循环 libusb_bulk_transfer(0x81)，喂给协议解析器。
//    发送：主线程 libusb_bulk_transfer(0x01)，与接收线程各自独立端点。
//    Windows 上 libusb 仅枚举到已装 WinUSB/libusbK 驱动的设备，
//    通常即我们的设备；可能存在多个，故提供列表供选择。
// ============================================================================
#ifndef USBLINK_H
#define USBLINK_H

#include <QThread>
#include <QMutex>
#include <QString>
#include <QList>
#include "Link.h"

struct libusb_context;
struct libusb_device_handle;

// 一个可用的 libusb 设备描述
struct UsbDeviceInfo
{
    int bus = 0;
    int address = 0;
    quint16 vid = 0;
    quint16 pid = 0;
    QString product;
    QString serial;
    // 打开时使用的标识 "bus:addr"
    QString id() const { return QStringLiteral("%1:%2").arg(bus).arg(address); }
    // 供下拉框显示的文本
    QString label() const
    {
        QString s = QStringLiteral("USB %1:%2")
                        .arg(vid, 4, 16, QChar('0'))
                        .arg(pid, 4, 16, QChar('0'));
        if (!product.isEmpty())
            s += QStringLiteral("  %1").arg(product);
        s += QStringLiteral("  [%1]").arg(id());
        return s;
    }
};

class UsbLink;

// 接收线程
class UsbReader : public QThread
{
    Q_OBJECT
public:
    UsbReader(UsbLink *owner, libusb_device_handle *h);
    void stop() { m_stop = true; }
protected:
    void run() override;
private:
    UsbLink *m_owner;
    libusb_device_handle *m_h;
    volatile bool m_stop = false;
};

class UsbLink : public Link
{
    Q_OBJECT
public:
    explicit UsbLink(QObject *parent = nullptr);
    ~UsbLink() override;

    // 应用固件 CAN 设备的默认 VID/PID（用于日志提示；枚举本身列出全部）
    static constexpr uint16_t kVid = 0x16D0;
    static constexpr uint16_t kPid = 0x117E;

    // 枚举全部可用 libusb 设备
    static QList<UsbDeviceInfo> scanDevices();
    // 是否存在默认设备（16D0:117E）
    static bool devicePresent();

    QString kind() const override { return QStringLiteral("USB"); }
    // arg = "bus:addr"；为空则取第一个设备
    bool open(const QString &arg = QString()) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    QString name() const override { return m_name; }
    void send(const QByteArray &bytes) override;

    // 供接收线程回调
    void onBulkIn(const QByteArray &data);

private:
    friend class UsbReader;
    libusb_context *m_ctx = nullptr;
    libusb_device_handle *m_handle = nullptr;
    int m_iface = 1;
    volatile bool m_open = false;
    QString m_name;
    UsbReader *m_reader = nullptr;
    QMutex m_txMutex;
};

#endif // USBLINK_H
