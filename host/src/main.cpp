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
#include <QListWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTimer>
#include <QSettings>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QPushButton>
#include <QStackedWidget>
#include <QGroupBox>
#include <QAction>
#include <QMenuBar>
#include <QMenu>
#include <QEvent>
#include <QKeyEvent>
#include <cstdio>
#include "HexFormat.h"
#include "MainWindow.h"
#include "Protocol.h"
#include "SimpleProto.h"
#include "CanOpenProto.h"
#include <QDir>
#include <QFile>
#include "TraceImporter.h"
#include "dfu_device.h"
#include "VendorDfuFlasher.h"
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

    // ---- 接收区按字段筛选（跟随当前可见表）----
    bool rxFilterOk = false;
    {
        auto *idF = w.findChild<QLineEdit *>(QStringLiteral("can.rx.f0"));
        auto *dtF = w.findChild<QLineEdit *>(QStringLiteral("can.rx.f3"));
        rxFilterOk = idF != nullptr && dtF != nullptr;
        if (rxFilterOk) {
            idF->setText(QStringLiteral("0x002"));               // 只留 id=0x2 的行
            rxFilterOk = rxTable->isRowHidden(0) && !rxTable->isRowHidden(1);
            idF->clear();
            dtF->setText(QStringLiteral("aa"));                  // 忽略大小写、包含匹配（只 row1 的 AA BB）
            rxFilterOk = rxFilterOk && rxTable->isRowHidden(0) && !rxTable->isRowHidden(1);
            dtF->clear();
            rxFilterOk = rxFilterOk && !rxTable->isRowHidden(0) && !rxTable->isRowHidden(1);
        }
        QString line = QStringLiteral("UITEST rxfilter ok=%1").arg(rxFilterOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && rxFilterOk;

    // ---- 十六进制输入自动格式化（大写 + 每字节空格）----
    bool hexFmtOk = formatHexBytes(QStringLiteral("aa1b2c")) == QStringLiteral("AA 1B 2C")
                 && formatHexBytes(QStringLiteral("E8 03")) == QStringLiteral("E8 03")
                 && formatHexBytes(QStringLiteral("zz12g")) == QStringLiteral("12")
                 && formatHexBytes(QStringLiteral("a b c")) == QStringLiteral("AB C");
    { QString line = QStringLiteral("UITEST hexfmt ok=%1").arg(hexFmtOk ? 1 : 0);
      line += QChar(10); std::fputs(line.toLocal8Bit().constData(), stdout); }
    ok = ok && hexFmtOk;

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

    // ---- 仲裁域波特率下拉：存在且默认 1 Mbit/s ----
    bool arbBitrateOk = false;
    if (auto *arb = w.findChild<QComboBox *>("arbBitrateBox"))
        arbBitrateOk = (arb->count() > 0) && (arb->currentData().toUInt() == 1000000u);
    ok = ok && arbBitrateOk;
    {
        QString line = QStringLiteral("UITEST arbBitrate ok=%1").arg(arbBitrateOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }

    // ---- 菜单栏与双模式页面 ----
    bool menuOk = false, modeSwitchOk = false;
    {
        auto *mb        = w.menuBar();
        auto *stack     = w.findChild<QStackedWidget *>("modeStack");
        auto *pageCan   = w.findChild<QWidget *>("pageCan");
        auto *pageRs485 = w.findChild<QWidget *>("pageRs485");
        auto *act485    = w.findChild<QAction *>("actionModeRs485");
        auto *actCan    = w.findChild<QAction *>("actionModeCan");
        auto *actEc     = w.findChild<QAction *>("actionModeEtherCat");
        auto *pageEc    = w.findChild<QWidget *>("pageEtherCat");
        menuOk = mb && stack && pageCan && pageRs485 && act485 && actCan && actEc && pageEc &&
                 stack->count() == 3 && stack->currentWidget() == pageCan &&
                 actCan->isChecked();
        if (menuOk) {
            act485->trigger();                       // 切到 RS485
            const bool toRs485 = (stack->currentWidget() == pageRs485);
            actEc->trigger();                        // 切到 EtherCAT
            const bool toEc = (stack->currentWidget() == pageEc);
            actCan->trigger();                       // 切回 CAN
            const bool toCan = (stack->currentWidget() == pageCan);
            modeSwitchOk = toRs485 && toEc && toCan;
            // EtherCAT 页关键控件存在 + 适配器扫描不崩溃
            auto *ecAd = w.findChild<QComboBox *>("ecAdapterBox");
            auto *ecScan = w.findChild<QPushButton *>("ecScanBtn");
            if (!(ecAd && ecScan)) modeSwitchOk = false;
        }
        QString line = QStringLiteral("UITEST menu ok=%1 modeSwitch=%2")
                           .arg(menuOk ? 1 : 0).arg(modeSwitchOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && menuOk && modeSwitchOk;

    // ---- 协议页面栈：CyberBeast / Simple / Open 三页，菜单切换 ----
    bool protoOk = false;
    {
        auto *pstack = w.findChild<QStackedWidget *>(QStringLiteral("protoStack"));
        auto *pcyb   = w.findChild<QWidget *>(QStringLiteral("pageProtoCyberBeast"));
        auto *psmp   = w.findChild<QWidget *>(QStringLiteral("pageProtoSimple"));
        auto *pgen   = w.findChild<QWidget *>(QStringLiteral("pageProtoGeneric"));
        auto *aCyb   = w.findChild<QAction *>(QStringLiteral("actionProtoCyberBeast"));
        auto *aSmp   = w.findChild<QAction *>(QStringLiteral("actionProtoSimple"));
        auto *aGen   = w.findChild<QAction *>(QStringLiteral("actionProtoGeneric"));
        protoOk = pstack && pcyb && psmp && pgen && aCyb && aSmp && aGen &&
                  pstack->count() == 4 && pstack->currentWidget() == pgen && aGen->isChecked();
        if (protoOk) {
            aCyb->trigger();
            const bool toCyb = (pstack->currentWidget() == pcyb);
            aSmp->trigger();
            const bool toSimple = (pstack->currentWidget() == psmp);
            aGen->trigger();
            const bool backGen = (pstack->currentWidget() == pgen);
            protoOk = toCyb && toSimple && backGen;
        }
        QString line = QStringLiteral("UITEST proto ok=%1").arg(protoOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && protoOk;

    // ---- 每个独立页面都含「连接区 + 日志区」，且状态共享 ----
    bool pagesConnOk = false, connSyncOk = false, logFanoutOk = false;
    {
        auto hasGroup = [](QWidget *page, const QString &title) {
            if (!page) return false;
            for (QGroupBox *gb : page->findChildren<QGroupBox *>())
                if (gb->title() == title) return true;
            return false;
        };
        QWidget *pages[] = {
            w.findChild<QWidget *>("pageProtoGeneric"),
            w.findChild<QWidget *>("pageProtoSimple"),
            w.findChild<QWidget *>("pageProtoCyberBeast"),
            w.findChild<QWidget *>("pageProtoOpen"),
            w.findChild<QWidget *>("pageRs485"),
        };
        pagesConnOk = true;
        for (QWidget *pg : pages)
            pagesConnOk = pagesConnOk &&
                          hasGroup(pg, QStringLiteral("连接")) &&
                          hasGroup(pg, QStringLiteral("日志"));

        // 传输切换：改任意一份 -> 全部同步
        QVector<QComboBox *> tr;
        for (QComboBox *cb : w.findChildren<QComboBox *>())
            if (cb->objectName().startsWith(QStringLiteral("transportBox"))) tr << cb;
        connSyncOk = (tr.size() == 5);
        if (connSyncOk) {
            tr[2]->setCurrentIndex(1);                 // CyberBeast 页那份 -> USB
            connSyncOk = true;
            for (QComboBox *cb : tr) connSyncOk = connSyncOk && cb->currentIndex() == 1;
        }

        // 日志共享：触发一次警告 -> 每份日志都出现
        auto *rsSend = w.findChild<QPushButton *>("rs485SendBtn");
        QVector<QPlainTextEdit *> logs;
        for (QPlainTextEdit *le : w.findChildren<QPlainTextEdit *>()) logs << le;
        if (rsSend && logs.size() == 6) {   // 5 页 buildLogGroup + EtherCAT 页
            rsSend->click();
            logFanoutOk = true;
            for (QPlainTextEdit *le : logs)
                logFanoutOk = logFanoutOk &&
                              le->toPlainText().contains(QStringLiteral("RS485 发送尚未实现"));
        }
        QString line = QStringLiteral("UITEST pages conn=%1 connSync=%2 logFanout=%3")
                           .arg(pagesConnOk ? 1 : 0).arg(connSyncOk ? 1 : 0).arg(logFanoutOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && pagesConnOk && connSyncOk && logFanoutOk;

    // ---- Simple 协议页：命令下拉 / 参数 / 预览换算 ----
    bool simpleOk = false;
    {
        auto *cmdBox  = w.findChild<QComboBox *>(QStringLiteral("simpleCmdBox"));
        auto *nodeEd  = w.findChild<QLineEdit *>(QStringLiteral("simpleNodeEdit"));
        auto *gearEd  = w.findChild<QLineEdit *>(QStringLiteral("simpleGearEdit"));
        auto *preview = w.findChild<QLineEdit *>(QStringLiteral("simplePreview"));
        simpleOk = cmdBox && nodeEd && gearEd && preview &&
                   cmdBox->count() == simple::commands().size() + simple::examples().size();
        if (simpleOk) {
            // 选 Set_Input_Pos (0x0C)
            int idx = cmdBox->findData(0x0C);
            cmdBox->setCurrentIndex(idx);
            nodeEd->setText(QStringLiteral("1"));
            gearEd->setText(QStringLiteral("1"));
            auto *p0 = w.findChild<QLineEdit *>(QStringLiteral("simpleParam0"));
            auto *p1 = w.findChild<QLineEdit *>(QStringLiteral("simpleParam1"));
            auto *p2 = w.findChild<QLineEdit *>(QStringLiteral("simpleParam2"));
            if (p0 && p1 && p2) {
                p0->setText(QStringLiteral("30"));   // 30 度
                p1->setText(QStringLiteral("1"));    // 1 rev/s
                p2->setText(QStringLiteral("5"));    // 5 Nm
                const QString t = preview->text();
                simpleOk = t.contains(QStringLiteral("ID=0x02C")) &&
                           t.contains(QStringLiteral("AB AA AA 3D E8 03 88 13"));
            } else {
                simpleOk = false;
            }
            // RxSdo (0x04)：opcode(读/写) + Endpoint + 写类型 + Value（仅写模式生效）
            if (simpleOk) {
                cmdBox->setCurrentIndex(cmdBox->findData(0x04));
                auto *op = w.findChild<QComboBox *>(QStringLiteral("sdoOpcodeBox"));
                auto *ep = w.findChild<QLineEdit *>(QStringLiteral("sdoEndpointEdit"));
                auto *ty = w.findChild<QComboBox *>(QStringLiteral("sdoTypeBox"));
                auto *vl = w.findChild<QLineEdit *>(QStringLiteral("sdoValueEdit"));
                simpleOk = op && ep && ty && vl &&
                           ty->count() == simple::sdoTypeNames().size();
                if (simpleOk) {
                    // 读模式：类型/Value 置灰、不下发值
                    op->setCurrentIndex(op->findData(0));
                    const bool readGreyed = !ty->isEnabled() && !vl->isEnabled();
                    ep->setText(QStringLiteral("2"));
                    vl->setText(QStringLiteral("123"));
                    const bool readIgnores = preview->text().contains(
                        QStringLiteral("00 02 00 00 00 00 00 00"));
                    // 写模式 float：9.25 @0x00F2 → 01 F2 00 00 00 00 14 41
                    op->setCurrentIndex(op->findData(1));
                    ep->setText(QStringLiteral("242"));
                    ty->setCurrentIndex(ty->findData(simple::SDO_FLOAT));
                    vl->setText(QStringLiteral("9.25"));
                    const QString tf = preview->text();
                    const bool writeOk = vl->isEnabled() && tf.contains(QStringLiteral("ID=0x024")) &&
                                         tf.contains(QStringLiteral("01 F2 00 00 00 00 14 41"));
                    // 写模式 int16：1000 → E8 03 00 00
                    ty->setCurrentIndex(ty->findData(simple::SDO_INT16));
                    vl->setText(QStringLiteral("1000"));
                    const bool int16Ok = preview->text().contains(QStringLiteral("E8 03 00 00"));
                    simpleOk = readGreyed && readIgnores && writeOk && int16Ok;
                }
            }
        }
        QString line = QStringLiteral("UITEST simple ok=%1").arg(simpleOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && simpleOk;

    // ---- 位置/速度单位下拉 + 实战示例 ----
    bool unitOk = false, exampleOk = false;
    {
        auto *cmdBox  = w.findChild<QComboBox *>(QStringLiteral("simpleCmdBox"));
        auto *nodeEd  = w.findChild<QLineEdit *>(QStringLiteral("simpleNodeEdit"));
        auto *gearEd  = w.findChild<QLineEdit *>(QStringLiteral("simpleGearEdit"));
        auto *preview = w.findChild<QLineEdit *>(QStringLiteral("simplePreview"));
        nodeEd->setText(QStringLiteral("1"));

        // 位置：Set_Input_Pos，单位下拉默认「角度(°)」；改为「转」+ gear2 → 30 转 → 60 电机转
        cmdBox->setCurrentIndex(cmdBox->findData(0x0C));
        auto *u0 = w.findChild<QComboBox *>(QStringLiteral("simpleUnit0"));
        auto *u1 = w.findChild<QComboBox *>(QStringLiteral("simpleUnit1"));
        unitOk = u0 && u1 && u0->count() == simple::positionUnitNames().size() &&
                 u1->count() == simple::velocityUnitNames().size() &&
                 u0->currentIndex() == 1;           // 默认角度
        if (unitOk) {
            auto *p0 = w.findChild<QLineEdit *>(QStringLiteral("simpleParam0"));
            auto *p1 = w.findChild<QLineEdit *>(QStringLiteral("simpleParam1"));
            auto *p2 = w.findChild<QLineEdit *>(QStringLiteral("simpleParam2"));
            p1->setText(QStringLiteral("0")); p2->setText(QStringLiteral("0"));
            gearEd->setText(QStringLiteral("2"));
            p0->setText(QStringLiteral("30"));
            u0->setCurrentIndex(0);                // 转
            const bool posTurns = preview->text().contains(QStringLiteral("00 00 70 42"));  // 60.0f
            // 速度：Set_Input_Vel，单位「转每分」→ 3000 rpm = 50 rev/s
            cmdBox->setCurrentIndex(cmdBox->findData(0x0D));
            gearEd->setText(QStringLiteral("1"));
            auto *v0 = w.findChild<QComboBox *>(QStringLiteral("simpleUnit0"));
            auto *vp = w.findChild<QLineEdit *>(QStringLiteral("simpleParam0"));
            vp->setText(QStringLiteral("3000"));
            v0->setCurrentIndex(2);                // 转每分
            const bool velRpm = preview->text().contains(QStringLiteral("00 00 48 42"));  // 50.0f
            unitOk = posTurns && velRpm;
        }

        // 实战示例：位置控制（0x0B=3/3 + 0x07=8 + 用户 0x0C）
        int exRow = -1;
        for (int r = 0; r < cmdBox->count(); ++r) {
            const QVariant v = cmdBox->itemData(r, Qt::UserRole + 1);
            if (v.isValid() && simple::examples().at(v.toInt()).name.contains(QStringLiteral("位置控制")))
                exRow = r;
        }
        if (exRow >= 0) {
            cmdBox->setCurrentIndex(exRow);
            auto *p0 = w.findChild<QLineEdit *>(QStringLiteral("simpleParam0"));
            const QString t = preview->text();
            exampleOk = p0 && t.contains(QStringLiteral("示例")) &&
                        t.contains(QStringLiteral("0x02B:03 00 00 00 03 00 00 00")) &&
                        t.contains(QStringLiteral("0x027:08 00 00 00 00 00 00 00")) &&
                        t.contains(QStringLiteral("0x02C:"));
        }
        QString line = QStringLiteral("UITEST units ok=%1 example=%2")
                           .arg(unitOk ? 1 : 0).arg(exampleOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && unitOk && exampleOk;
    // ---- Open（CANopen）协议页 ----
    bool coOk = false;
    {
        auto *stack  = w.findChild<QStackedWidget *>(QStringLiteral("protoStack"));
        auto *page   = w.findChild<QWidget *>(QStringLiteral("pageProtoOpen"));
        auto *nodeEd = w.findChild<QLineEdit *>(QStringLiteral("coNodeEdit"));
        auto *funcBox= w.findChild<QComboBox *>(QStringLiteral("coFuncBox"));
        auto *prev   = w.findChild<QLineEdit *>(QStringLiteral("coPreview"));
        auto *sendB  = w.findChild<QPushButton *>(QStringLiteral("coSendBtn"));
        auto *odB    = w.findChild<QPushButton *>(QStringLiteral("coOdBrowserBtn"));
        auto *rxT    = w.findChild<QTableWidget *>(QStringLiteral("coRxTable"));
        auto *scr    = w.findChild<QTableWidget *>(QStringLiteral("coScroll"));
        coOk = stack && page && nodeEd && funcBox && prev && sendB && odB && rxT && scr &&
               funcBox->count() == 12 + canopen::examples().size();
        if (coOk) {
            if (auto *a = w.findChild<QAction *>(QStringLiteral("actionProtoOpen"))) a->trigger();
            coOk = (stack->currentWidget() == page);
            nodeEd->setText(QStringLiteral("1"));
            // NMT operational → ID=0x000 DATA=01 01
            funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_NMT));
            auto *nmt = w.findChild<QComboBox *>(QStringLiteral("coNmtCmdBox"));
            nmt->setCurrentIndex(nmt->findData(0x01));
            const bool nmtOk = prev->text().contains(QStringLiteral("ID=0x000")) &&
                               prev->text().contains(QStringLiteral("DATA=01 01"));
            // SDO 写 6040h:00 = 6 (uint16) → 2B 40 60 00 06 00 00 00
            funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_RSDO));
            auto *idx = w.findChild<QComboBox *>(QStringLiteral("coSdoIdxBox"));
            auto *val = w.findChild<QLineEdit *>(QStringLiteral("coSdoValueEdit"));
            const bool haveIdx = idx && idx->findData((uint)(0x6040u << 8)) >= 0;
            if (haveIdx) idx->setCurrentIndex(idx->findData((uint)(0x6040u << 8)));
            val->setText(QStringLiteral("6"));
            const bool sdoOk = haveIdx && prev->text().contains(QStringLiteral("ID=0x601")) &&
                               prev->text().contains(QStringLiteral("2B 40 60 00 06 00 00 00"));
            // 实战示例：周期同步速度模式（末帧 COB-ID 0x401）
            int exRow = -1;
            for (int r = 0; r < funcBox->count(); ++r) {
                const QVariant v = funcBox->itemData(r, Qt::UserRole + 1);
                if (v.isValid() && canopen::examples().at(v.toInt()).ids.last() == 0x401) exRow = r;
            }
            if (exRow >= 0) funcBox->setCurrentIndex(exRow);
            coOk = nmtOk && sdoOk && exRow >= 0 && prev->text().contains(QStringLiteral("0x401"));
            // 实战示例可配置：选「周期同步位置模式」，设 180°单位
            {
                auto *gear = w.findChild<QLineEdit *>(QStringLiteral("coGearEdit"));
                if (gear) gear->setText(QStringLiteral("1"));
                int posRow = -1;
                for (int r = 0; r < funcBox->count(); ++r) {
                    const QVariant v = funcBox->itemData(r, Qt::UserRole + 1);
                    if (v.isValid() && canopen::examples().at(v.toInt()).name.contains(QStringLiteral("位置")) &&
                        canopen::examples().at(v.toInt()).inputs.size() == 1) posRow = r;
                }
                if (posRow >= 0) {
                    funcBox->setCurrentIndex(posRow);
                    auto *vex = w.findChild<QLineEdit *>(QStringLiteral("coExVal0"));
                    auto *uex = w.findChild<QComboBox *>(QStringLiteral("coExUnit0"));
                    bool exIn = vex && uex && uex->count() == 3;
                    if (exIn) {
                        vex->setText(QStringLiteral("180"));
                        uex->setCurrentIndex(1);   // 角度
                        exIn = prev->text().contains(QStringLiteral("0F 00 00 20 00 00"));  // 180° = 0.5 转
                        uex->setCurrentIndex(0);
                        vex->setText(QStringLiteral("1"));   // 1 转
                        exIn = exIn && prev->text().contains(QStringLiteral("0F 00 00 40 00 00"));
                    }
                    coOk = coOk && exIn;
                    // 实战示例：命令列表分组隐藏
                    if (auto *lst = w.findChild<QListWidget *>(QStringLiteral("coList")))
                        coOk = coOk && !lst->isVisible();
                }
                // PDO 映射修改示例：可配置 PDO + 目标对象
                int mapRow = -1;
                for (int r = 0; r < funcBox->count(); ++r) {
                    const QVariant v = funcBox->itemData(r, Qt::UserRole + 1);
                    if (v.isValid() && canopen::examples().at(v.toInt()).name.contains(QStringLiteral("映射修改"))) mapRow = r;
                }
                if (mapRow >= 0) {
                    funcBox->setCurrentIndex(mapRow);
                    auto *pdoBox = w.findChild<QComboBox *>(QStringLiteral("coMapPdoBox"));
                    auto *o0 = w.findChild<QComboBox *>(QStringLiteral("coMapObj0"));
                    bool mapOk = pdoBox && o0 && o0->count() > 10 &&
                                 pdoBox->currentData().toInt() == (int)canopen::F_TPDO1;
                    if (mapOk) {
                        // 默认：TPDO1 当前（EDS）映射 = 6041h + 6061h
                        mapOk = prev->text().contains(QStringLiteral("23 00 1A 01 10 00 41 60")) &&
                                prev->text().contains(QStringLiteral("23 00 1A 02 08 00 61 60"));
                        // 切到 RPDO1 → 下方改为 RPDO1 的当前映射（6040h + 6060h）
                        int r1 = pdoBox->findData((int)canopen::F_RPDO1);
                        if (r1 >= 0) pdoBox->setCurrentIndex(r1);
                        QCoreApplication::processEvents();   // \u8ba9\u5ef6\u540e\u91cd\u5efa\u751f\u6548
                        mapOk = mapOk && prev->text().contains(QStringLiteral("23 00 14 01 01 02 00 80")) &&
                                prev->text().contains(QStringLiteral("23 00 16 01 10 00 40 60"));
                    }
                    coOk = coOk && mapOk;
                }
            }
        if (auto *g = w.findChild<QAction *>(QStringLiteral("actionProtoGeneric"))) g->trigger();  // 复位到通用CAN
        }
        QString line = QStringLiteral("UITEST canopen ok=%1");
        line = line.arg(coOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && coOk;

    // ---- CANopen 持久化：node/func(示例) 往返 ----
    bool coPersist = false;
    {
        if (auto *cb = w.findChild<QComboBox *>(QStringLiteral("coFuncBox"))) {
            int exRow = -1;
            for (int r = 0; r < cb->count(); ++r) {
                const QVariant v = cb->itemData(r, Qt::UserRole + 1);
                if (v.isValid() && canopen::examples().at(v.toInt()).ids.last() == 0x401) exRow = r;
            }
            if (exRow >= 0) cb->setCurrentIndex(exRow);
        }
        if (auto *nd = w.findChild<QLineEdit *>(QStringLiteral("coNodeEdit"))) nd->setText(QStringLiteral("2"));
        if (auto *gr = w.findChild<QLineEdit *>(QStringLiteral("coGearEdit"))) gr->setText(QStringLiteral("3"));
        w.saveSettings();
        MainWindow w5;
        auto *cb5 = w5.findChild<QComboBox *>(QStringLiteral("coFuncBox"));
        auto *nd5 = w5.findChild<QLineEdit *>(QStringLiteral("coNodeEdit"));
        auto *gr5 = w5.findChild<QLineEdit *>(QStringLiteral("coGearEdit"));
        coPersist = cb5 && nd5 && gr5 && gr5->text() == QStringLiteral("3") &&
            nd5->text() == QStringLiteral("2") &&
            cb5->currentData(Qt::UserRole + 1).isValid() &&
            canopen::examples().at(cb5->currentData(Qt::UserRole + 1).toInt()).ids.last() == 0x401;
        QString line = QStringLiteral("UITEST canopen persist ok=%1").arg(coPersist ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && coPersist;

    // ---- PDO 多行（EDS 索引下拉 + 数值）+ EDS 导入 ----
    bool pdoOk = false, edsImportOk = false;
    {
        if (auto *funcBox = w.findChild<QComboBox *>(QStringLiteral("coFuncBox"))) {
            if (auto *a = w.findChild<QAction *>(QStringLiteral("actionProtoOpen"))) a->trigger();
            if (auto *nd = w.findChild<QLineEdit *>(QStringLiteral("coNodeEdit"))) nd->setText(QStringLiteral("1"));
            funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_RPDO4));
            auto *idx0 = w.findChild<QComboBox *>(QStringLiteral("coPdoIdx0"));
            auto *idx1 = w.findChild<QComboBox *>(QStringLiteral("coPdoIdx1"));
            auto *prev = w.findChild<QLineEdit *>(QStringLiteral("coPreview"));
            // 索引不可选（仅按默认映射显示）；RPDO4 默认 = 6040h + 6071h
            pdoOk = idx0 && idx1 && prev && !idx0->isEnabled() && !idx1->isEnabled() &&
                    idx0->currentData().toUInt() == ((0x6040u << 8) | 0) &&
                    idx1->currentData().toUInt() == ((0x6071u << 8) | 0);
            if (pdoOk) {
                auto *val1 = w.findChild<QLineEdit *>(QStringLiteral("coPdoVal1"));
                if (val1) val1->setText(QStringLiteral("1000"));
                // RPDO4(node1)=0x501；6071h(INT16)=1000 → E8 03
                pdoOk = pdoOk && prev->text().contains(QStringLiteral("ID=0x501")) &&
                        prev->text().contains(QStringLiteral("E8 03"));
                // 会话内记忆：切到 TPDO1 再切回 RPDO4，输入值仍保留
                funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_TPDO1));
                funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_RPDO4));
                auto *val1b = w.findChild<QLineEdit *>(QStringLiteral("coPdoVal1"));
                pdoOk = pdoOk && val1b && val1b->text() == QStringLiteral("1000");
                // 重开恢复默认（不持久化）：新窗口 RPDO4 的 coPdoVal1 复位为默认 0
                w.saveSettings();
                MainWindow wPdo;
                if (auto *cb = wPdo.findChild<QComboBox *>(QStringLiteral("coFuncBox")))
                    cb->setCurrentIndex(cb->findData((int)canopen::F_RPDO4));
                auto *vp = wPdo.findChild<QLineEdit *>(QStringLiteral("coPdoVal1"));
                pdoOk = pdoOk && vp && vp->text() != QStringLiteral("1000");
            }
            QString line = QStringLiteral("UITEST canopen pdo ok=%1");
            line = line.arg(pdoOk ? 1 : 0);
            line += QChar(10);
            std::fputs(line.toLocal8Bit().constData(), stdout);
        }
    }
    ok = ok && pdoOk;

    // ---- PDO 映射修改 → PDO 传输区同步 ----
    bool mapSyncOk = false;
    {
        if (auto *a = w.findChild<QAction *>(QStringLiteral("actionProtoOpen"))) a->trigger();
        auto *funcBox = w.findChild<QComboBox *>(QStringLiteral("coFuncBox"));
        int mapRow = -1;
        for (int r = 0; funcBox && r < funcBox->count(); ++r) {
            const QVariant v = funcBox->itemData(r, Qt::UserRole + 1);
            if (v.isValid() && canopen::examples().at(v.toInt()).name.contains(QStringLiteral("映射修改"))) mapRow = r;
        }
        mapSyncOk = funcBox && mapRow >= 0;
        if (mapSyncOk) {
            funcBox->setCurrentIndex(mapRow);              // 映射修改示例
            auto *pdoBox = w.findChild<QComboBox *>(QStringLiteral("coMapPdoBox"));
            mapSyncOk = pdoBox != nullptr;
            if (mapSyncOk) {
                pdoBox->setCurrentIndex(pdoBox->findData((int)canopen::F_TPDO1));  // 目标 PDO = TPDO1
                QCoreApplication::processEvents();          // 触发延迟重建
                auto *o0 = w.findChild<QComboBox *>(QStringLiteral("coMapObj0"));
                mapSyncOk = o0 != nullptr;
                int k = -1;
                for (int j = 0; o0 && j < o0->count(); ++j)
                    if (o0->itemData(j).toUInt() == ((0x6064u << 8) | 0)) k = j;   // 换成 6064h
                if (k >= 0) o0->setCurrentIndex(k);
                w.coSyncTransmitFromMapping();             // 执行映射修改 → 同步传输区
                funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_TPDO1));
                auto *i0 = w.findChild<QComboBox *>(QStringLiteral("coPdoIdx0"));
                mapSyncOk = mapSyncOk && i0 && i0->currentData().toUInt() == ((0x6064u << 8) | 0);
            }
        }
        QString line = QStringLiteral("UITEST canopen mapSync ok=%1").arg(mapSyncOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && mapSyncOk;

    // ---- 发送缓存跨页面一致：模式未变不发 SET_MODE，数据未变不发数据帧 ----
    bool cacheConsistOk = false;
    {
        QByteArray fr = QByteArray::fromHex("AABB01020304050607FF");
        w.sendCachedFrames(true, { fr }, 0u); w.flushTxNow();
        const QByteArray f1 = w.lastTxFlush();
        w.sendCachedFrames(true, { fr }, 0u); w.flushTxNow();
        const QByteArray f2 = w.lastTxFlush();
        // 另一页切到单帧：模式变化 → 必须重发 SET_MODE(single)
        QByteArray fr2 = QByteArray::fromHex("AABB0A");
        w.sendCachedFrames(false, { fr2 }, 0u); w.flushTxNow();
        const QByteArray f3 = w.lastTxFlush();
        cacheConsistOk =
            f1.contains(QByteArray::fromHex("AAAA0701000000")) &&   // SET_MODE 列表
            f1.contains(QByteArray::fromHex("AAAA0200000000")) &&   // TX_START
            f1.contains(fr) &&
            f2 == QByteArray::fromHex("AAAA0200000000") &&         // 未变：仅 TX_START
            f3.contains(QByteArray::fromHex("AAAA0700000000")) &&   // SET_MODE 单帧
            f3.contains(fr2) && f3.contains(QByteArray::fromHex("AAAA0200000000"));
        QString line = QStringLiteral("UITEST cacheConsist ok=%1").arg(cacheConsistOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && cacheConsistOk;

    // ---- MCU 错误 ASCII 上报：AA AD 帧 → 日志区 ----
    bool errReportOk = false;
    {
        // 1) 直接经解析器：AA AD | ERR_USB_CRC | DA AA | CRC8
        const QByteArray name = QByteArrayLiteral("ERR_USB_CRC");
        quint8 cb = 0;
        for (int i = 0; i < name.size(); ++i) {
            cb ^= (quint8)name[i];
            for (int b = 0; b < 8; ++b) cb = (cb & 0x80) ? (quint8)((cb << 1) ^ 0x07) : (quint8)(cb << 1);
        }
        QByteArray fr; fr.append(char(0xAA)); fr.append(char(0xAD));
        fr += name; fr.append(char(0xDA)); fr.append(char(0xAA)); fr.append(char(cb));
        proto::Parser ps; ps.feed(fr);
        auto msgs = ps.takeMessages();
        const bool parsed = msgs.size() == 1 && msgs[0].type == proto::Message::Log &&
                            msgs[0].text == QStringLiteral("ERR_USB_CRC") && msgs[0].crcOk;
        // 2) 该消息进入日志区（onLogMessage → logError）
        proto::LogMessage lm; lm.text = msgs.isEmpty() ? QStringLiteral("ERR_USB_CRC") : msgs[0].text;
        lm.crcOk = true;
        QMetaObject::invokeMethod(&w, "onLogMessage", Qt::DirectConnection,
                                  Q_ARG(proto::LogMessage, lm));
        errReportOk = parsed && w.logText().contains(QStringLiteral("ERR_USB_CRC"));
        QString line = QStringLiteral("UITEST errReport ok=%1").arg(errReportOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && errReportOk;
    if (auto *g = w.findChild<QAction *>(QStringLiteral("actionProtoGeneric"))) g->trigger();

    // EDS 导入往返（复制到 <exe>/eds 后重载，再恢复内置）
    {
        const QString tmp = QDir::tempPath() + QStringLiteral("/canopen_test_ds402.eds");
        QFile tf(tmp);
        if (tf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            tf.write(canopen::edsText().toUtf8());
            tf.close();
        }
        QString err;
        edsImportOk = canopen::importEdsFile(tmp, &err) &&
                      canopen::edsSourceName().contains(QStringLiteral("外部")) &&
                      !canopen::objectDictionary().isEmpty();
        canopen::resetToEmbeddedEds();
        edsImportOk = edsImportOk && canopen::edsSourceName().contains(QStringLiteral("内置"));
        QFile::remove(tmp);
        QFile::remove(canopen::externalEdsPath());
        QString line = QStringLiteral("UITEST canopen edsImport ok=%1").arg(edsImportOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && edsImportOk;

    // ---- CANopen 命令列表：追加 / 调序 / 持久化 ----
    bool coListOk = false, coListPersist = false;
    {
        if (auto *a = w.findChild<QAction *>(QStringLiteral("actionProtoOpen"))) a->trigger();
        auto *funcBox = w.findChild<QComboBox *>(QStringLiteral("coFuncBox"));
        auto *addB    = w.findChild<QPushButton *>(QStringLiteral("coAddBtn"));
        auto *lst     = w.findChild<QListWidget *>(QStringLiteral("coList"));
        if (auto *nd = w.findChild<QLineEdit *>(QStringLiteral("coNodeEdit"))) nd->setText(QStringLiteral("1"));
        coListOk = funcBox && addB && lst;
        if (coListOk) {
            // 1) SDO 写 6040h=6 → 追加
            funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_RSDO));
            auto *idx = w.findChild<QComboBox *>(QStringLiteral("coSdoIdxBox"));
            if (idx && idx->findData((uint)(0x6040u << 8)) >= 0)
                idx->setCurrentIndex(idx->findData((uint)(0x6040u << 8)));
            if (auto *v = w.findChild<QLineEdit *>(QStringLiteral("coSdoValueEdit"))) v->setText(QStringLiteral("6"));
            addB->click();
            // 2) NMT operational → 追加
            funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_NMT));
            if (auto *nmt = w.findChild<QComboBox *>(QStringLiteral("coNmtCmdBox")))
                nmt->setCurrentIndex(nmt->findData(0x01));
            addB->click();
            coListOk = (lst->count() == 2);
            // 预览随列表动态更新
            if (auto *pv = w.findChild<QLineEdit *>(QStringLiteral("coPreview")))
                coListOk = coListOk && pv->text().contains(QStringLiteral("共 2 帧"));
            // 3) 第 2 行下移不可（已在底），第 2 行上移 → NMT 到顶部
            if (coListOk) {
                auto *up1 = w.findChild<QToolButton *>(QStringLiteral("coListUp1"));
                if (up1) up1->click();
                auto *lbl0 = w.findChild<QLabel *>(QStringLiteral("coListLbl0"));
                coListOk = lst->count() == 2 && lbl0 && lbl0->text().contains(QStringLiteral("NMT"));
                // 4) 点击第二项（SDO）回填编辑器；修改后「更新」覆盖（条数不变）
                if (coListOk) {
                    lst->setCurrentRow(1);
                    QMetaObject::invokeMethod(lst, "itemClicked", Qt::DirectConnection, Q_ARG(QListWidgetItem *, lst->item(1)));
                    auto *vv = w.findChild<QLineEdit *>(QStringLiteral("coSdoValueEdit"));
                    const bool restored = vv && vv->text() == QStringLiteral("6");
                    if (vv) vv->setText(QStringLiteral("7"));
                    addB->click();   // 更新选中项
                    if (restored) {
                        auto *prev = w.findChild<QLineEdit *>(QStringLiteral("coPreview"));
                        coListOk = prev && prev->text().contains(QStringLiteral("共 2 帧")) &&
                                   prev->text().contains(QStringLiteral("→"));
                    } else { coListOk = false; }
                }
            }
        }
        QString line = QStringLiteral("UITEST canopen list ok=%1").arg(coListOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && coListOk;

    // 持久化：列表随设置保存/恢复
    {
        w.saveSettings();
        MainWindow w6;
        auto *lst6 = w6.findChild<QListWidget *>(QStringLiteral("coList"));
        coListPersist = lst6 && lst6->count() == 2;
        QString line = QStringLiteral("UITEST canopen list persist ok=%1").arg(coListPersist ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    if (auto *g = w.findChild<QAction *>(QStringLiteral("actionProtoGeneric"))) g->trigger();  // 复位到通用CAN
    ok = ok && coListPersist;


    // ---- 菜单快捷键 Ctrl+P/M/H 打开协议/模式/帮助 ----
    bool shortcutOk = false;
    {
        auto *amp = w.findChild<QAction *>("actMenuProtocol");
        auto *amm = w.findChild<QAction *>("actMenuMode");
        auto *amh = w.findChild<QAction *>("actMenuHelp");
        shortcutOk = amp && amm && amh &&
                     amp->shortcut() == QKeySequence(QStringLiteral("Ctrl+P")) &&
                     amm->shortcut() == QKeySequence(QStringLiteral("Ctrl+M")) &&
                     amh->shortcut() == QKeySequence(QStringLiteral("Ctrl+H"));
        QString line = QStringLiteral("UITEST menuShortcut ok=%1").arg(shortcutOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && shortcutOk;

    // ---- 空格：焦点在列表某行时发送该行（无连接时被事件过滤器捕获 -> 记录警告）----
    bool spaceOk = false;
    {
        table->setCurrentCell(0, C_ID);
        const QString before = w.logText();
        QKeyEvent ke(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        const bool consumed = QApplication::sendEvent(table, &ke);
        spaceOk = consumed && (w.logText() != before);   // 键被吞掉且产生了日志
        QString line = QStringLiteral("UITEST spaceSend ok=%1").arg(spaceOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && spaceOk;

    // ---- 数据域波特率下拉：存在且默认 5 Mbit/s ----
    bool dataBitrateOk = false;
    if (auto *db = w.findChild<QComboBox *>("dataBitrateBox"))
        dataBitrateOk = (db->count() > 0) && (db->currentData().toUInt() == 5000000u);
    ok = ok && dataBitrateOk;
    {
        QString line = QStringLiteral("UITEST dataBitrate ok=%1").arg(dataBitrateOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }

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
        // 保存前设定已知状态：Simple 页回到 Set_Input_Pos(0x0C) / node=1 / 参数=30
        if (auto *cb = w.findChild<QComboBox *>("simpleCmdBox")) {
            cb->setCurrentIndex(cb->findData(0x0C));
            if (auto *nd = w.findChild<QLineEdit *>("simpleNodeEdit")) nd->setText(QStringLiteral("1"));
            if (auto *p0 = w.findChild<QLineEdit *>("simpleParam0")) p0->setText(QStringLiteral("30"));
        }
        if (auto *rd = w.findChild<QLineEdit *>("rs485DataEdit")) rd->setText(QStringLiteral("AA BB CC"));
        w.saveSettings();
        MainWindow w2;
        auto *t2  = w2.findChild<QTableWidget *>("table");
        auto *id2 = w2.findChild<QLineEdit *>("idEdit");
        persistOk = t2 && id2 &&
            t2->item(0, C_ID) && t2->item(0, C_ID)->text() == QStringLiteral("100") &&
            t2->item(0, C_DATA) && t2->item(0, C_DATA)->text().contains(QStringLiteral("11 22")) &&
            id2->text() == QStringLiteral("300");

        // 模式 / 协议页 / Simple 页数据也应恢复
        auto *mstack2 = w2.findChild<QStackedWidget *>("modeStack");
        auto *pstack2 = w2.findChild<QStackedWidget *>("protoStack");
        auto *cmd2    = w2.findChild<QComboBox *>("simpleCmdBox");
        auto *node2   = w2.findChild<QLineEdit *>("simpleNodeEdit");
        auto *p0b     = w2.findChild<QLineEdit *>("simpleParam0");
        persistOk = persistOk && mstack2 && pstack2 && cmd2 && node2 && p0b &&
            mstack2->currentIndex() == 0 &&                  // CAN 模式
            pstack2->currentIndex() == 3 &&                  // 通用CAN 协议页
            cmd2->currentData().toInt() == 0x0C &&           // Set_Input_Pos
            node2->text() == QStringLiteral("1") &&
            p0b->text() == QStringLiteral("30");             // 参数 30 度
        auto *rd2 = w2.findChild<QLineEdit *>("rs485DataEdit");
        persistOk = persistOk && rd2 && rd2->text() == QStringLiteral("AA BB CC");

        // RxSdo 专属字段也应持久化：切到 0x04，写 模式 / int16 / 0x00F2 / 500
        if (auto *cb = w.findChild<QComboBox *>("simpleCmdBox")) {
            cb->setCurrentIndex(cb->findData(0x04));
            if (auto *op = w.findChild<QComboBox *>("sdoOpcodeBox")) op->setCurrentIndex(op->findData(1));
            if (auto *ep = w.findChild<QLineEdit *>("sdoEndpointEdit")) ep->setText(QStringLiteral("242"));
            if (auto *ty = w.findChild<QComboBox *>("sdoTypeBox")) ty->setCurrentIndex(ty->findData(simple::SDO_INT16));
            if (auto *vl = w.findChild<QLineEdit *>("sdoValueEdit")) vl->setText(QStringLiteral("500"));
        }
        w.saveSettings();
        {
            MainWindow w3;
            auto *cb3 = w3.findChild<QComboBox *>("simpleCmdBox");
            auto *op3 = w3.findChild<QComboBox *>("sdoOpcodeBox");
            auto *ep3 = w3.findChild<QLineEdit *>("sdoEndpointEdit");
            auto *ty3 = w3.findChild<QComboBox *>("sdoTypeBox");
            auto *vl3 = w3.findChild<QLineEdit *>("sdoValueEdit");
            bool sdoOk = cb3 && op3 && ep3 && ty3 && vl3 &&
                cb3->currentData().toInt() == 0x04 &&
                op3->currentData().toInt() == 1 &&
                ep3->text() == QStringLiteral("242") &&
                ty3->currentData().toInt() == simple::SDO_INT16 &&
                vl3->text() == QStringLiteral("500") &&
                vl3->isEnabled();   // 写模式
            persistOk = persistOk && sdoOk;
            QString sl = QStringLiteral("UITEST persist sdo ok=%1").arg(sdoOk ? 1 : 0);
            sl += QChar(10);
            std::fputs(sl.toLocal8Bit().constData(), stdout);
        }

        // 实战示例 + 单位下拉也应持久化：选「位置控制」示例，参数单位改「转」
        {
            auto *cb = w.findChild<QComboBox *>("simpleCmdBox");
            int exRow = -1;
            for (int r = 0; cb && r < cb->count(); ++r) {
                const QVariant v = cb->itemData(r, Qt::UserRole + 1);
                if (v.isValid() && simple::examples().at(v.toInt()).name.contains(QStringLiteral("位置控制")))
                    exRow = r;
            }
            if (cb && exRow >= 0) {
                cb->setCurrentIndex(exRow);
                if (auto *u0 = w.findChild<QComboBox *>("simpleUnit0")) u0->setCurrentIndex(0); // 转
            }
            w.saveSettings();
            MainWindow w4;
            auto *cb4 = w4.findChild<QComboBox *>("simpleCmdBox");
            auto *u04 = w4.findChild<QComboBox *>("simpleUnit0");
            const bool exOk = cb4 && u04 &&
                cb4->currentData(Qt::UserRole + 1).isValid() &&
                simple::examples().at(cb4->currentData(Qt::UserRole + 1).toInt())
                    .name.contains(QStringLiteral("位置控制")) &&
                u04->currentIndex() == 0;   // 单位「转」
            persistOk = persistOk && exOk;
            QString el = QStringLiteral("UITEST persist example ok=%1").arg(exOk ? 1 : 0);
            el += QChar(10);
            std::fputs(el.toLocal8Bit().constData(), stdout);
        }

        QString line = QStringLiteral("UITEST persist ok=%1").arg(persistOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && persistOk;
    QSettings().clear();
    std::fputs(QStringLiteral("UITEST persist=%1\n").arg(persistOk ? 1 : 0)
                   .toLocal8Bit().constData(), stdout);
    // ---- 连接初始化：不再自动查询固件版本（命令 10），避免固件 ERR_USB_BADFRAME ----
    bool connOk = false;
    {
        const int base = w.txFlushCount();
        QMetaObject::invokeMethod(&w, "onPortOpened", Q_ARG(QString, QStringLiteral("TEST")));
        const QByteArray conn = w.lastTxFlush();
        const bool hasCmds = conn.contains(QByteArray::fromHex("AAAA01")) &&
                             conn.contains(QByteArray::fromHex("AAAA08")) &&
                             conn.contains(QByteArray::fromHex("AAAA07"));
        const bool noVer   = !conn.contains(QByteArray::fromHex("AAAA0A"));
        connOk = (w.txFlushCount() == base + 1) && hasCmds && noVer;
        QString cl = QStringLiteral("UITEST conn noVerQuery=%1 flush=%2")
                         .arg(noVer ? 1 : 0).arg(w.txFlushCount() - base);
        cl += QChar(10);
        std::fputs(cl.toLocal8Bit().constData(), stdout);
    }
    ok = ok && connOk;

    std::fputs(ok ? "UITEST: PASS\n" : "UITEST: FAIL\n", stdout);
    return ok ? 0 : 1;
}

// DFU 离线自检：校验 DfuSe 命令编码 / 内存布局解析 / 扇区计算（无硬件依赖）
static int runDfuSelfTest()
{
    int fail = 0;
    auto check = [&](bool c, const QString &name) {
        std::fputs(QStringLiteral("%1 %2\n").arg(c ? "[ok]  " : "[FAIL]").arg(name)
                       .toLocal8Bit().constData(), stdout);
        if (!c) fail++;
    };

    // DfuSe 特殊命令编码
    check(dfu::encodeSetAddress(0x08000000) == QByteArray::fromHex("2100000008"),
          QStringLiteral("set-address 0x08000000 -> 21 00 00 00 08"));
    check(dfu::encodeErase(0x08010000) == QByteArray::fromHex("4100000108"),
          QStringLiteral("erase 0x08010000 -> 41 00 00 01 08"));

    // 内存布局解析（示例串取自 DFU alt-setting）
    quint32 base = 0;
    const QString layout = QStringLiteral("@Internal Flash  /0x08000000/04*016Kg,01*064Kg,03*128Kg");
    const QList<dfu::Sector> secs = dfu::parseLayout(layout, &base);
    check(base == 0x08000000, QStringLiteral("base=0x08000000"));
    check(secs.size() == 8, QStringLiteral("扇区数=8"));
    check(!secs.isEmpty() && secs[0].len == 0x4000 && secs[4].len == 0x10000
              && secs[5].len == 0x20000,
          QStringLiteral("扇区大小 16K/64K/128K"));
    quint32 total = 0;
    for (const dfu::Sector &s : secs) total += s.len;
    check(total == 0x80000, QStringLiteral("总容量 = 512KB"));

    // 非 Internal Flash 名应被忽略
    quint32 b2 = 0;
    check(dfu::parseLayout(QStringLiteral("@Option Bytes /0x1FFFF800/01*016 g"), &b2).isEmpty(),
          QStringLiteral("非 Internal Flash 名 -> 空表"));

    // 块大小 = gcd(扇区, 2048)
    check(dfu::transferSize(0x4000) == 2048, QStringLiteral("transferSize(16K)=2048"));
    check(dfu::transferSize(0x100) == 256,  QStringLiteral("transferSize(256)=256"));

    // 覆盖计算：20184 字节从 0x08000000 -> 扇区0(16K)+扇区1
    const QList<dfu::Sector> cov = dfu::sectorsCovering(secs, 0x08000000, 20184);
    check(cov.size() == 2, QStringLiteral("20184B 覆盖 2 个扇区"));

    std::fputs(fail == 0 ? "DFU-SELFTEST: ALL PASS\n" : "DFU-SELFTEST: FAILURES\n", stdout);
    return fail == 0 ? 0 : 1;
}

int main(int argc, char *argv[])
{
    // 协议自检：host.exe --selftest  （不开窗口，跑完即退）
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--selftest") {
            QString report;
            bool ok = proto::selfTest(&report);
            QString sreport;
            ok = simple::selfTest(&sreport) && ok;
            report += sreport;
            QString creport;
            ok = canopen::selfTest(&creport) && ok;
            report += creport;
            std::fputs(report.toLocal8Bit().constData(), stdout);
            std::fputs(ok ? "SELFTEST: ALL PASS\n" : "SELFTEST: FAILURES\n", stdout);
            return ok ? 0 : 1;
        }
    }

    // DFU 离线自检：host.exe --dfu-selftest
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--dfu-selftest")
            return runDfuSelfTest();
    }

    // DFU 协议探针：host.exe --dfu-probe
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--dfu-probe") {
            std::fputs(dfu::probeDfu().toLocal8Bit().constData(), stdout);
            return 0;
        }
    }

    // DFU 设备描述符 dump：host.exe --dfu-info
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--dfu-info") {
            std::fputs(dfu::describeDevice().toLocal8Bit().constData(), stdout);
            return 0;
        }
    }

    // CAN 记录导入测试（无头）：host.exe --import-trace <file>
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--import-trace") {
            if (i + 1 >= argc) { std::fputs("usage: --import-trace <file>\n", stdout); return 2; }
            const QString path = QString::fromLocal8Bit(argv[i + 1]);
            trace::Result r = trace::load(path);
            if (!r.ok) {
                std::fputs(("ERROR: " + r.error + "\n").toLocal8Bit().constData(), stdout);
                return 1;
            }
            QByteArray out;
            out += QStringLiteral("format=%1 frames=%2 skipped=%3\n")
                       .arg(r.format).arg(r.frames.size()).arg(r.skipped).toLocal8Bit();
            int shown = 0;
            for (const trace::Frame &f : r.frames) {
                if (shown++ >= 12) break;
                out += QStringLiteral("  id=0x%1 %2 %3 len=%4 data=%5\n")
                           .arg(f.id, 0, 16).arg(f.extended ? "EXT" : "STD")
                           .arg(f.fd ? (f.brs ? "FD+BRS" : "FD") : "CAN")
                           .arg(f.data.size())
                           .arg(QString::fromLatin1(f.data.toHex(' ').toUpper())).toLocal8Bit();
            }
            std::fputs(out.constData(), stdout);
            std::fputs("IMPORT-TRACE: OK\n", stdout);
            return 0;
        }
    }

    // DFU 烧录测试：host.exe --dfu-flash <bin> [addr]
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--dfu-flash") {
            QString path;
            quint32 addr = 0x08000000;
            if (i + 1 < argc) path = QString::fromLocal8Bit(argv[i + 1]);
            if (i + 2 < argc) addr = QString::fromLocal8Bit(argv[i + 2]).toUInt(nullptr, 0);
            if (path.isEmpty()) { std::fputs("usage: --dfu-flash <bin> [addr]\n", stdout); return 2; }
            std::fputs(QStringLiteral("flashing %1 -> 0x%2 ...\n").arg(path)
                           .arg(addr, 8, 16, QLatin1Char('0')).toLocal8Bit().constData(), stdout);
            QString err;
            bool ok = dfu::flashBin(path, addr, &err,
                                    [](int p, const QString &s) {
                                        std::fputs(QStringLiteral("  %1%  %2\n").arg(p).arg(s)
                                                       .toLocal8Bit().constData(), stdout);
                                    });
            if (!ok) std::fputs(("  ERR: " + err + "\n").toLocal8Bit().constData(), stdout);
            std::fputs(ok ? "DFU-FLASH: OK\n" : "DFU-FLASH: FAIL\n", stdout);
            return ok ? 0 : 1;
        }
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName("CANable2-N32");
    QApplication::setApplicationName(QStringLiteral("CyberBeast 协议调试工具"));
    // 改名后 QSettings 默认路径会变：把旧位置 (CANable2-N32 / CyberBeast CANable 2)
    // 的键值一次性搬到新位置，避免老用户配置/窗口几何丢失。
    {
        QSettings curSt;
        if (curSt.allKeys().isEmpty()) {
            QSettings oldSt(QStringLiteral("CANable2-N32"), QStringLiteral("CyberBeast CANable 2"));
            for (const QString &k : oldSt.allKeys()) curSt.setValue(k, oldSt.value(k));
        }
    }

    // 整体字号放大两号
    {
        QFont f = app.font();
        int ps = f.pointSize();
        if (ps <= 0)
            ps = 9;   // 部分平台 font() 返回像素单位，给一个合理基准
        f.setPointSize(ps + 2);
        app.setFont(f);
    }

    // 厂商 DLL 路径烧录测试（QApplication 之后以取到 exe 目录）：host.exe --vendor-flash <bin> [addr]
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--vendor-flash") {
            QString path;
            quint32 addr = 0x08000000;
            if (i + 1 < argc) path = QString::fromLocal8Bit(argv[i + 1]);
            if (i + 2 < argc) addr = QString::fromLocal8Bit(argv[i + 2]).toUInt(nullptr, 0);
            if (path.isEmpty()) { std::fputs("usage: --vendor-flash <bin> [addr]\n", stdout); return 2; }
            std::fputs(QStringLiteral("vendor_dfu available: %1\n")
                           .arg(vendorDfu::available() ? "yes" : "no").toLocal8Bit().constData(), stdout);
            QString err;
            bool ok = vendorDfu::flash(path, addr, &err, [](int p, const QString &s) {
                std::fputs(QStringLiteral("  %1%  %2\n").arg(p).arg(s).toLocal8Bit().constData(), stdout);
            });
            if (!ok) std::fputs(("  ERR: " + err + "\n").toLocal8Bit().constData(), stdout);
            std::fputs(ok ? "VENDOR-FLASH: OK\n" : "VENDOR-FLASH: FAIL\n", stdout);
            return ok ? 0 : 1;
        }
    }

    // 无头逻辑自测：host.exe --uitest
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--uitest")
            return runUiTest();
    }

    // GUI 烧机自检：host.exe --guitest
    //   打开主窗口 -> 校验窗口落在可用屏幕内 -> 短暂运行 -> 自动关闭
    //   打包脚本在必要时可调用，验证 GUI 可开可关且布局未跑出屏幕
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--guitest") {
            QCoreApplication::setOrganizationName(QStringLiteral("CANable2-N32-guitest"));
            QCoreApplication::setApplicationName(QStringLiteral("canable2_guitest"));
            MainWindow w;
            w.show();
            w.raise();
            w.activateWindow();

            bool layoutOk = true;
            QString layoutMsg;
            if (QScreen *sc = w.screen() ? w.screen() : QGuiApplication::primaryScreen()) {
                const QRect avail = sc->availableGeometry();
                const QRect g = w.frameGeometry();
                if (!avail.intersects(g)) {
                    layoutOk = false;
                    layoutMsg = QStringLiteral("window outside available screen");
                } else if (w.width() < 400 || w.height() < 300) {
                    layoutOk = false;
                    layoutMsg = QStringLiteral("window too small: %1x%2")
                                    .arg(w.width()).arg(w.height());
                } else {
                    layoutMsg = QStringLiteral("geom=%1,%2 %3x%4 avail=%5x%6")
                                    .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                                    .arg(avail.width()).arg(avail.height());
                }
            } else {
                layoutOk = false;
                layoutMsg = QStringLiteral("no primary screen");
            }
            std::fputs(("GUITEST layout: " + layoutMsg + "\n").toLocal8Bit().constData(), stdout);

            QTimer::singleShot(1500, &app, [&]() {
                w.close();
                app.quit();
            });
            const int rc = app.exec();
            const bool ok = (rc == 0) && layoutOk;
            std::fputs(ok ? "GUITEST: OK\n" : "GUITEST: FAIL\n", stdout);
            return ok ? 0 : 1;
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
