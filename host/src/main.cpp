// CANable2-N32 上位机 — 程序入口
#include <QApplication>
#include <QFont>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QLineEdit>
#include <QComboBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QCheckBox>
#include <QToolButton>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTimer>
#include <QSettings>
#include <QCoreApplication>
#include <QPushButton>
#include <cstdio>
#include "MainWindow.h"
#include "Protocol.h"
#include "dfu_device.h"
#include "UsbLink.h"

// 无头 UI 逻辑自测：
//  - 排版：发送表默认 20 行、行号列、选择列无表头、默认未勾选
//  - 发送：勾选数决定模式、仅数据变化才打包、一次 doSend 只写一次
//  - 接收：相同帧 id 就地更新（行数不变），不同 id 多行
static int runUiTest()
{
    // 先隔离 QSettings 并清空，再构造窗口（否则窗口构造时会载入上次的持久化状态）
    QCoreApplication::setOrganizationName(QStringLiteral("CANable2-N32-uitest"));
    QCoreApplication::setApplicationName(QStringLiteral("canable2_uitest"));
    QSettings().clear();
    MainWindow w;
    auto *table    = w.findChild<QTableWidget *>("table");
    auto *rxTable  = w.findChild<QTableWidget *>("rxTable");
    auto *idEdit   = w.findChild<QLineEdit *>("idEdit");
    auto *dataEdit = w.findChild<QLineEdit *>("dataEdit");
    auto *modeBtn   = w.findChild<QToolButton *>("modeBtn");
    auto *modeLbl   = w.findChild<QLabel *>("modeLbl");
    auto *scroll    = w.findChild<QTableWidget *>("scroll");   // 滚动视图（带表头）
    if (modeBtn) modeBtn->click();                           // 点击切换图标 -> 滚动显示
    const bool modeLblOk = modeLbl && modeLbl->text() == QStringLiteral("滚动");
    const bool scrollHdrOk = scroll && scroll->columnCount() == 5 &&
                             scroll->horizontalHeaderItem(0) &&
                             scroll->horizontalHeaderItem(0)->text() == QStringLiteral("方向");
    if (!table || !rxTable || !idEdit || !dataEdit) {
        std::fputs("UITEST: FAIL (widgets not found)\n", stdout);
        return 1;
    }

    // 发送表列：0 行号 1 选择 2 ID 3 标准/扩展 4 长度 5 帧类型 6 数据
    const int C_NO = 0, C_CHK = 1, C_ID = 2, C_DATA = 6;

    if (table->rowCount() != 20) { std::fputs("UITEST: FAIL (default rows != 20)\n", stdout); return 1; }
    if (table->item(0, C_NO)->text() != QStringLiteral("1")) { std::fputs("UITEST: FAIL (row-number)\n", stdout); return 1; }
    if (table->item(1, C_NO)->text() != QStringLiteral("2")) { std::fputs("UITEST: FAIL (row-number2)\n", stdout); return 1; }
    if (table->item(0, C_CHK)->checkState() != Qt::Unchecked) { std::fputs("UITEST: FAIL (default checkbox)\n", stdout); return 1; }
    // 表头前两列无文字
    if (!table->horizontalHeaderItem(C_NO)->text().isEmpty() ||
        !table->horizontalHeaderItem(C_CHK)->text().isEmpty()) {
        std::fputs("UITEST: FAIL (header text not empty)\n", stdout);
        return 1;
    }

    // 行0/行1 填 ID 与数据（下拉保持默认 标准/8B/标准帧）
    table->item(0, C_ID)->setText(QStringLiteral("100"));
    table->item(0, C_DATA)->setText(QStringLiteral("11 22 33 44 55 66 77 88"));
    table->item(1, C_ID)->setText(QStringLiteral("200"));
    table->item(1, C_DATA)->setText(QStringLiteral("AA BB CC DD"));

    idEdit->setText(QStringLiteral("300"));
    dataEdit->setText(QStringLiteral("01 02"));

    const int flush0 = w.txFlushCount();

    table->item(0, C_CHK)->setCheckState(Qt::Checked);
    table->item(1, C_CHK)->setCheckState(Qt::Checked);
    const QString first  = w.doSend();
    const QByteArray firstFlush = w.lastTxFlush();
    const int scrollBefore2 = scroll ? scroll->rowCount() : 0;
    const QString second = w.doSend();
    const QByteArray secondFlush = w.lastTxFlush();
    const int scrollAfter2 = scroll ? scroll->rowCount() : 0;
    // 数据未变化：滚动区仍打印要发送的帧，但实际只下发 TX_START 命令（未打包数据帧）
    const bool echoUnchangedOk =
        (scrollAfter2 - scrollBefore2) == 2 &&
        (secondFlush == QByteArray::fromHex("AAAA0200000000"));

    table->item(1, C_CHK)->setCheckState(Qt::Unchecked);
    const QString single1 = w.doSend();
    table->item(0, C_CHK)->setCheckState(Qt::Unchecked);
    const QString single2 = w.doSend();

    // 日志区不再出现 TX 数据/命令 hex（仅告警/报错）
    const QString log = w.logText();
    const bool logClean = !log.contains(QStringLiteral("TX ")) &&
                          !log.contains(QStringLiteral("AA AB")) &&
                          !log.contains(QStringLiteral("AA AA"));

    const int flushTotal = w.txFlushCount() - flush0;   // 期望 4
    bool bundle1 = firstFlush.startsWith(QByteArray::fromHex("AAAA07"))
                   && firstFlush.endsWith(QByteArray::fromHex("AAAA0200000000"))
                   && firstFlush.contains(QByteArray::fromHex("AAAA06"))
                   && firstFlush.size() == 51;
    bool bundle2 = (secondFlush == QByteArray::fromHex("AAAA0200000000"));

    // ---- 接收区就地更新 ----
    proto::RxFrame f1; f1.id = 0x1; f1.data = QByteArray::fromHex("0102");
    proto::RxFrame f1b; f1b.id = 0x1; f1b.data = QByteArray::fromHex("030405");
    proto::RxFrame f2; f2.id = 0x2; f2.data = QByteArray::fromHex("AABB");
    w.feedRx(f1);
    w.feedRx(f1b);   // 同 id -> 就地更新
    w.feedRx(f2);    // 不同 id -> 新行
    const int rxRows = w.rxRowCount();
    bool rxOk = (rxRows == 2)                                   // 同 id 不新增行
                && w.rxCellText(0, 3) == QStringLiteral("03 04 05")  // 数据已更新
                && w.rxCellText(0, 4) == QStringLiteral("2");        // 次数=2
    // 无边线
    bool noGrid = !rxTable->showGrid();

    // ---- 导出 CSV：仅勾选行 ----
    // 此时勾选状态：行1 已在前面被取消，行0 也被取消 -> 重新勾选行0、行1
    table->item(0, C_CHK)->setCheckState(Qt::Checked);
    table->item(1, C_CHK)->setCheckState(Qt::Checked);
    const QString csv = w.buildExportCsv();
    const QStringList rowsCsv = csv.split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
    bool csvOk = rowsCsv.size() == 3                            // 表头 + 2 行
                 && rowsCsv[0] == QStringLiteral("序号,帧ID,帧格式,帧类型,数据长度,数据")
                 && rowsCsv[1].contains(QStringLiteral("0x100"))
                 && rowsCsv[1].contains(QStringLiteral("标准帧"))   // 帧格式
                 && rowsCsv[1].contains(QStringLiteral(",CAN,"))    // 帧类型
                 && rowsCsv[1].contains(QStringLiteral("11 22 33 44 55 66 77 88"))
                 && rowsCsv[2].contains(QStringLiteral("0x200"))
                 && rowsCsv[2].contains(QStringLiteral("AA BB CC DD"));
    std::fputs(QStringLiteral("UITEST csvRows=%1 csvOk=%2\n")
                   .arg(rowsCsv.size()).arg(csvOk ? 1 : 0)
                   .toLocal8Bit().constData(), stdout);

    bool ok = first.contains(QStringLiteral("changed=1")) && first.contains(QStringLiteral("列表"))
              && second.contains(QStringLiteral("changed=0"))
              && single1.contains(QStringLiteral("changed=1"))
              && single2.contains(QStringLiteral("changed=1"))
              && logClean
              && flushTotal == 4 && bundle1 && bundle2
              && rxRows == 2 && rxOk && noGrid && csvOk;

    // ---- 传输选择：切到 USB 自动刷新，端口框始终可用 ----
    auto *transportBox = w.findChild<QComboBox *>("transportBox");
    auto *portBox      = w.findChild<QComboBox *>("portBox");
    bool transOk = false;
    if (transportBox && portBox) {
        transportBox->setCurrentIndex(0);              // 串口
        const bool serialEnabled = portBox->isEnabled();
        transportBox->setCurrentIndex(1);              // 切 USB -> 触发 refreshPorts
        const int usbCount = portBox->count();
        const int expect   = UsbLink::scanDevices().size();
        bool usbEnabled = portBox->isEnabled();        // USB 下仍可选
        // 存在本应用设备时，应被排到首位
        const auto devs = UsbLink::scanDevices();
        bool oursPresent = false;
        for (const UsbDeviceInfo &d : devs)
            if (d.vid == UsbLink::kVid && d.pid == UsbLink::kPid) oursPresent = true;
        bool firstIsOurs = true;
        if (oursPresent && !devs.isEmpty())
            firstIsOurs = (devs.first().vid == UsbLink::kVid && devs.first().pid == UsbLink::kPid);
        transportBox->setCurrentIndex(0);              // 切回串口 -> 再刷新
        transOk = serialEnabled && usbEnabled && usbCount == expect && firstIsOurs;
    }
    std::fputs(QStringLiteral("UITEST trans usbCount=%1 transOk=%2\n")
                   .arg(portBox ? portBox->count() : -1).arg(transOk ? 1 : 0)
                   .toLocal8Bit().constData(), stdout);

    ok = ok && transOk;

    const QString stext = w.scrollText();
    const bool scrollOk = stext.contains(QStringLiteral("TX")) &&
                          stext.contains(QStringLiteral("RX")) &&
                          stext.contains(QStringLiteral("0X100")) &&
                          stext.contains(QStringLiteral("0X002"));
    ok = ok && scrollOk && modeLblOk && scrollHdrOk && echoUnchangedOk;

    // ---- 清空接收：静止表与滚动表都要清空 ----
    bool clearOk = false;
    if (auto *clearBtn = w.findChild<QPushButton *>("clearRxBtn")) {
        const int scrollRowsBefore = scroll ? scroll->rowCount() : 0;
        const int staticRowsBefore = w.rxRowCount();
        clearBtn->click();
        clearOk = staticRowsBefore > 0 && scrollRowsBefore > 0 &&
                  w.rxRowCount() == 0 && scroll && scroll->rowCount() == 0;
    }
    ok = ok && clearOk;
    std::fputs(QStringLiteral("UITEST clearOk=%1\n").arg(clearOk ? 1 : 0)
                   .toLocal8Bit().constData(), stdout);
    std::fputs(QStringLiteral("UITEST send  : rows=%1 flush=%2 bundle1=%3 bundle2=%4 logClean=%5 scrollOk=%6\n")
                   .arg(table->rowCount()).arg(flushTotal)
                   .arg(bundle1 ? 1 : 0).arg(bundle2 ? 1 : 0)
                   .arg(logClean ? 1 : 0).arg(scrollOk ? 1 : 0)
                   .toLocal8Bit().constData(), stdout);
    std::fputs(QStringLiteral("UITEST firstFlush : %1\n")
                   .arg(QString::fromLatin1(firstFlush.toHex(' ').toUpper()))
                   .toLocal8Bit().constData(), stdout);
    std::fputs(QStringLiteral("UITEST rxRows=%1 rxOk=%2 noGrid=%3\n")
                   .arg(rxRows).arg(rxOk ? 1 : 0).arg(noGrid ? 1 : 0)
                   .toLocal8Bit().constData(), stdout);
    // ---- 持久化：保存后重建应恢复已填内容 ----
    bool persistOk = false;
    {
        w.saveSettings();
        MainWindow w2;
        auto *t2  = w2.findChild<QTableWidget *>("table");
        auto *id2 = w2.findChild<QLineEdit *>("idEdit");
        persistOk = t2 && id2 &&
            t2->item(0, C_ID) && t2->item(0, C_ID)->text() == QStringLiteral("100") &&
            t2->item(0, C_DATA) && t2->item(0, C_DATA)->text().contains(QStringLiteral("11 22")) &&
            id2->text() == QStringLiteral("300");
    }
    ok = ok && persistOk;
    QSettings().clear();
    std::fputs(QStringLiteral("UITEST persist=%1\n").arg(persistOk ? 1 : 0)
                   .toLocal8Bit().constData(), stdout);
    std::fputs(ok ? "UITEST: PASS\n" : "UITEST: FAIL\n", stdout);
    return ok ? 0 : 1;
}

int main(int argc, char *argv[])
{
    // 协议自检：host.exe --selftest  （不开窗口，跑完即退）
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--selftest") {
            QString report;
            bool ok = proto::selfTest(&report);
            std::fputs(report.toLocal8Bit().constData(), stdout);
            std::fputs(ok ? "SELFTEST: ALL PASS\n" : "SELFTEST: FAILURES\n", stdout);
            return ok ? 0 : 1;
        }
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("CANable2 Host");
    QApplication::setOrganizationName("CANable2-N32");

    // 整体字号放大两号
    {
        QFont f = app.font();
        int ps = f.pointSize();
        if (ps <= 0)
            ps = 9;   // 部分平台 font() 返回像素单位，给一个合理基准
        f.setPointSize(ps + 2);
        app.setFont(f);
    }

    // 无头逻辑自测：host.exe --uitest
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--uitest")
            return runUiTest();
    }

    // GUI 烧机自检：host.exe --guitest
    //   打开主窗口 -> 短暂运行（可模拟发送/滚动显示）-> 自动关闭退出；用于每次编译后验证 GUI 可开可关
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--guitest") {
            QCoreApplication::setOrganizationName(QStringLiteral("CANable2-N32-guitest"));
            QCoreApplication::setApplicationName(QStringLiteral("canable2_guitest"));
            MainWindow w;
            w.show();
            QTimer::singleShot(1500, &app, [&]() {
                w.close();                 // 触发 closeEvent（保存设置路径）
                app.quit();
            });
            const int rc = app.exec();
            std::fputs(rc == 0 ? "GUITEST: OK\n" : "GUITEST: FAIL\n", stdout);
            return rc;
        }
    }

    // 枚举 USB 设备（读 DFU 设备的 VID/PID）：host.exe --dfu-scan
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--dfu-scan") {
            std::fputs(dfu::scanDevices().toLocal8Bit().constData(), stdout);
            dfu::Info in = dfu::findDevice();
            std::fputs(in.found ? "DFU device: FOUND\n" : "DFU device: not found\n", stdout);
            return 0;
        }
    }

    // 测试 libusb 传输：host.exe --usb-open
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--usb-open") {
            const auto devs = UsbLink::scanDevices();
            std::fputs(QStringLiteral("usb devices: %1\n").arg(devs.size())
                           .toLocal8Bit().constData(), stdout);
            for (const UsbDeviceInfo &d : devs)
                std::fputs(("  " + d.label() + "\n").toLocal8Bit().constData(), stdout);

            UsbLink usb;
            QObject::connect(&usb, &Link::opened, [](const QString &n) {
                std::fputs(QStringLiteral("USB open OK: %1\n").arg(n).toLocal8Bit().constData(), stdout);
            });
            QObject::connect(&usb, &Link::errorOccurred, [](const QString &e) {
                std::fputs(QStringLiteral("USB open ERR: %1\n").arg(e).toLocal8Bit().constData(), stdout);
            });
            const QString id = devs.isEmpty() ? QString() : devs.first().id();
            bool ok = usb.open(id);
            std::fputs(ok ? "RESULT: OPEN\n" : "RESULT: FAIL\n", stdout);
            usb.close();
            return ok ? 0 : 1;
        }
    }

    MainWindow w;
    w.show();
    return app.exec();
}
