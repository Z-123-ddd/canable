// CANable2-N32 上位机 — 程序入口
#include <QApplication>
#include <QClipboard>
#include <QItemSelection>
#include <QStandardItemModel>
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
#include <QSplitter>
#include <QGroupBox>
#include <QAction>
#include <QMenuBar>
#include <QMenu>
#include <QEvent>
#include <QKeyEvent>
#include <cstdio>
#include "HexFormat.h"
#include "MainWindow.h"
#include "FrameParse.h"
#include "Protocol.h"
#include "SimpleProto.h"
#include "CanOpenProto.h"
#include <QDir>
#include <QFile>
#include "TraceImporter.h"
#include "CommandClipboardDialog.h"
#include "ServoDs402.h"
#include "EsiXml.h"
#include "EtherCatLink.h"
#include <joint_sdk/joint_sdk.h>
#include "dfu_device.h"
#include "VendorDfuFlasher.h"
#include "UsbLink.h"

// 无头 UI 逻辑自测：
//  - 排版：发送表默认 20 行、行号列、选择列无表头、默认未勾选
//  - 发送：单条发送(编辑区)/列表发送(勾选行)两个按钮、仅数据变化才打包、一次发送只写一次
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
    const QString list1  = w.doSendList();                     // 列表发送（勾选 2 行）
    const QByteArray listFlush = w.lastTxFlush();
    const int scrollBefore2 = scroll ? scroll->rowCount() : 0;
    const QString list2  = w.doSendList();                     // 数据未变化 → 只发 TX_START
    const QByteArray listFlush2 = w.lastTxFlush();
    const int scrollAfter2 = scroll ? scroll->rowCount() : 0;
    // 数据未变化：滚动区仍打印要发送的帧，但实际只下发 TX_START 命令（未打包数据帧）
    const bool echoUnchangedOk =
        (scrollAfter2 - scrollBefore2) == 2 &&
        (listFlush2 == QByteArray::fromHex("AAAA0200000000"));

    // 单条发送：取单帧编辑区（id=300, data=01 02）
    const QString single1 = w.doSendSingle();
    // 清空勾选后"列表发送" → 无可发送数据（不写串口）
    table->item(0, C_CHK)->setCheckState(Qt::Unchecked);
    table->item(1, C_CHK)->setCheckState(Qt::Unchecked);
    const QString listEmpty = w.doSendList();

    // 日志区不再出现 TX 数据/命令 hex（仅告警/报错）
    const QString log = w.logText();
    const bool logClean = !log.contains(QStringLiteral("TX ")) &&
                          !log.contains(QStringLiteral("AA AB")) &&
                          !log.contains(QStringLiteral("AA AA"));

    const int flushTotal = w.txFlushCount() - flush0;   // 期望 3（两次列表 + 一次单条）
    bool bundle1 = listFlush.startsWith(QByteArray::fromHex("AAAA07"))
                   && listFlush.endsWith(QByteArray::fromHex("AAAA0200000000"))
                   && listFlush.contains(QByteArray::fromHex("AAAA06"))
                   && listFlush.size() == 51;
    bool bundle2 = (listFlush2 == QByteArray::fromHex("AAAA0200000000"));

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

    // ---- 接收区数据导出：buildRxCsv 产出表头 + 行 ----
    bool rxExportOk = false;
    {
        const QVector<ClipRecord> recs = w.rxTableRecords(rxTable);
        bool found1 = false, found2 = false;
        for (const ClipRecord &r : recs) {
            if (r.id == 0x1u && r.data == QByteArray::fromHex("030405")) found1 = true;
            if (r.id == 0x2u && r.data == QByteArray::fromHex("AABB"))  found2 = true;
        }
        rxExportOk = (recs.size() == 2) && found1 && found2;
        std::fputs(QStringLiteral("UITEST rxExport ok=%1 n=%2\n")
                       .arg(rxExportOk ? 1 : 0).arg(recs.size())
                       .toLocal8Bit().constData(), stdout);
    }


    // ---- 接收区存储上限：已提升到 ≈10MB（150 条远未达上限，应全部保留）----
    bool rxCapOk = false;
    {
        MainWindow wCap;
        for (int i = 0; i < 150; ++i) {          // 喂 150 个不同帧ID
            proto::RxFrame f;
            f.id = (quint32)(0x100 + i);
            f.data = QByteArray::fromHex("01");
            wCap.simulateRx(f);
        }
        // 接收缓存上限已提升到 ≈10MB：150 条远未达上限，应全部保留
        rxCapOk = (wCap.rxRowCount() == 150) &&
                  wCap.rxCellText(0, 0) == QStringLiteral("0X100") &&
                  wCap.rxCellText(149, 0) == QStringLiteral("0X195");
    }

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

    // ---- 命令剪切栏：文本格式 + CSV/ASC/candump/TRC 导出（并回读校验）----
    bool clipOk = false;
    {
        QVector<ClipRecord> recs;
        ClipRecord r1; r1.id = 0x100; r1.extended = false; r1.data = QByteArray::fromHex("11223344");
        ClipRecord r2; r2.id = 0x18FF50E5; r2.extended = true; r2.data = QByteArray::fromHex("AABBCCDD");
        recs << r1 << r2;

        const QString pipe = CommandClipboardDialog::pipeText(recs);
        const QStringList pl = pipe.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString clipCsv = CommandClipboardDialog::exportText(recs, CommandClipboardDialog::Csv);
        clipOk = pl.size() == 2
                 && pl[0] == QStringLiteral("0x100|11 22 33 44|标准")
                 && pl[1] == QStringLiteral("0x18FF50E5|AA BB CC DD|扩展")
                 && clipCsv.contains(QStringLiteral("序号,帧ID,帧格式,帧类型,数据长度,数据"))
                 && clipCsv.contains(QStringLiteral("0x100,标准帧,CAN,4,11 22 33 44"))
                 && clipCsv.contains(QStringLiteral("0x18FF50E5,扩展帧,CAN,4,AA BB CC DD"));

        // 回读校验：CSV / ASC / candump / TRC 均可被 TraceImporter 解析回同样的帧
        const QString dir = QDir::tempPath();
        auto roundTrip = [&](CommandClipboardDialog::Format fmt) -> bool {
            const QString path = dir + QStringLiteral("/clip_rt.") +
                                 CommandClipboardDialog::defaultExt(fmt);
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            f.write(CommandClipboardDialog::exportText(recs, fmt).toUtf8());
            f.close();
            const trace::Result res = trace::load(path);
            if (!res.ok || res.frames.size() != 2) return false;
            bool okk = true;
            for (int i = 0; i < 2; ++i) {
                const ClipRecord &a = recs[i];
                const trace::Frame &b = res.frames[i];
                if (a.id != b.id || a.extended != b.extended || a.data != b.data)
                    okk = false;
            }
            return okk;
        };
        const bool rtCsv  = roundTrip(CommandClipboardDialog::Csv);
        const bool rtAsc  = roundTrip(CommandClipboardDialog::Asc);
        const bool rtTrc  = roundTrip(CommandClipboardDialog::Trc);
        const bool rtCand = roundTrip(CommandClipboardDialog::Candump);
        clipOk = clipOk && rtCsv && rtAsc && rtTrc && rtCand;

        // 选择式列表窗口：默认全选；清空后 selectedRecords 为空；可单选/范围选择
        {
            CommandClipboardDialog dlg(recs);
            auto *tb = dlg.findChild<QTableWidget *>("clipTable");
            const bool defAll = tb && dlg.selectedRecords().size() == 2;
            QMetaObject::invokeMethod(&dlg, "selectNone", Qt::DirectConnection);
            const bool none = dlg.selectedRecords().isEmpty();
            bool one = false, range = false;
            if (tb) {
                tb->selectRow(0);                          // 单击选中一行
                one = dlg.selectedRecords().size() == 1;
                // 模拟 Shift+点击范围：选中 0..1
                if (tb->selectionModel())
                    tb->selectionModel()->select(
                        QItemSelection(tb->model()->index(0, 0), tb->model()->index(1, 0)),
                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                range = dlg.selectedRecords().size() == 2;
            }
            qApp->clipboard()->clear();
            QMetaObject::invokeMethod(&dlg, "copyToClipboard", Qt::DirectConnection);
            const bool copiedSel = qApp->clipboard()->text().contains(QStringLiteral("0x100|11 22 33 44|标准"));
            clipOk = clipOk && defAll && none && one && range && copiedSel;
        }

        std::fputs(QStringLiteral("UITEST clip ok=%1 csv=%2 asc=%3 trc=%4 candump=%5\n")
                       .arg(clipOk ? 1 : 0).arg(rtCsv ? 1 : 0).arg(rtAsc ? 1 : 0)
                       .arg(rtTrc ? 1 : 0).arg(rtCand ? 1 : 0)
                       .toLocal8Bit().constData(), stdout);
    }

    bool ok = list1.contains(QStringLiteral("changed=1")) && list1.contains(QStringLiteral("列表发送"))
              && list2.contains(QStringLiteral("changed=0")) && echoUnchangedOk
              && single1.contains(QStringLiteral("changed=1")) && single1.contains(QStringLiteral("单条发送"))
              && listEmpty.contains(QStringLiteral("empty"))
              && logClean
              && flushTotal == 3 && bundle1 && bundle2
              && rxRows == 2 && rxOk && noGrid && csvOk && rxCapOk && clipOk && rxExportOk;

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
            // EtherCAT 页关键控件存在（统一走 joint-sdk）+ 适配器扫描不崩溃
            auto *ecAd = w.findChild<QComboBox *>("ecAdapterBox");
            auto *ecJOpen = w.findChild<QPushButton *>("jointOpenBtn");
            if (!(ecAd && ecJOpen)) modeSwitchOk = false;
        }
        QString line = QStringLiteral("UITEST menu ok=%1 modeSwitch=%2")
                           .arg(menuOk ? 1 : 0).arg(modeSwitchOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
    }
    ok = ok && menuOk && modeSwitchOk;

    // ---- EtherCAT：统一走厂商 joint-sdk（无裸 SOEM 控件）+ DS402 状态机 ----
    bool ecServoOk = false;
    {
        auto *pageEc   = w.findChild<QWidget *>("pageEtherCat");
        auto *xmlBtn   = w.findChild<QPushButton *>("ecImportXmlBtn");
        auto *objBox   = w.findChild<QComboBox *>("ecObjBox");
        auto *sdoR     = w.findChild<QPushButton *>("ecSdoReadBtn");
        auto *sdoW     = w.findChild<QPushButton *>("ecSdoWriteBtn");
        auto *jOpen    = w.findChild<QPushButton *>("jointOpenBtn");
        auto *jAct     = w.findChild<QPushButton *>("jointActivateBtn");
        auto *jEn      = w.findChild<QPushButton *>("jointEnableBtn");
        auto *jCmd     = w.findChild<QComboBox *>("jointModeBox");
        auto *jStat    = w.findChild<QLabel *>("jointStatusLbl");
        // 裸 SOEM 控件应已移除
        const bool rawGone = !w.findChild<QPushButton *>("ecScanBtn")
                          && !w.findChild<QPushButton *>("ecOpBtn")
                          && !w.findChild<QPushButton *>("ecServoEnableBtn");
        ecServoOk = pageEc && xmlBtn && objBox && sdoR && sdoW &&
                    jOpen && jAct && jEn && jCmd && jStat && rawGone &&
                    jCmd->currentData().toInt() == 8;          // 关节默认 CSP=8

        // DS402 状态机（纯逻辑）
        using ds402::State;
        ecServoOk = ecServoOk &&
            ds402::decode(0x0040) == State::SwitchOnDisabled &&   // 0x0040
            ds402::decode(0x0021) == State::ReadyToSwitchOn &&    // 0x0021
            ds402::decode(0x0023) == State::SwitchedOn &&         // 0x0023
            ds402::decode(0x0027) == State::OperationEnabled &&   // 0x0027
            ds402::decode(0x0008) == State::Fault &&              // 故障
            ds402::nextEnableControlword(0x0040) == 0x0006 &&     // Shutdown
            ds402::nextEnableControlword(0x0021) == 0x0007 &&     // Switch on
            ds402::nextEnableControlword(0x0023) == 0x000F &&     // Enable operation
            ds402::nextEnableControlword(0x0027) == 0x000F &&     // 保持
            ds402::nextEnableControlword(0x0008) == 0x0080 &&     // Fault reset
            ds402::isEnabled(0x0027) && !ds402::isEnabled(0x0023) &&
            ds402::isFault(0x0008) && !ds402::isFault(0x0027);

        std::fputs(QStringLiteral("UITEST ecServo ok=%1\n").arg(ecServoOk ? 1 : 0)
                       .toLocal8Bit().constData(), stdout);

        // 未选网卡时「打开+配置」应安全返回（不卡死、不崩溃）
        if (jOpen) jOpen->click();
    }
    ok = ok && ecServoOk;

    // ---- 固件版本比对（关于）：设备版本与内置最新固件比对 ----
    bool fwCmpOk = false;
    {
        fwCmpOk = MainWindow::firmwareStatusNote(QStringLiteral("v30.0.5-keil")) == QStringLiteral("（固件最新）")
                && MainWindow::firmwareStatusNote(QStringLiteral("30.0.5"))       == QStringLiteral("（固件最新）")
                && MainWindow::firmwareStatusNote(QStringLiteral("v30.0.4-abc"))  == QStringLiteral("（固件可更新）")
                && MainWindow::firmwareStatusNote(QStringLiteral("")).isEmpty();
        std::fputs(QStringLiteral("UITEST fwVersion ok=%1\n").arg(fwCmpOk ? 1 : 0)
                       .toLocal8Bit().constData(), stdout);
    }
    ok = ok && fwCmpOk;

    // ---- 列表窗口高度可调：每个含列表的页都有 [列表|日志] 垂直分隔条，日志最少一行 ----
    bool listSplitOk = false;
    {
        const QList<QSplitter *> sps = w.findChildren<QSplitter *>(QStringLiteral("listLogSplit"));
        listSplitOk = sps.size() >= 2;                 // 通用发送列表 + CANopen 命令列表
        for (QSplitter *sp : sps) {
            if (sp->orientation() != Qt::Vertical || sp->count() != 2) { listSplitOk = false; break; }
            if (!w.findChild<QPlainTextEdit *>("log") &&
                !w.findChild<QPlainTextEdit *>("log3")) { /* 日志存在即可 */ }
        }
        // 日志的最小高度应≈一行（不会被拖动压没）
        if (auto *lg = w.findChild<QPlainTextEdit *>(QStringLiteral("log")))
            listSplitOk = listSplitOk && lg->minimumHeight() > 0 &&
                          lg->minimumHeight() <= lg->fontMetrics().height() + 16;
        std::fputs(QStringLiteral("UITEST listSplit ok=%1 n=%2\n")
                       .arg(listSplitOk ? 1 : 0).arg(sps.size())
                       .toLocal8Bit().constData(), stdout);
    }
    ok = ok && listSplitOk;

    // ---- ESI XML 解析：对象字典 + RxPdo/TxPdo + 控制字/状态字偏移 ----
    bool esiOk = false;
    {
        const QByteArray xml =
            "<EtherCATInfo><Devices><Device><Name>TestDrive</Name>"
            "<Profile><Dictionary><Objects>"
            "<Object><Index>#x6040</Index><Name>Control Word</Name><Type>UINT</Type><BitSize>16</BitSize>"
            "<Flags><Access>rw</Access></Flags><Info><DefaultData>0600</DefaultData></Info></Object>"
            "<Object><Index>#x6060</Index><Name>Modes of operation</Name><Type>SINT</Type><BitSize>8</BitSize></Object>"
            "</Objects></Dictionary></Profile>"
            "<RxPdo Sm=\"2\"><Index>#x1600</Index><Name>Outputs</Name>"
            "<Entry><Index>#x6040</Index><SubIndex>0</SubIndex><BitLen>16</BitLen><Name>CW</Name></Entry>"
            "<Entry><Index>#x607A</Index><SubIndex>0</SubIndex><BitLen>32</BitLen><Name>Target</Name></Entry>"
            "</RxPdo>"
            "<TxPdo Sm=\"3\"><Index>#x1A00</Index><Name>Inputs</Name>"
            "<Entry><Index>#x6061</Index><SubIndex>0</SubIndex><BitLen>8</BitLen><Name>Mode</Name></Entry>"
            "<Entry><Index>#x6041</Index><SubIndex>0</SubIndex><BitLen>16</BitLen><Name>SW</Name></Entry>"
            "</TxPdo>"
            "</Device></Devices></EtherCATInfo>";
        EsiXmlData d; QString err;
        const bool parsed = parseEsiXml(xml, &d, &err);
        esiOk = parsed
                && d.objects.size() == 2
                && d.findObject(0x6040, 0) != nullptr
                && d.findObject(0x6040, 0)->name == QStringLiteral("Control Word")
                && d.findObject(0x6040, 0)->def == QByteArray::fromHex("0600")
                && d.rxPdos.size() == 1 && d.txPdos.size() == 1
                && d.rxPdos.first().index == 0x1600 && d.rxPdos.first().sm == 2
                && d.rxPdos.first().entries.size() == 2
                && d.controlwordByteOffset() == 0     // 0x6040 是第一个 entry
                && d.statuswordByteOffset() == 1;     // 0x6041 在第 2 个 entry（前有 8bit → 1 字节）
        std::fputs(QStringLiteral("UITEST esi ok=%1 objs=%2 rx=%3 tx=%4 cw=%5 sw=%6\n")
                       .arg(esiOk ? 1 : 0).arg(d.objects.size()).arg(d.rxPdos.size())
                       .arg(d.txPdos.size()).arg(d.controlwordByteOffset()).arg(d.statuswordByteOffset())
                       .toLocal8Bit().constData(), stdout);

        // 可选：对真实 ESI 文件做一次解析（CANABLE_TEST_ESI=<path>）
        const QByteArray esiPath = qgetenv("CANABLE_TEST_ESI");
        if (!esiPath.isEmpty()) {
            EsiXmlData rd; QString rerr;
            const bool okF = loadEsiXml(QString::fromLocal8Bit(esiPath), &rd, &rerr);
            std::fputs(QStringLiteral("UITEST esi-file ok=%1 objs=%2 rx=%3 tx=%4 cw=%5 sw=%6 dev=%7\n")
                           .arg(okF ? 1 : 0).arg(rd.objects.size()).arg(rd.rxPdos.size())
                           .arg(rd.txPdos.size()).arg(rd.controlwordByteOffset()).arg(rd.statuswordByteOffset())
                           .arg(rd.device).toLocal8Bit().constData(), stdout);
            esiOk = esiOk && okF && !rd.objects.isEmpty() && !rd.rxPdos.isEmpty();
        }
    }
    ok = ok && esiOk;

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
        for (QPlainTextEdit *le : w.findChildren<QPlainTextEdit *>())
            if (le->objectName().startsWith(QStringLiteral("log"))) logs << le;   // 仅日志面板（排除解析区）
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

    // ---- 接收暂停 + 解析区 + CyberBeast 配置区 ----
    {
        bool pauseOk = false, parseOk = false, cbPageOk = false;

        // 暂停按钮：每个接收区一个；点击切换文本
        QVector<QPushButton *> pbs;
        for (QPushButton *b : w.findChildren<QPushButton *>())
            if (b->objectName() == QStringLiteral("pauseRxBtn")) pbs << b;
        pauseOk = (pbs.size() >= 5);
        if (!pbs.isEmpty()) {
            QPushButton *b = pbs.first();
            const QString t0 = b->text();
            b->click();                                  // 暂停
            const bool paused = (b->text() != t0);
            b->click();                                  // 恢复
            pauseOk = pauseOk && paused && (b->text() == t0);
        }

        // 解析区：4 个协议页各一份（表格：帧ID | 解析；按 ID 就地更新）
        auto *pcb = w.findChild<QTableWidget *>(QStringLiteral("parse_cyberbeast"));
        auto *psi = w.findChild<QTableWidget *>(QStringLiteral("parse_simple"));
        auto *pco = w.findChild<QTableWidget *>(QStringLiteral("parse_canopen"));
        auto *prs = w.findChild<QTableWidget *>(QStringLiteral("parse_rs485"));
        parseOk = (pcb && psi && pco && prs);

        // 解析正确性 + 就地更新：同一 ID 再来一帧不新增行
        if (parseOk && pcb) {
            proto::RxFrame f;
            f.ext = true;
            f.id = (2u<<26) | (0x00u<<18) | (0x05u<<10) | (0x03u<<2) | 1u;   // Pri=2 MIT Dest=5 Src=3
            f.data = QByteArray::fromHex("0010A0C000000000");
            w.simulateRx(f);
            const QString cbLine = frameParse::cyberBeast(f);
            const int rows1 = pcb->rowCount();
            w.simulateRx(f);                                   // 同 ID 再来一帧
            const int rows2 = pcb->rowCount();
            parseOk = parseOk && rows1 >= 1 && rows2 == rows1
                   && pcb->item(0, 1) && pcb->item(0, 1)->text().contains(QStringLiteral("MIT_CONTROL"))
                   && psi->rowCount() >= 1;
            (void)cbLine;
        }

        // CyberBeast 配置区控件
        cbPageOk = w.findChild<QComboBox *>(QStringLiteral("cbCatBox"))
                && w.findChild<QComboBox *>(QStringLiteral("cbCmdBox"))
                && w.findChild<QPushButton *>(QStringLiteral("cbSendBtn"))
                && w.findChild<QPushButton *>(QStringLiteral("cbStopBtn"))
                && w.findChild<QTableWidget *>(QStringLiteral("cbRxTable"))
                && w.findChild<QLineEdit *>(QStringLiteral("cbPreview"));

        QString line = QStringLiteral("UITEST pause/parse cb=%1 pause=%2 parse=%3")
                           .arg(cbPageOk ? 1 : 0).arg(pauseOk ? 1 : 0).arg(parseOk ? 1 : 0);
        line += QChar(10);
        std::fputs(line.toLocal8Bit().constData(), stdout);
        ok = ok && pauseOk && parseOk && cbPageOk;
    }

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
                    // RxSdo 实战示例：选中后同样复用 0x04 参数区（读/写/Endpoint 可配，不再写死）
                    int exIdx = -1;
                    for (int i = 0; i < cmdBox->count(); ++i)
                        if (cmdBox->itemText(i).contains(QStringLiteral("RxSdo"))) { exIdx = i; break; }
                    if (exIdx >= 0) {
                        cmdBox->setCurrentIndex(exIdx);
                        auto *opEx = w.findChild<QComboBox *>(QStringLiteral("sdoOpcodeBox"));
                        auto *epEx = w.findChild<QLineEdit *>(QStringLiteral("sdoEndpointEdit"));
                        simpleOk = simpleOk && opEx && epEx;
                    }
                    (void)0;
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
            // 新增：固定帧输入项（控制模式/输入模式/轴状态）渲染并可改值生效
            auto *ex0 = w.findChild<QLineEdit *>(QStringLiteral("simpleExVal0"));
            auto *ex1 = w.findChild<QLineEdit *>(QStringLiteral("simpleExVal1"));
            auto *ex2 = w.findChild<QLineEdit *>(QStringLiteral("simpleExVal2"));
            exampleOk = exampleOk && ex0 && ex1 && ex2 &&
                        ex0->text() == QStringLiteral("3") && ex1->text() == QStringLiteral("3") &&
                        ex2->text() == QStringLiteral("8");
            if (ex0) { ex0->setText(QStringLiteral("4"));
                       exampleOk = exampleOk && preview->text().contains(QStringLiteral("0x02B:04 00 00 00"));
                       ex0->setText(QStringLiteral("3")); }
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
        auto *sendOne = w.findChild<QPushButton *>(QStringLiteral("coSendSingleBtn"));
        auto *odB    = w.findChild<QPushButton *>(QStringLiteral("coOdBrowserBtn"));
        auto *rxT    = w.findChild<QTableWidget *>(QStringLiteral("coRxTable"));
        auto *scr    = w.findChild<QTableWidget *>(QStringLiteral("coScroll"));
        coOk = stack && page && nodeEd && funcBox && prev && sendB && sendOne && odB && rxT && scr &&
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
                if (v.isValid() && canopen::examples().at(v.toInt()).ids.last() == 0x400) exRow = r;
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
                        !canopen::examples().at(v.toInt()).inputs.isEmpty() &&
                        canopen::examples().at(v.toInt()).inputs.first().kind == canopen::CI_POS) posRow = r;
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
                // 力矩示例：新增 操作模式/输入模式 下拉（默认 10 / 6），改值生效
                int tqRow = -1;
                for (int r = 0; r < funcBox->count(); ++r) {
                    const QVariant v = funcBox->itemData(r, Qt::UserRole + 1);
                    if (v.isValid() && canopen::examples().at(v.toInt()).name.contains(QStringLiteral("力矩"))) tqRow = r;
                }
                if (tqRow >= 0) {
                    funcBox->setCurrentIndex(tqRow);
                    auto *opEx = w.findChild<QComboBox *>(QStringLiteral("coExUnit1"));
                    auto *inEx = w.findChild<QComboBox *>(QStringLiteral("coExUnit2"));
                    bool tqOk = opEx && inEx && opEx->currentData().toInt() == 10 &&
                                inEx->currentData().toInt() == 6 &&
                                inEx->count() == 10 &&
                                inEx->itemText(2).contains(QStringLiteral("速度斜坡")) &&
                                inEx->itemText(2).contains(QStringLiteral("(2)"));   // 2002h 完整枚举
                    if (opEx) { const int i = opEx->findData(9); if (i >= 0) opEx->setCurrentIndex(i);
                                tqOk = tqOk && prev->text().contains(QStringLiteral("2F 60 60 00 09 00 00 00")); }
                    coOk = coOk && tqOk;
                }
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
                if (v.isValid() && canopen::examples().at(v.toInt()).ids.last() == 0x400) exRow = r;
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
            canopen::examples().at(cb5->currentData(Qt::UserRole + 1).toInt()).ids.last() == 0x400;
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
                // 5) 单条发送：取参数框当前配置的命令，与命令列表选中无关
                if (coListOk) {
                    funcBox->setCurrentIndex(funcBox->findData((int)canopen::F_RSDO));
                    auto *idx5 = w.findChild<QComboBox *>(QStringLiteral("coSdoIdxBox"));
                    if (idx5 && idx5->findData((uint)(0x6040u << 8)) >= 0)
                        idx5->setCurrentIndex(idx5->findData((uint)(0x6040u << 8)));
                    if (auto *v5 = w.findChild<QLineEdit *>(QStringLiteral("coSdoValueEdit")))
                        v5->setText(QStringLiteral("6"));
                    // 故意把列表选中设成第 1 项(NMT)，单条发送应不受其影响
                    lst->setCurrentRow(0);
                    const QVector<QByteArray> oneFrames = w.coSingleCommandFrames();
                    bool oneOk = oneFrames.size() == 1;
                    if (oneOk) {
                        proto::RxFrame rx;
                        oneOk = proto::decodeDataFrame(oneFrames.first(), &rx) &&
                                rx.id == 0x601u;   // 参数框 RSDO 写 6040h 的 COB-ID
                    }
                    coListOk = coListOk && oneOk;

                    // SDO 写首字节应随对象数据类型变化（VAR 对象 + 记录子项）
                    auto *val5 = w.findChild<QLineEdit *>(QStringLiteral("coSdoValueEdit"));
                    auto *op5  = w.findChild<QComboBox *>(QStringLiteral("coSdoOpBox"));
                    if (idx5 && val5 && lst) {
                        auto firstByte = [&](quint16 obj, int sub, bool *okp) -> int {
                            const uint key = (uint)((obj << 8) | (sub & 0xFF));
                            if (idx5->findData(key) >= 0) idx5->setCurrentIndex(idx5->findData(key));
                            if (op5) op5->setCurrentIndex(0);   // 强制「写」以测命令码（与访问类型无关）
                            const QVector<QByteArray> fr = w.coSingleCommandFrames();
                            proto::RxFrame rx;
                            if (okp) *okp = (!fr.isEmpty() && proto::decodeDataFrame(fr.first(), &rx));
                            return (okp && *okp) ? (int)(quint8)rx.data.at(0) : -1;
                        };
                        bool o1=false,o2=false,o3=false,o4=false,o5=false,o6=false;
                        const int b6040 = firstByte(0x6040, 0, &o1);   // UINT16 → 0x2B
                        const int b6060 = firstByte(0x6060, 0, &o2);   // INT8   → 0x2F
                        const int b6071 = firstByte(0x6071, 0, &o3);   // INT16  → 0x2B
                        const int b608f0 = firstByte(0x608F, 0, &o4);  // UINT8  → 0x2F
                        const int b6098 = firstByte(0x6098, 0, &o5);   // ?
                        const int b2002 = firstByte(0x2002, 1, &o6);   // UINT16 → 0x2B
                        std::fputs(QStringLiteral("UITEST sdoFirstByte 6040=%1 6060=%2 6071=%3 608F0=%4 6098=%5 2002:1=%6\n")
                                       .arg(b6040).arg(b6060).arg(b6071).arg(b608f0).arg(b6098).arg(b2002)
                                       .toLocal8Bit().constData(), stdout);
                        // 首字节=命令码：UINT16/INT16→0x2B, INT8/UINT8→0x2F
                        coListOk = coListOk && o1 && o2 && o3 && o4 && o5 && o6 &&
                                   b6040 == 0x2B && b6060 == 0x2F && b6071 == 0x2B &&
                                   b608f0 == 0x2F && b6098 == 0x2F && b2002 == 0x2B;
                    }
                    // 按 EDS 访问类型约束读/写：ro(0x1001) 禁用「写」，rw(0x6040) 都可选
                    if (idx5 && op5) {
                        auto *mdl = qobject_cast<QStandardItemModel *>(op5->model());
                        if (idx5->findData((uint)(0x1001u << 8)) >= 0) {
                            idx5->setCurrentIndex(idx5->findData((uint)(0x1001u << 8)));
                            const bool roWriteOff = mdl && mdl->item(0) && !mdl->item(0)->isEnabled();
                            const bool roReadOn   = mdl && mdl->item(1) && mdl->item(1)->isEnabled();
                            const bool flipped    = op5->currentData().toInt() == 1;   // 自动切到「读」
                            coListOk = coListOk && roWriteOff && roReadOn && flipped;
                        }
                        if (idx5->findData((uint)(0x6040u << 8)) >= 0) {
                            idx5->setCurrentIndex(idx5->findData((uint)(0x6040u << 8)));
                            const bool rwBoth = mdl && mdl->item(0)->isEnabled() && mdl->item(1)->isEnabled();
                            coListOk = coListOk && rwBoth;
                        }
                        std::fputs(QStringLiteral("UITEST sdoAccess ro-disables-write/rw-both ok=%1\n")
                                       .arg(coListOk ? 1 : 0).toLocal8Bit().constData(), stdout);
                    }
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
                   .arg(QString::fromLatin1(listFlush.toHex(' ').toUpper()))
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

// 无头 EtherCAT 联调：host.exe --ectest <网卡名>  （需要管理员 + Npcap + 从站）
// 走完整 SOEM 时序：open → config_init → config_map(+blockLRW/DC) → SAFE-OP 预热 → OP，
// 逐步打印状态 / AL 状态码 / WKC，用于判断是上位机兼容问题还是命令格式问题。
static int runEcTest(const QString &iface)
{
    auto out = [](const QString &s) { std::fputs(s.toLocal8Bit().constData(), stdout); std::fputc('\n', stdout); };
    out(QStringLiteral("EC-TEST iface=%1").arg(iface));

    EtherCatLink ec;
    if (qgetenv("CANABLE_EC_NO_DC") == QByteArray("1")) ec.setUseDc(false);
    if (!ec.open(iface)) { out(QStringLiteral("EC-TEST: 打开网卡失败")); return 1; }
    const int n = ec.scanSlaves();
    out(QStringLiteral("EC-TEST slaves=%1").arg(n));
    if (n <= 0) { ec.close(); out(QStringLiteral("EC-TEST: 未发现从站（检查网线/链路/独占）")); return 1; }

    for (int i = 1; i <= n; ++i) {
        const EcSlaveInfo s = ec.slave(i);
        out(QStringLiteral("EC-TEST slave %1: %2 state=%3 AL=0x%4 O=%5 I=%6 vid=0x%7 pid=0x%8")
                .arg(i).arg(s.name).arg(EtherCatLink::stateName(s.state))
                .arg(s.alStatus, 4, 16, QLatin1Char('0')).arg(s.obits).arg(s.ibits)
                .arg(s.vendorId, 8, 16, QLatin1Char('0')).arg(s.productId, 8, 16, QLatin1Char('0')));
    }

    ec.mapProcessData();   // 含 blockLRW + DC 配置
    out(QStringLiteral("EC-TEST mapped(1): expectedWkc=%1 slaveO=%2 slaveI=%3 slaveState=%4")
            .arg(ec.expectedWkc()).arg(ec.slave(1).obits).arg(ec.slave(1).ibits)
            .arg(EtherCatLink::stateName(ec.slaveState(1))));

    // 诊断/修复：该驱动冷启动 0x1C12:00=0（无 RxPDO 分配）→ 主站映射为空 → 停在 PRE/SAFE-OP。
    // 关键：PDO 分配只能在 PRE-OP 改（映射后处于 SAFE-OP，写不进）。先回 PRE-OP 再写，然后重新映射。
    {
        ec.requestState(EtherCatLink::ECST_PREOP, 3000);
        QString we;
        ec.sdoWriteUInt(1, 0x1C12, 0x00, 1, 0,      &we);
        ec.sdoWriteUInt(1, 0x1C12, 0x01, 2, 0x1600, &we);
        ec.sdoWriteUInt(1, 0x1C12, 0x00, 1, 1,      &we);
        ec.sdoWriteUInt(1, 0x1C13, 0x00, 1, 0,      &we);
        ec.sdoWriteUInt(1, 0x1C13, 0x01, 2, 0x1A00, &we);
        ec.sdoWriteUInt(1, 0x1C13, 0x00, 1, 1,      &we);
        ec.sdoWriteUInt(1, 0x6060, 0x00, 1, 8,      &we);   // CSP
        quint32 v = 0;
        ec.sdoReadUInt(1, 0x1C12, 0, 1, &v, &we);
        out(QStringLiteral("EC-TEST PRE-OP 写入后 0x1C12:00 = %1").arg(int(v)));
    }
    ec.mapProcessData();   // 重新映射（此时应得到非零 O/I）
    {
        const EcSlaveInfo s = ec.slave(1);
        out(QStringLiteral("EC-TEST mapped(2): expectedWkc=%1 slaveO=%2 slaveI=%3")
                .arg(ec.expectedWkc()).arg(s.obits).arg(s.ibits));
    }

    const bool op = ec.requestState(EtherCatLink::ECST_OP, 5000);
    out(QStringLiteral("EC-TEST request OP: ok=%1 masterState=%2")
            .arg(op ? 1 : 0).arg(EtherCatLink::stateName(ec.currentState())));
    for (int i = 1; i <= n; ++i)
        out(QStringLiteral("EC-TEST slave %1 after: state=%2 AL=0x%3")
                .arg(i).arg(EtherCatLink::stateName(ec.slaveState(i)))
                .arg(ec.slaveAlStatus(i), 4, 16, QLatin1Char('0')));

    // 分步：显式 SAFE-OP → 打印 → OP → 打印（定位卡在哪一级）
    ec.requestState(EtherCatLink::ECST_SAFEOP, 3000);
    out(QStringLiteral("EC-TEST after SAFE-OP: slaveState=%1 AL=0x%2")
            .arg(EtherCatLink::stateName(ec.slaveState(1)))
            .arg(ec.slaveAlStatus(1), 4, 16, QLatin1Char('0')));
    const bool op2 = ec.requestState(EtherCatLink::ECST_OP, 8000);
    out(QStringLiteral("EC-TEST after OP(2): ok=%1 slaveState=%2 AL=0x%3")
            .arg(op2 ? 1 : 0).arg(EtherCatLink::stateName(ec.slaveState(1)))
            .arg(ec.slaveAlStatus(1), 4, 16, QLatin1Char('0')));

    for (int k = 0; k < 5; ++k) {
        ec.sendProcessData();
        const int wkc = ec.receiveProcessData(2000);
        out(QStringLiteral("EC-TEST WKC=%1 / 期望 %2").arg(wkc).arg(ec.expectedWkc()));
    }

    ec.close();
    out(op ? QStringLiteral("EC-TEST: OP OK") : QStringLiteral("EC-TEST: OP FAILED"));
    return op ? 0 : 1;
}

// 用厂商 joint-sdk 走完整生命周期（configure→activate=OP）做无头验证：
//   host.exe --jointtest <网卡名> [esi.xml]
static int runJointTest(const QString &iface, const QString &esi)
{
    auto out = [](const QString &s) { std::fputs(s.toLocal8Bit().constData(), stdout); std::fputc('\n', stdout); };
    out(QStringLiteral("JOINT-TEST iface=%1 esi=%2").arg(iface, esi.isEmpty() ? QStringLiteral("(builtin)") : esi));

    // 运行期数据（dc_timing.conf / ESI）拷贝在 exe 旁 joint_sdk/ 下
    const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/joint_sdk");
    qputenv("JSDK_DC_TIMING_CONF", (base + QStringLiteral("/config/dc_timing.conf")).toLocal8Bit());

    jsdk_context_config_t cfg; jsdk_context_config_default(&cfg);
    std::snprintf(cfg.ifname, sizeof(cfg.ifname), "%s", iface.toLocal8Bit().constData());
    jsdk_context_t *ctx = jsdk_context_create(&cfg);
    if (!ctx) { out(QStringLiteral("JOINT-TEST: context_create 失败（网卡名/权限？）")); return 1; }

    jsdk_joint_config_t jc; jsdk_joint_config_default(&jc);
    jc.position = 1;
    const QByteArray esiBa = esi.toLocal8Bit();
    jc.profile_name = esi.isEmpty() ? nullptr : esiBa.constData();
    jsdk_joint_t *joint = nullptr;
    if (jsdk_context_add_joint(ctx, &jc, &joint) != JSDK_OK) {
        out(QStringLiteral("JOINT-TEST: add_joint 失败：%1").arg(QString::fromLocal8Bit(jsdk_context_last_error(ctx))));
        jsdk_context_destroy(ctx); return 1;
    }

    if (jsdk_context_configure(ctx) != JSDK_OK) {
        out(QStringLiteral("JOINT-TEST: configure 失败：%1").arg(QString::fromLocal8Bit(jsdk_context_last_error(ctx))));
        jsdk_context_destroy(ctx); return 1;
    }
    out(QStringLiteral("JOINT-TEST: configured (SAFE_OP)"));

    if (jsdk_context_activate(ctx) != JSDK_OK) {
        out(QStringLiteral("JOINT-TEST: activate(OP) 失败：%1").arg(QString::fromLocal8Bit(jsdk_context_last_error(ctx))));
        jsdk_context_destroy(ctx); return 1;
    }
    out(QStringLiteral("JOINT-TEST: activated (OPERATIONAL)"));

    // 跑若干周期，读反馈
    jsdk_joint_feedback_t fb{};
    for (int i = 0; i < 50; ++i) {
        jsdk_context_cycle_begin(ctx, (uint64_t)i);
        jsdk_joint_get_feedback(joint, &fb);
        jsdk_context_cycle_end(ctx);
    }
    jsdk_bus_state_t bus{};
    jsdk_context_get_bus_state(ctx, &bus);
    out(QStringLiteral("JOINT-TEST: fb status=0x%1 mode=%2 pos=%3 online=%4 op=%5 al=%6 | WKC=%7")
            .arg(fb.statusword, 4, 16, QLatin1Char('0')).arg(fb.mode_display).arg(fb.actual_position)
            .arg(fb.online).arg(fb.operational).arg(fb.al_state).arg(bus.working_counter));

    jsdk_context_deactivate(ctx);
    jsdk_context_destroy(ctx);
    out(QStringLiteral("JOINT-TEST: OP OK"));
    return 0;
}

int main(int argc, char *argv[])
{    // 协议自检：host.exe --selftest  （不开窗口，跑完即退）
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
            return runUiTest();    }

    // EtherCAT 无头联调：host.exe --ectest <网卡名>
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--ectest") {
            const QString iface = (i + 1 < argc) ? QString::fromLocal8Bit(argv[i + 1]) : QString();
            if (iface.isEmpty()) { std::fputs("usage: --ectest <pcap-device-name>\n", stdout); return 1; }
            return runEcTest(iface);
        }
    }

    // 厂商 joint-sdk 生命周期验证：host.exe --jointtest <网卡名> [esi.xml]
    for (int i = 1; i < argc; i++) {
        if (QString::fromLocal8Bit(argv[i]) == "--jointtest") {
            const QString iface = (i + 1 < argc) ? QString::fromLocal8Bit(argv[i + 1]) : QString();
            const QString esi   = (i + 2 < argc) ? QString::fromLocal8Bit(argv[i + 2]) : QString();
            if (iface.isEmpty()) { std::fputs("usage: --jointtest <pcap-device-name> [esi.xml]\n", stdout); return 1; }
            return runJointTest(iface, esi);
        }
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
