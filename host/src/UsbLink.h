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
struct libusb_device;
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
//
// 用 libusb 的异步接口，同时挂 kUrbs 个 bulk IN 传输。同步 libusb_bulk_transfer
// 每次读都要等一个完整往返（实测 ~0.2 ms/次），而上行数据是短包连续到达的；
// 多 URB 常驻之后，一个 URB 在回调里交还数据的同时其余 URB 已经在等，
// 主机侧的往返延迟被摊掉，吞吐由总线速率而不是每请求延迟决定。
class UsbReader : public QThread
{
    Q_OBJECT
public:
    UsbReader(UsbLink *owner, libusb_device_handle *h, libusb_context *ctx);
    void stop() { m_stop = true; }
    // run() 退出时是否把所有异步传输都回收干净了（否则不能碰 libusb_close）
    bool cleanShutdown() const { return m_cleanShutdown; }
protected:
    void run() override;
private:
    // libusb 的类型（libusb_transfer / LIBUSB_CALL）不出现在本头文件里：
    // <libusb-1.0/libusb.h> 会污染其它翻译单元，故用不透明上下文封装。
    struct AsyncCtx;

    // 暂存缓冲的刷新策略：约 30 Hz，或攒到 64 KB 时立即送。
    // 一次快照对应一次跨线程事件，与帧率解耦。
    static constexpr int    kFlushIntervalMs   = 33;
    static constexpr qint64 kStagingFlushBytes = 64 * 1024;

    void flushStaging();

    UsbLink *m_owner;
    libusb_device_handle *m_h;
    libusb_context *m_usbCtx;      // 事件泵必须泵自己的上下文，不能用默认的
    volatile bool m_stop = false;
    volatile bool m_cleanShutdown = false;
    AsyncCtx *m_ctx = nullptr;

    QByteArray m_staging;            // 仅 reader 线程访问
    qint64     m_lastFlushMs = 0;
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
    // 等待待发数据真正写出去（防止"发完就 close"把 ENTER_DFU 丢掉）
    void flush() override;

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
    // 待发数据。所有 libusb 调用都必须发生在接收线程里：同步 libusb_bulk_transfer
    // 自身会处理事件，若由 UI 线程发起，就和接收线程的事件泵并发进 libusb，
    // 破坏其内部状态（实测 libusb-1.0.dll 0xc0000005）。因此 send() 只入队，
    // 由接收线程负责写出。
    QByteArray m_txPending;
};

#endif // USBLINK_H
