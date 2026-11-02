#include "UsbLink.h"
#include "dfu_device.h"

#include <libusb-1.0/libusb.h>
#include <QMetaObject>
#include <QDebug>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <QDateTime>
#include <QElapsedTimer>

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
UsbReader::UsbReader(UsbLink *owner, libusb_device_handle *h, libusb_context *ctx)
    : m_owner(owner), m_h(h), m_usbCtx(ctx) {}

// 异步接收上下文：所有 libusb 类型只在这里出现。
struct UsbReader::AsyncCtx
{
    static constexpr int kUrbs    = 8;      // 并发挂起的 bulk IN 传输数
    static constexpr int kBufSize = 4096;

    libusb_transfer *urbs[kUrbs] = {};
    unsigned char   *bufs[kUrbs] = {};
    UsbReader       *self = nullptr;
    /* 已提交、尚未在回调里回收的传输数。
     * libusb_cancel_transfer() 只是"排队取消"，回调之后才真正归还；
     * 取消后立刻 libusb_free_transfer()/delete ctx 会让回调解引用已释放内存
     * （设备一进 bootloader 立刻断开时必现 -> 闪退）。所以必须先把手上的
     * 传输全部回收干净，再释放。 */
    std::atomic<int> pending { 0 };

    static void LIBUSB_CALL cb(libusb_transfer *t);
    void submit(int i);
    void rearm(libusb_transfer *t);
};

void LIBUSB_CALL UsbReader::AsyncCtx::cb(libusb_transfer *t)
{
    // libusb 在事件线程里回调：这里只“交还数据 + 立刻重新挂起”，
    // 解析丢给 Qt 队列，避免在 USB 回调里做重活。
    auto *ctx = static_cast<AsyncCtx *>(t->user_data);
    if (!ctx)
        return;
    UsbReader *self = ctx->self;
    if (!self)
        return;

    // 无论后续怎么处理，这次传输都已经回到我们手里 —— 先记账，
    // run() 的收尾循环靠它判断"全部回收完毕"。
    if (ctx->pending > 0)
        --ctx->pending;

    if (self->m_stop)
        return;

    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        self->m_stop = true;
        return;
    }

    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0) {
        /* Append to a staging buffer and only wake the main thread occasionally.
         *
         * Posting one queued event per USB packet does not scale: a busy bus
         * delivers ~8000 packets/s, i.e. 8000 cross-thread events/s, each
         * carrying a tiny QByteArray. The main thread cannot drain that and the
         * UI freezes. Instead the reader thread accumulates bytes and flushes
         * at most every kFlushIntervalMs (or when the staging buffer is large),
         * so the main thread sees a few dozen events per second regardless of
         * the frame rate. */
        self->m_staging.append((const char *)t->buffer, t->actual_length);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (self->m_staging.size() >= kStagingFlushBytes ||
            now - self->m_lastFlushMs >= kFlushIntervalMs) {
            self->flushStaging();
        }
    } else if (t->status == LIBUSB_TRANSFER_STALL) {
        libusb_clear_halt(self->m_h, EP_IN);
    }

    ctx->rearm(t);
}

void UsbReader::AsyncCtx::submit(int i)
{
    if (libusb_submit_transfer(urbs[i]) != 0) {
        self->m_stop = true;      // 设备拔出/带宽不足：交给 run() 收尾
        return;
    }
    ++pending;
}

void UsbReader::AsyncCtx::rearm(libusb_transfer *t)
{
    if (self->m_stop)
        return;
    if (libusb_submit_transfer(t) != 0)
        self->m_stop = true;
}

/* Hand the accumulated bytes to the main thread as one event.  Called from the
 * reader thread only. */
void UsbReader::flushStaging()
{
    if (m_staging.isEmpty())
        return;
    QByteArray data;
    data.swap(m_staging);                 // 交出所有权，不复制
    m_lastFlushMs = QDateTime::currentMSecsSinceEpoch();
    UsbLink *owner = m_owner;
    QMetaObject::invokeMethod(owner, [owner, data]() { owner->onBulkIn(data); },
                              Qt::QueuedConnection);
}


void UsbReader::run()
{
    m_cleanShutdown = false;
    m_ctx = new AsyncCtx;
    m_ctx->self = this;

    for (int i = 0; i < AsyncCtx::kUrbs; i++) {
        m_ctx->bufs[i] = new unsigned char[AsyncCtx::kBufSize];
        m_ctx->urbs[i] = libusb_alloc_transfer(0);
        if (!m_ctx->urbs[i]) {
            m_stop = true;
            break;
        }
        libusb_fill_bulk_transfer(m_ctx->urbs[i], m_h, EP_IN, m_ctx->bufs[i],
                                  AsyncCtx::kBufSize, &AsyncCtx::cb, m_ctx, TIMEOUT);
        m_ctx->submit(i);
    }

    // 事件泵：所有 URB 的完成与重新挂起都在 libusb_handle_events 里推进。
    while (!m_stop) {
        struct timeval tv { 0, 5000 };     // 5 ms，作为暂存刷新的时间基准
        {
            // 事件处理不能和 UI 线程的同步 bulk_transfer 并发：
            // libusb_bulk_transfer 内部同样会处理事件，两线程同时进 libusb
            // 会破坏其内部状态（实测 libusb-1.0.dll 0xc0000005）。
            QMutexLocker g(&dfu::libusbGuard());
            libusb_handle_events_timeout_completed(m_usbCtx, &tv, nullptr);
        }

        // 到点就送，避免数据停在暂存里等下一个包
        if (!m_staging.isEmpty() &&
            QDateTime::currentMSecsSinceEpoch() - m_lastFlushMs >= kFlushIntervalMs)
            flushStaging();

        // 待发数据也在本线程写出（唯一持有 libusb 的线程）
        QByteArray tx;
        {
            QMutexLocker lock(&m_owner->m_txMutex);
            tx.swap(m_owner->m_txPending);
        }
        if (!tx.isEmpty()) {
            QMutexLocker g(&dfu::libusbGuard());
            int off = 0;
            while (off < tx.size() && !m_stop) {
                int chunk = qMin(4096, tx.size() - off);
                int moved = 0;
                int r = libusb_bulk_transfer(m_h, EP_OUT,
                                             (unsigned char *)(tx.constData() + off),
                                             chunk, &moved, TIMEOUT);
                if (r < 0)
                    break;               // 设备已掉线：丢弃剩余，等 close()
                off += moved;
            }
        }
    }
    flushStaging();                        // 退出前把剩余字节交出去

    /* 收尾：先把在飞的传输全部回收，再释放。
     *
     * libusb_cancel_transfer() 是异步的——它只排队取消，真正的归还发生在
     * 之后的回调里。以前这里 cancel 完立刻 free_transfer() 并 delete m_ctx，
     * 于是稍后到达的回调会解引用已释放的 transfer/ctx：点击「更新固件」时
     * 设备收到 ENTER_DFU 立刻掉线，正好命中这个窗口，表现为闪退。
     *
     * 正确顺序：cancel -> 泵事件直到 pending==0 -> 再 free/delete。 */
    for (int i = 0; i < AsyncCtx::kUrbs; i++) {
        if (m_ctx->urbs[i])
            libusb_cancel_transfer(m_ctx->urbs[i]);
    }
    {
        QElapsedTimer reap; reap.start();
        while (m_ctx->pending.load() > 0 && reap.elapsed() < 2000) {
            struct timeval tv { 0, 5000 };
            QMutexLocker g(&dfu::libusbGuard());
            libusb_handle_events_timeout_completed(m_usbCtx, &tv, nullptr);
        }
        if (m_ctx->pending.load() > 0) {
            std::fprintf(stderr, "[usb] %d transfer(s) not reaped within 2s\n",
                         m_ctx->pending.load());
            m_cleanShutdown = false;   // 不能安全拆除：交给 close() 决定
        }
    }

    const bool clean = m_cleanShutdown;
    for (int i = 0; i < AsyncCtx::kUrbs; i++) {
        // 不干净时连 transfer 一起留着：上下文反正也不再使用，但留下悬挂
        // 指针比泄漏更糟。
        if (m_ctx->urbs[i] && clean) {
            libusb_free_transfer(m_ctx->urbs[i]);
            m_ctx->urbs[i] = nullptr;
        }
        if (clean) {
            delete[] m_ctx->bufs[i];
            m_ctx->bufs[i] = nullptr;
        }
    }
    delete m_ctx;
    m_ctx = nullptr;
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
    m_reader = new UsbReader(this, m_handle, m_ctx);
    m_reader->start();

    emit opened(m_name);
    return true;
}

void UsbLink::close()
{
    bool clean = true;
    if (m_reader) {
        m_reader->stop();
        /* 事件泵每 5ms 醒一次，正常几十毫秒就退出。等它自己收尾——删一个还在跑的
         * QThread 是未定义行为：它的 URB 回调可能仍在访问即将被销毁的上下文。 */
        if (!m_reader->wait(5000)) {
            std::fprintf(stderr, "[usb] reader thread did not exit in 5s; leaking context to avoid a crash\n");
            m_reader = nullptr;
            m_ctx = nullptr;
            m_handle = nullptr;
            if (m_open) { m_open = false; emit closed(); }
            return;
        }
        clean = m_reader->cleanShutdown();
        delete m_reader;
        m_reader = nullptr;
    }

    if (!clean) {
        /* 仍有异步传输挂在句柄上。libusb_close() 会去遍历已释放的传输链表，
         * 崩在 libusb-1.0.dll 里（0xc0000005，点「更新固件」时设备掉线必现）。
         * 宁可泄漏这一次的句柄/上下文，也不能让进程崩掉；下次 open() 会用
         * 全新的上下文，功能不受影响。 */
        std::fprintf(stderr, "[usb] transfers still pending; skipping libusb teardown\n");
        m_handle = nullptr;
        m_ctx = nullptr;
        if (m_open) { m_open = false; emit closed(); }
        return;
    }

    QMutexLocker guard(&dfu::libusbGuard());
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

    /* 只入队，由接收线程写出去。
     *
     * 之前这里直接调 libusb_bulk_transfer：它内部同样要处理事件，于是 UI 线程
     * 和接收线程会并发进入同一个 libusb 上下文，破坏其内部状态——点「更新固件」
     * 时设备立刻掉线、大量异步完成与这次同步调用交织，必定触发
     * libusb-1.0.dll 0xc0000005（闪退）。改成单一线程持有 libusb 之后，
     * 这条路径不再有跨线程的库调用。 */
    {
        QMutexLocker lock(&m_txMutex);
        m_txPending.append(bytes);
    }
    emit rawTx(bytes);      // UI 侧显示"已发送"与真正落盘无关，先发信号
}

void UsbLink::flush()
{
    if (!m_open)
        return;
    /* 等待接收线程把待发数据写完。ENTER_DFU 之后紧接着 close()，若不等待，
     * 命令可能还在队列里就被关掉，设备永远进不了 DFU。 */
    QElapsedTimer t; t.start();
    while (t.elapsed() < 2000) {
        {
            QMutexLocker lock(&m_txMutex);
            if (m_txPending.isEmpty())
                return;
        }
        QThread::msleep(2);
    }
}

void UsbLink::onBulkIn(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    feedParser(data);
}
