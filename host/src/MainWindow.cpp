#include "MainWindow.h"
#include "TraceImporter.h"
#include "SerialLink.h"
#include "UsbLink.h"
#include "FirmwareDialog.h"
#include "dfu_device.h"
#include "SimpleProto.h"
#include "HexFormat.h"
#include "CanOpenProto.h"
#include "ObjectDictionaryDialog.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QSerialPortInfo>
#include <QTime>
#include <QSignalBlocker>
#include <QFontMetrics>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QMessageBox>
#include <QCloseEvent>
#include <QToolButton>
#include <QScrollBar>
#include <QFont>
#include <QSettings>
#include <QEvent>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QKeySequence>
#include <QActionGroup>
#include <QStackedWidget>
#include <QListWidget>
#include <QSize>

// ---------------------------------------------------------------------------
//  CAN 波特率下拉项（显示名 -> Hz）
// ---------------------------------------------------------------------------
struct BaudItem { const char *name; quint32 hz; };
static const BaudItem kBauds[] = {
    {"10 kbit/s",   10000},
    {"20 kbit/s",   20000},
    {"50 kbit/s",   50000},
    {"83.3 kbit/s", 83333},
    {"100 kbit/s",  100000},
    {"125 kbit/s",  125000},
    {"250 kbit/s",  250000},
    {"500 kbit/s",  500000},
    {"750 kbit/s",  750000},
    {"1 Mbit/s",    1000000},
};

// CAN FD 数据域（数据段）可选波特率：默认 5M
static const BaudItem kDataBauds[] = {
    {"2 Mbit/s",    2000000},
    {"5 Mbit/s",    5000000},
};

// 帧类型编码（用于下拉 data）
enum FrameType { FT_CLASSIC = 0, FT_FD = 1, FT_FD_BRS = 2 };

// 发送表列：行号 | 选择 | 帧ID | 标准/扩展 | 发送字节长度 | 帧类型 | 数据
enum Col { COL_NO = 0, COL_CHK, COL_ID, COL_IDTYPE, COL_LEN, COL_FRAMETYPE, COL_DATA, COL_COUNT };

// 接收表列：帧ID | 类型 | 长度 | 数据 | 次数
enum RxCol { RX_ID = 0, RX_TYPE, RX_LEN, RX_DATA, RX_COUNT, RX_COL_COUNT };

// 滚动视图列：方向 | 帧ID | 类型 | 长度 | 数据
enum ScrollCol { SC_DIR = 0, SC_ID, SC_TYPE, SC_LEN, SC_DATA, SC_COL_COUNT };

// 发送字节长度可选值（DLC 字节数）
static const int kLenValues[] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};

static QComboBox *makeIdTypeBox()
{
    auto *b = new QComboBox;
    b->addItem(QStringLiteral("标准帧"), 0);
    b->addItem(QStringLiteral("扩展帧"), 1);
    return b;
}
static QComboBox *makeFrameTypeBox()
{
    auto *b = new QComboBox;
    b->addItem(QStringLiteral("CAN"),        FT_CLASSIC);
    b->addItem(QStringLiteral("CAN FD"),     FT_FD);
    b->addItem(QStringLiteral("CAN FD加速"), FT_FD_BRS);
    return b;
}
static QComboBox *makeLenBox()
{
    auto *b = new QComboBox;
    for (int v : kLenValues)
        b->addItem(QString::number(v), v);
    b->setCurrentIndex(8);   // 默认 8 字节
    return b;
}

// 输入框高度 = 1.5 倍行高（增大 0.5 倍）
static int tallerHeight(const QWidget *w, int lines = 1)
{
    return w->fontMetrics().height() * 3 / 2 * lines + 8;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("CyberBeast CANable 2"));

    auto *pageCan = new QWidget;                 // CAN 页（外层，容纳协议调试页栈）
    pageCan->setObjectName("pageCan");
    auto *canContent = new QWidget;              // 现有通用 CAN 调试 UI（= 协议菜单的“通用CAN”页）
    canContent->setObjectName("pageProtoGeneric");
    auto *root = new QHBoxLayout(canContent);    // 左右布局

    m_serial = new SerialLink(this);
    m_usb    = new UsbLink(this);
    m_link   = m_serial;                 // 默认串口
    for (Link *lk : { (Link *)m_serial, (Link *)m_usb }) {
        connect(lk, &Link::opened,             this, &MainWindow::onPortOpened);
        connect(lk, &Link::closed,             this, &MainWindow::onPortClosed);
        connect(lk, &Link::errorOccurred,      this, &MainWindow::onSerialError);
        connect(lk, &Link::frameReceived,      this, &MainWindow::onFrameReceived);
        connect(lk, &Link::responseReceived,   this, &MainWindow::onResponseReceived);
        connect(lk, &Link::statusLineReceived, this, &MainWindow::onStatusLine);
        connect(lk, &Link::logMessageReceived,  this, &MainWindow::onLogMessage);
    }

    // ======================= 左列：发送 =======================
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);

    // ---------------- 连接行（每页各一份，状态共享；[0] 即本页=规范）----------------
    lv->addWidget(buildConnGroup());

    // ---------------- CAN 参数行 ----------------
    auto *paramBox = new QGroupBox(QStringLiteral("CAN 参数"));
    auto *ph = new QHBoxLayout(paramBox);
    ph->addWidget(new QLabel(QStringLiteral("仲裁域波特率:")));
    m_bitrateBox = new QComboBox;                // 仲裁段波特率（命令 1）
    m_bitrateBox->setObjectName("arbBitrateBox");
    for (const auto &b : kBauds)
        m_bitrateBox->addItem(QString::fromUtf8(b.name), b.hz);
    m_bitrateBox->setCurrentIndex(9);             // 默认 1 Mbit/s
    ph->addWidget(m_bitrateBox);
    ph->addWidget(new QLabel(QStringLiteral("数据域波特率:")));
    m_dataBitrateBox = new QComboBox;            // CAN FD 数据段波特率（命令 8）
    m_dataBitrateBox->setObjectName("dataBitrateBox");
    for (const auto &b : kDataBauds)
        m_dataBitrateBox->addItem(QString::fromUtf8(b.name), b.hz);
    m_dataBitrateBox->setCurrentIndex(1);         // 默认 5 Mbit/s
    ph->addWidget(m_dataBitrateBox);
    m_loopBox = new QCheckBox(QStringLiteral("循环发送"));
    m_loopBox->setObjectName("loopBox");
    ph->addWidget(m_loopBox);
    ph->addWidget(new QLabel(QStringLiteral("周期(ms):")));
    m_intervalSpin = new QSpinBox;
    m_intervalSpin->setRange((int)proto::INTERVAL_MIN_MS, (int)proto::INTERVAL_MAX_MS);
    m_intervalSpin->setValue(100);
    ph->addWidget(m_intervalSpin);
    ph->addStretch();
    lv->addWidget(paramBox);

    // ---------------- 单帧编辑区 ----------------
    auto *frameBox = new QGroupBox(QStringLiteral("单帧编辑"));
    auto *fg = new QGridLayout(frameBox);

    fg->addWidget(new QLabel(QStringLiteral("ID(hex):")), 0, 0);
    m_idEdit = new QLineEdit;                       // 默认空
    m_idEdit->setObjectName("idEdit");
    m_idEdit->setMinimumWidth(140);
    m_idEdit->setMinimumHeight(tallerHeight(m_idEdit));   // 高度 ×1.5
    fg->addWidget(m_idEdit, 0, 1);

    fg->addWidget(new QLabel(QStringLiteral("ID类型:")), 0, 2);
    m_idTypeBox = makeIdTypeBox();
    m_idTypeBox->setObjectName("idTypeBox");
    fg->addWidget(m_idTypeBox, 0, 3);

    fg->addWidget(new QLabel(QStringLiteral("发送字节长度:")), 0, 4);
    m_lenBox = makeLenBox();
    m_lenBox->setObjectName("lenBox");
    fg->addWidget(m_lenBox, 0, 5);

    fg->addWidget(new QLabel(QStringLiteral("帧类型:")), 0, 6);
    m_frameTypeBox = makeFrameTypeBox();
    m_frameTypeBox->setObjectName("frameTypeBox");
    fg->addWidget(m_frameTypeBox, 0, 7);

    fg->addWidget(new QLabel(QStringLiteral("数据(hex):")), 1, 0);
    m_dataEdit = new HexLineEdit;                   // 默认空（自动大写 + 每字节空格）
    m_dataEdit->setObjectName("dataEdit");
    m_dataEdit->setMinimumWidth(8 * 3 * 8);         // 能完整显示 >=8 字节
    m_dataEdit->setMinimumHeight(tallerHeight(m_dataEdit));   // 高度 ×1.5
    m_dataEdit->setClearButtonEnabled(true);
    fg->addWidget(m_dataEdit, 1, 1, 1, 7);

    lv->addWidget(frameBox);

    // ---------------- 发送列表（表格） ----------------
    auto *listBox = new QGroupBox(QStringLiteral("发送列表 (勾选决定发送：≥2 列表 / 1 单帧 / 0 用编辑区；最多 20 行)"));
    auto *llv = new QVBoxLayout(listBox);
    m_table = new QTableWidget(0, COL_COUNT);
    m_table->setObjectName("table");
    m_table->setHorizontalHeaderLabels({
        QString(), QString(), QStringLiteral("帧ID(hex)"),
        QStringLiteral("标准/扩展"), QStringLiteral("发送字节长度"),
        QStringLiteral("帧类型"), QStringLiteral("数据(hex)")});   // 行号/选择无表头文字
    m_table->horizontalHeader()->setSectionResizeMode(COL_DATA, QHeaderView::Stretch);
    for (int c = 0; c < COL_COUNT; c++)
        if (c != COL_DATA) m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setMinimumHeight(300);
    m_table->setItemDelegateForColumn(COL_DATA, new HexDelegate(this));   // 数据列：自动大写 + 每字节空格
    resetTable();                 // 默认 20 行空表格
    llv->addWidget(m_table);

    auto *lb = new QHBoxLayout;
    m_resetBtn = new QPushButton(QStringLiteral("重置列表"));
    m_resetBtn->setObjectName("resetBtn");
    m_exportBtn = new QPushButton(QStringLiteral("导出CSV"));
    m_exportBtn->setObjectName("exportBtn");
    m_importBtn = new QPushButton(QStringLiteral("导入记录"));
    m_importBtn->setObjectName("importBtn");
    m_importBtn->setToolTip(QStringLiteral("导入其他 CAN 工具导出的记录(CSV/ASC/TRC/candump)到发送列表"));
    auto *hint = new QLabel(QStringLiteral("行内直接填写；勾选√决定发送行"));
    hint->setStyleSheet("color:#666;");
    lb->addWidget(m_resetBtn);
    lb->addWidget(m_importBtn);
    lb->addWidget(m_exportBtn);
    lb->addWidget(hint);
    lb->addStretch();
    llv->addLayout(lb);
    lv->addWidget(listBox);

    // ---------------- 操作 ----------------
    auto *opBox = new QGroupBox(QStringLiteral("操作"));
    auto *oh = new QHBoxLayout(opBox);
    m_sendBtn = new QPushButton(QStringLiteral("发送"));
    m_sendBtn->setObjectName("sendBtn");
    m_stopBtn = new QPushButton(QStringLiteral("停止"));
    m_statusBtn = new QPushButton(QStringLiteral("查询状态"));
    oh->addWidget(m_sendBtn);
    oh->addWidget(m_stopBtn);
    oh->addWidget(m_statusBtn);
    oh->addStretch();
    lv->addWidget(opBox);

    // ---------------- 日志（每页各一份，状态共享；[0] 即本页=规范）----------------
    lv->addWidget(buildLogGroup(), 1);

    root->addWidget(left, 3);

    // ======================= 右列：接收 =======================
    auto *rxBox = new QGroupBox(QStringLiteral("接收区"));
    auto *rv = new QVBoxLayout(rxBox);

    // 顶部：当前显示模式文字提示 + 切换图标
    auto *rtop = new QHBoxLayout;
    m_modeLbl = new QLabel;                         // 上方文本提示：当前模式
    m_modeLbl->setObjectName("modeLbl");
    m_modeBtn = new QToolButton;                    // 切换图标
    m_modeBtn->setObjectName("modeBtn");
    m_modeBtn->setText(QStringLiteral("\u21c4"));   // ⇄
    m_modeBtn->setToolTip(QStringLiteral("切换接收区显示模式"));
    m_modeBtn->setAutoRaise(true);
    {
        QFont f = m_modeBtn->font();
        f.setPointSize(f.pointSize() + 4);
        m_modeBtn->setFont(f);
    }
    m_tsBox = new QCheckBox(QStringLiteral("显示时间戳"));
    m_tsBox->setObjectName("tsBox");
    rtop->addWidget(m_modeLbl);
    rtop->addWidget(m_modeBtn);
    rtop->addSpacing(12);
    rtop->addWidget(m_tsBox);
    rtop->addStretch();
    rv->addLayout(rtop);

    m_rxTable = new QTableWidget(0, RX_COL_COUNT);
    m_rxTable->setObjectName("rxTable");
    m_rxTable->setHorizontalHeaderLabels({
        QStringLiteral("帧ID(hex)"), QStringLiteral("类型"),
        QStringLiteral("长度"), QStringLiteral("数据(hex)"), QStringLiteral("次数")});
    m_rxTable->horizontalHeader()->setSectionResizeMode(RX_DATA, QHeaderView::Stretch);
    for (int c = 0; c < RX_COL_COUNT; c++)
        if (c != RX_DATA) m_rxTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_rxTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_rxTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_rxTable->setShowGrid(false);                                   // 无边线
    m_rxTable->setFrameShape(QFrame::NoFrame);                       // 无外框
    m_rxTable->setStyleSheet("QTableWidget{gridline-color:transparent;}");
    m_rxTable->verticalHeader()->setVisible(false);
    m_rxTable->setMinimumWidth(420);
    rv->addWidget(makeRxFilterRow(QStringLiteral("can.rx"), m_rxTable,
        { QStringLiteral("帧ID"), QStringLiteral("类型"), QStringLiteral("长度"),
          QStringLiteral("数据"), QStringLiteral("次数") }));
    rv->addWidget(m_rxTable, 1);

    m_scrollTable = new QTableWidget(0, SC_COL_COUNT);   // 滚动显示：表头固定，数据行滚动追加
    m_scrollTable->setObjectName("scroll");
    m_scrollTable->setHorizontalHeaderLabels({
        QStringLiteral("方向"), QStringLiteral("帧ID(hex)"), QStringLiteral("类型"),
        QStringLiteral("长度"), QStringLiteral("数据(hex)")});
    m_scrollTable->horizontalHeader()->setSectionResizeMode(SC_DATA, QHeaderView::Stretch);
    for (int c = 0; c < SC_COL_COUNT; c++)
        if (c != SC_DATA) m_scrollTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_scrollTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_scrollTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_scrollTable->setShowGrid(false);
    m_scrollTable->setFrameShape(QFrame::NoFrame);
    m_scrollTable->setStyleSheet("QTableWidget{gridline-color:transparent;}");
    m_scrollTable->verticalHeader()->setVisible(false);
    m_scrollTable->setMinimumWidth(420);
    m_scrollTable->setVisible(false);
    rv->addWidget(makeRxFilterRow(QStringLiteral("can.scroll"), m_scrollTable,
        { QStringLiteral("方向"), QStringLiteral("帧ID"), QStringLiteral("类型"),
          QStringLiteral("长度"), QStringLiteral("数据") }));
    rv->addWidget(m_scrollTable, 1);

    auto *rb = new QHBoxLayout;
    m_clearRxBtn = new QPushButton(QStringLiteral("清空接收"));
    m_clearRxBtn->setObjectName("clearRxBtn");
    rb->addWidget(m_clearRxBtn);
    rb->addStretch();
    rv->addLayout(rb);

    root->addWidget(rxBox, 2);

    m_statusLbl = new QLabel(QStringLiteral("未连接"));
    statusBar()->addWidget(m_statusLbl);

    // ---------------- 顶部菜单栏 ----------------
    auto *protoMenu = menuBar()->addMenu(QStringLiteral("协议(&P)"));
    protoMenu->setObjectName("menuProtocol");
    auto *protoGroup = new QActionGroup(this);
    protoGroup->setExclusive(true);
    m_actionProtoCyberBeast = protoMenu->addAction(QStringLiteral("CyberBeast"));
    m_actionProtoCyberBeast->setObjectName("actionProtoCyberBeast");
    m_actionProtoCyberBeast->setCheckable(true);
    protoGroup->addAction(m_actionProtoCyberBeast);
    m_actionProtoSimple = protoMenu->addAction(QStringLiteral("Simple"));
    m_actionProtoSimple->setObjectName("actionProtoSimple");
    m_actionProtoSimple->setCheckable(true);
    protoGroup->addAction(m_actionProtoSimple);
    m_actionProtoOpen = protoMenu->addAction(QStringLiteral("Open"));
    m_actionProtoOpen->setObjectName("actionProtoOpen");
    m_actionProtoOpen->setCheckable(true);
    protoGroup->addAction(m_actionProtoOpen);
    m_actionProtoGeneric = protoMenu->addAction(QStringLiteral("通用CAN"));
    m_actionProtoGeneric->setObjectName("actionProtoGeneric");
    m_actionProtoGeneric->setCheckable(true);
    m_actionProtoGeneric->setChecked(true);          // 默认显示通用 CAN UI
    protoGroup->addAction(m_actionProtoGeneric);

    auto *modeMenu = menuBar()->addMenu(QStringLiteral("模式(&M)"));
    modeMenu->setObjectName("menuMode");
    auto *modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);
    m_actionModeCan = modeMenu->addAction(QStringLiteral("CAN"));
    m_actionModeCan->setObjectName("actionModeCan");
    m_actionModeCan->setCheckable(true);
    m_actionModeCan->setChecked(true);
    modeGroup->addAction(m_actionModeCan);
    m_actionModeRs485 = modeMenu->addAction(QStringLiteral("RS485"));
    m_actionModeRs485->setObjectName("actionModeRs485");
    m_actionModeRs485->setCheckable(true);
    modeGroup->addAction(m_actionModeRs485);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->setObjectName("menuHelp");
    auto *aboutAction = helpMenu->addAction(QStringLiteral("关于"));
    aboutAction->setObjectName("actionAbout");

    // 菜单快捷键：Ctrl+P / Ctrl+M / Ctrl+H 打开对应菜单（等效点击该菜单按钮）
    auto bindMenuShortcut = [this](QMenu *menu, const QKeySequence &ks, const QString &objName) {
        auto *a = new QAction(this);
        a->setObjectName(objName);
        a->setShortcut(ks);
        a->setShortcutContext(Qt::ApplicationShortcut);
        addAction(a);
        connect(a, &QAction::triggered, this, [this, menu]() {
            // 先收起上一个由快捷键展开的菜单（popup 不会自动关掉其它菜单栏菜单）
            if (m_openMenu && m_openMenu != menu)
                m_openMenu->close();
            // 以「菜单栏按钮」为锚点弹出（menuAction 作为 at 传入，Qt 按菜单栏语义定位到
            // 按钮下方、支持上下键导航）。
            QMenuBar *mb = menuBar();
            const QRect r = mb->actionGeometry(menu->menuAction());
            const QPoint p = mb->mapToGlobal(QPoint(r.left(), r.bottom() + 1));
            menu->popup(p, menu->menuAction());
            m_openMenu = menu;
        });
    };
    bindMenuShortcut(protoMenu, QKeySequence(QStringLiteral("Ctrl+P")), QStringLiteral("actMenuProtocol"));
    bindMenuShortcut(modeMenu,  QKeySequence(QStringLiteral("Ctrl+M")), QStringLiteral("actMenuMode"));
    bindMenuShortcut(helpMenu,  QKeySequence(QStringLiteral("Ctrl+H")), QStringLiteral("actMenuHelp"));

    // ---------------- 模式页面（QStackedWidget：0=CAN, 1=RS485）----------------
    m_pageCan = pageCan;
    m_pageRs485 = new QWidget;
    m_pageRs485->setObjectName("pageRs485");
    {
        // 布局与通用CAN一致：左=连接/数据区/日志，右=接收区
        auto *rsroot = new QHBoxLayout(m_pageRs485);

        auto *lcol = new QWidget;
        auto *lcl = new QVBoxLayout(lcol);
        lcl->addWidget(buildConnGroup());

        // ---- 数据区（发送）----
        auto *dataBox = new QGroupBox(QStringLiteral("数据区"));
        auto *dv = new QVBoxLayout(dataBox);
        auto *dh = new QHBoxLayout;
        dh->addWidget(new QLabel(QStringLiteral("数据(hex):")));
        m_rs485DataEdit = new HexLineEdit;
        m_rs485DataEdit->setObjectName("rs485DataEdit");
        m_rs485DataEdit->setMinimumHeight(tallerHeight(m_rs485DataEdit));
        m_rs485DataEdit->setClearButtonEnabled(true);
        dh->addWidget(m_rs485DataEdit, 1);
        dv->addLayout(dh);
        auto *dop = new QHBoxLayout;
        m_rs485SendBtn = new QPushButton(QStringLiteral("发送"));
        m_rs485SendBtn->setObjectName("rs485SendBtn");
        m_rs485StopBtn = new QPushButton(QStringLiteral("停止"));
        m_rs485StopBtn->setObjectName("rs485StopBtn");
        dop->addWidget(m_rs485SendBtn);
        dop->addWidget(m_rs485StopBtn);
        dop->addStretch();
        dv->addLayout(dop);
        auto *dHint = new QLabel(QStringLiteral("（RS485 协议/发送待实现；此页为布局占位，与通用CAN同构）"));
        dHint->setStyleSheet("color:#666;");
        dv->addWidget(dHint);
        lcl->addWidget(dataBox);

        lcl->addWidget(buildLogGroup(), 1);
        rsroot->addWidget(lcol, 3);

        // ---- 接收区（与通用CAN一致：就地/滚动 + 清空）----
        auto *rxBox = new QGroupBox(QStringLiteral("接收区"));
        auto *rv = new QVBoxLayout(rxBox);
        auto *rtop = new QHBoxLayout;
        m_rs485ModeLbl = new QLabel;
        m_rs485ModeLbl->setObjectName("rs485ModeLbl");
        m_rs485ModeBtn = new QToolButton;
        m_rs485ModeBtn->setObjectName("rs485ModeBtn");
        m_rs485ModeBtn->setText(QStringLiteral("⇄"));
        m_rs485ModeBtn->setAutoRaise(true);
        m_rs485TsBox = new QCheckBox(QStringLiteral("显示时间戳"));
        m_rs485TsBox->setObjectName("rs485TsBox");
        rtop->addWidget(m_rs485ModeLbl);
        rtop->addWidget(m_rs485ModeBtn);
        rtop->addSpacing(12);
        rtop->addWidget(m_rs485TsBox);
        rtop->addStretch();
        rv->addLayout(rtop);

        m_rs485RxTable = new QTableWidget(0, RX_COL_COUNT);
        m_rs485RxTable->setObjectName("rs485RxTable");
        m_rs485RxTable->setHorizontalHeaderLabels({
            QStringLiteral("帧ID(hex)"), QStringLiteral("类型"),
            QStringLiteral("长度"), QStringLiteral("数据(hex)"), QStringLiteral("次数")});
        m_rs485RxTable->horizontalHeader()->setSectionResizeMode(RX_DATA, QHeaderView::Stretch);
        for (int c = 0; c < RX_COL_COUNT; c++)
            if (c != RX_DATA) m_rs485RxTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        m_rs485RxTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_rs485RxTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_rs485RxTable->setShowGrid(false);
        m_rs485RxTable->setFrameShape(QFrame::NoFrame);
        m_rs485RxTable->setStyleSheet("QTableWidget{gridline-color:transparent;}");
        m_rs485RxTable->verticalHeader()->setVisible(false);
        m_rs485RxTable->setMinimumWidth(420);
        rv->addWidget(makeRxFilterRow(QStringLiteral("rs485.rx"), m_rs485RxTable,
            { QStringLiteral("帧ID"), QStringLiteral("类型"), QStringLiteral("长度"),
              QStringLiteral("数据"), QStringLiteral("次数") }));
        rv->addWidget(m_rs485RxTable, 1);

        m_rs485ScrollTable = new QTableWidget(0, SC_COL_COUNT);
        m_rs485ScrollTable->setObjectName("rs485Scroll");
        m_rs485ScrollTable->setHorizontalHeaderLabels({
            QStringLiteral("方向"), QStringLiteral("帧ID(hex)"), QStringLiteral("类型"),
            QStringLiteral("长度"), QStringLiteral("数据(hex)")});
        m_rs485ScrollTable->horizontalHeader()->setSectionResizeMode(SC_DATA, QHeaderView::Stretch);
        for (int c = 0; c < SC_COL_COUNT; c++)
            if (c != SC_DATA) m_rs485ScrollTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        m_rs485ScrollTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_rs485ScrollTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_rs485ScrollTable->setShowGrid(false);
        m_rs485ScrollTable->setFrameShape(QFrame::NoFrame);
        m_rs485ScrollTable->setStyleSheet("QTableWidget{gridline-color:transparent;}");
        m_rs485ScrollTable->verticalHeader()->setVisible(false);
        m_rs485ScrollTable->setMinimumWidth(420);
        m_rs485ScrollTable->setVisible(false);
        rv->addWidget(makeRxFilterRow(QStringLiteral("rs485.scroll"), m_rs485ScrollTable,
            { QStringLiteral("方向"), QStringLiteral("帧ID"), QStringLiteral("类型"),
              QStringLiteral("长度"), QStringLiteral("数据") }));
        rv->addWidget(m_rs485ScrollTable, 1);

        auto *rb = new QHBoxLayout;
        m_rs485ClearRxBtn = new QPushButton(QStringLiteral("清空接收"));
        m_rs485ClearRxBtn->setObjectName("rs485ClearRxBtn");
        rb->addWidget(m_rs485ClearRxBtn);
        rb->addStretch();
        rv->addLayout(rb);
        rsroot->addWidget(rxBox, 2);
    }
    // ---------------- 协议调试页栈（CAN 页内：CyberBeast / Simple / Open / 通用CAN）----------------
    m_pageProtoCyberBeast = new QWidget;
    m_pageProtoCyberBeast->setObjectName("pageProtoCyberBeast");
    {
        auto *v = new QVBoxLayout(m_pageProtoCyberBeast);
        v->addWidget(buildConnGroup());
        auto *l = new QLabel(QStringLiteral("CyberBeast 协议调试页（骨架，待实现）"));
        l->setAlignment(Qt::AlignCenter);
        v->addWidget(l, 1);
        v->addWidget(buildLogGroup(), 1);
    }
    m_pageProtoGeneric = canContent;             // 现有通用 CAN UI
    m_pageProtoSimple = new QWidget;
    m_pageProtoSimple->setObjectName("pageProtoSimple");
    {
        auto *sroot = new QHBoxLayout(m_pageProtoSimple);

        // ---------- 左：连接 + 命令区 + 日志 ----------
        auto *lcol = new QWidget;
        auto *lcl = new QVBoxLayout(lcol);
        lcl->addWidget(buildConnGroup());

        auto *cBox = new QGroupBox(QStringLiteral("命令 (CAN Simple)"));
        auto *cg = new QGridLayout(cBox);
        cg->addWidget(new QLabel(QStringLiteral("节点ID:")), 0, 0);
        m_simpleNodeEdit = new QLineEdit(QStringLiteral("1"));
        m_simpleNodeEdit->setObjectName("simpleNodeEdit");
        cg->addWidget(m_simpleNodeEdit, 0, 1);
        cg->addWidget(new QLabel(QStringLiteral("减速比:")), 0, 2);
        m_simpleGearEdit = new QLineEdit(QStringLiteral("1"));
        m_simpleGearEdit->setObjectName("simpleGearEdit");
        cg->addWidget(m_simpleGearEdit, 0, 3);
        cg->addWidget(new QLabel(QStringLiteral("命令:")), 1, 0);
        m_simpleCmdBox = new QComboBox;
        m_simpleCmdBox->setObjectName("simpleCmdBox");
        for (const simple::Cmd &c : simple::commands())
            m_simpleCmdBox->addItem(QStringLiteral("0x%1  %2")
                                        .arg(c.id, 2, 16, QLatin1Char('0')).arg(c.name), c.id);
        // 追加手册「CAN 协议实战」示例条目（UserRole=主命令ID，UserRole+1=示例索引）
        for (int i = 0; i < simple::examples().size(); ++i) {
            const simple::Example &e = simple::examples().at(i);
            m_simpleCmdBox->addItem(QStringLiteral("★ %1").arg(e.name), e.mainCmd);
            m_simpleCmdBox->setItemData(m_simpleCmdBox->count() - 1, i, Qt::UserRole + 1);
        }
        m_simpleCmdBox->setMinimumWidth(280);
        cg->addWidget(m_simpleCmdBox, 1, 1, 1, 3);
        lcl->addWidget(cBox);

        // 参数（随命令动态重建）
        auto *pBox = new QGroupBox(QStringLiteral("参数"));
        auto *pv = new QVBoxLayout(pBox);
        m_simpleParamArea = new QWidget;
        m_simpleParamArea->setObjectName("simpleParamArea");
        pv->addWidget(m_simpleParamArea);
        lcl->addWidget(pBox);

        // 预览
        auto *prBox = new QGroupBox(QStringLiteral("预览 (CAN ID / 数据 hex)"));
        auto *prv = new QVBoxLayout(prBox);
        m_simplePreview = new QLineEdit;
        m_simplePreview->setObjectName("simplePreview");
        m_simplePreview->setReadOnly(true);
        prv->addWidget(m_simplePreview);
        lcl->addWidget(prBox);

        auto *sop = new QHBoxLayout;
        m_simpleSendBtn = new QPushButton(QStringLiteral("发送"));
        m_simpleSendBtn->setObjectName("simpleSendBtn");
        m_simpleStopBtn = new QPushButton(QStringLiteral("停止"));
        m_simpleStopBtn->setObjectName("simpleStopBtn");
        sop->addWidget(m_simpleSendBtn);
        sop->addWidget(m_simpleStopBtn);
        sop->addStretch();
        lcl->addLayout(sop);
        lcl->addWidget(buildLogGroup(), 1);
        sroot->addWidget(lcol, 3);

        // ---------- 右：接收区（与通用 CAN UI 一致）----------
        auto *srxBox = new QGroupBox(QStringLiteral("接收区"));
        auto *srv = new QVBoxLayout(srxBox);
        auto *srtop = new QHBoxLayout;
        m_simpleModeLbl = new QLabel;
        m_simpleModeLbl->setObjectName("simpleModeLbl");
        m_simpleModeBtn = new QToolButton;
        m_simpleModeBtn->setObjectName("simpleModeBtn");
        m_simpleModeBtn->setText(QStringLiteral("⇄"));
        m_simpleModeBtn->setAutoRaise(true);
        m_simpleTsBox = new QCheckBox(QStringLiteral("显示时间戳"));
        m_simpleTsBox->setObjectName("simpleTsBox");
        srtop->addWidget(m_simpleModeLbl);
        srtop->addWidget(m_simpleModeBtn);
        srtop->addSpacing(12);
        srtop->addWidget(m_simpleTsBox);
        srtop->addStretch();
        srv->addLayout(srtop);

        m_simpleRxTable = new QTableWidget(0, RX_COL_COUNT);
        m_simpleRxTable->setObjectName("simpleRxTable");
        m_simpleRxTable->setHorizontalHeaderLabels({
            QStringLiteral("帧ID(hex)"), QStringLiteral("类型"),
            QStringLiteral("长度"), QStringLiteral("数据(hex)"), QStringLiteral("次数")});
        m_simpleRxTable->horizontalHeader()->setSectionResizeMode(RX_DATA, QHeaderView::Stretch);
        for (int c = 0; c < RX_COL_COUNT; c++)
            if (c != RX_DATA) m_simpleRxTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        m_simpleRxTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_simpleRxTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_simpleRxTable->setShowGrid(false);
        m_simpleRxTable->setFrameShape(QFrame::NoFrame);
        m_simpleRxTable->setStyleSheet("QTableWidget{gridline-color:transparent;}");
        m_simpleRxTable->verticalHeader()->setVisible(false);
        m_simpleRxTable->setMinimumWidth(420);
        srv->addWidget(makeRxFilterRow(QStringLiteral("simple.rx"), m_simpleRxTable,
            { QStringLiteral("帧ID"), QStringLiteral("类型"), QStringLiteral("长度"),
              QStringLiteral("数据"), QStringLiteral("次数") }));
        srv->addWidget(m_simpleRxTable, 1);

        m_simpleScrollTable = new QTableWidget(0, SC_COL_COUNT);
        m_simpleScrollTable->setObjectName("simpleScroll");
        m_simpleScrollTable->setHorizontalHeaderLabels({
            QStringLiteral("方向"), QStringLiteral("帧ID(hex)"), QStringLiteral("类型"),
            QStringLiteral("长度"), QStringLiteral("数据(hex)")});
        m_simpleScrollTable->horizontalHeader()->setSectionResizeMode(SC_DATA, QHeaderView::Stretch);
        for (int c = 0; c < SC_COL_COUNT; c++)
            if (c != SC_DATA) m_simpleScrollTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        m_simpleScrollTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_simpleScrollTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_simpleScrollTable->setShowGrid(false);
        m_simpleScrollTable->setFrameShape(QFrame::NoFrame);
        m_simpleScrollTable->setStyleSheet("QTableWidget{gridline-color:transparent;}");
        m_simpleScrollTable->verticalHeader()->setVisible(false);
        m_simpleScrollTable->setMinimumWidth(420);
        m_simpleScrollTable->setVisible(false);
        srv->addWidget(makeRxFilterRow(QStringLiteral("simple.scroll"), m_simpleScrollTable,
            { QStringLiteral("方向"), QStringLiteral("帧ID"), QStringLiteral("类型"),
              QStringLiteral("长度"), QStringLiteral("数据") }));
        srv->addWidget(m_simpleScrollTable, 1);

        auto *srb = new QHBoxLayout;
        m_simpleClearRxBtn = new QPushButton(QStringLiteral("清空接收"));
        m_simpleClearRxBtn->setObjectName("simpleClearRxBtn");
        srb->addWidget(m_simpleClearRxBtn);
        srb->addStretch();
        srv->addLayout(srb);
        sroot->addWidget(srxBox, 2);

        rebuildSimpleParams();
        simpleSetRxMode(false);
    }
    m_pageProtoOpen = new QWidget;
    m_pageProtoOpen->setObjectName("pageProtoOpen");
    {
        // 布局与其它 CAN 调试页一致：左=连接/功能/参数/预览/发送/日志，右=接收区
        auto *oroot = new QHBoxLayout(m_pageProtoOpen);

        auto *lcol = new QWidget;
        auto *lcl = new QVBoxLayout(lcol);
        lcl->addWidget(buildConnGroup());

        // ---- 功能（COB-ID）----
        auto *fBox = new QGroupBox(QStringLiteral("功能（CANopen）"));
        auto *fg = new QGridLayout(fBox);
        fg->addWidget(new QLabel(QStringLiteral("node_id:")), 0, 0);
        m_coNodeEdit = new QLineEdit(QStringLiteral("1"));
        m_coNodeEdit->setObjectName("coNodeEdit");
        fg->addWidget(m_coNodeEdit, 0, 1);
        fg->addWidget(new QLabel(QStringLiteral("减速比:")), 0, 2);
        m_coGearEdit = new QLineEdit(QStringLiteral("1"));
        m_coGearEdit->setObjectName("coGearEdit");
        fg->addWidget(m_coGearEdit, 0, 3);
        fg->addWidget(new QLabel(QStringLiteral("功能:")), 1, 0);
        m_coFuncBox = new QComboBox;
        m_coFuncBox->setObjectName("coFuncBox");
        {
            const QVector<QString> names = canopen::functionNames();
            const int fns[] = { (int)canopen::F_NMT, (int)canopen::F_SYNC,
                                (int)canopen::F_RPDO1, (int)canopen::F_RPDO2,
                                (int)canopen::F_RPDO3, (int)canopen::F_RPDO4,
                                (int)canopen::F_TPDO1, (int)canopen::F_TPDO2,
                                (int)canopen::F_TPDO3, (int)canopen::F_TPDO4,
                                (int)canopen::F_RSDO, (int)canopen::F_HEARTBEAT };
            for (int f : fns) m_coFuncBox->addItem(names.at(f), f);
            for (int i = 0; i < canopen::examples().size(); ++i) {
                m_coFuncBox->addItem(QStringLiteral("★ %1").arg(canopen::examples().at(i).name),
                                     (int)canopen::F_COUNT);
                m_coFuncBox->setItemData(m_coFuncBox->count() - 1, i, Qt::UserRole + 1);
            }
            m_coFuncBox->setMinimumWidth(280);
        }
        fg->addWidget(m_coFuncBox, 1, 1, 1, 3);
        lcl->addWidget(fBox);

        // ---- 参数（动态）----
        auto *pBox = new QGroupBox(QStringLiteral("参数"));
        auto *pv = new QVBoxLayout(pBox);
        m_coParamArea = new QWidget;
        m_coParamArea->setObjectName("coParamArea");
        pv->addWidget(m_coParamArea);
        // ---- 命令列表（可追加；上半=编辑器，下半=列表）----
        auto *listBox = new QGroupBox(QStringLiteral("命令列表（按顺序发送：↑/↓ 调序，可拖动）"));
        m_coListBox = listBox;
        auto *lv = new QVBoxLayout(listBox);
        m_coList = new QListWidget;
        m_coList->setObjectName("coList");
        m_coList->setDragDropMode(QAbstractItemView::InternalMove);
        m_coList->setDefaultDropAction(Qt::MoveAction);
        m_coList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_coList->setMinimumHeight(120);
        lv->addWidget(m_coList);
        auto *lbtns = new QHBoxLayout;
        m_coAddBtn = new QPushButton(QStringLiteral("＋ 追加到列表"));
        m_coAddBtn->setObjectName("coAddBtn");
        m_coListClearBtn = new QPushButton(QStringLiteral("清空列表"));
        m_coListClearBtn->setObjectName("coListClearBtn");
        lbtns->addWidget(m_coAddBtn);
        lbtns->addWidget(m_coListClearBtn);
        lbtns->addStretch();
        lv->addLayout(lbtns);
        pv->addWidget(listBox);
        lcl->addWidget(pBox);

        // ---- 预览 ----
        auto *prBox = new QGroupBox(QStringLiteral("预览（COB-ID / 数据）"));
        auto *prv = new QVBoxLayout(prBox);
        m_coPreview = new QLineEdit;
        m_coPreview->setObjectName("coPreview");
        m_coPreview->setReadOnly(true);
        prv->addWidget(m_coPreview);
        lcl->addWidget(prBox);

        auto *sop = new QHBoxLayout;
        m_coSendBtn = new QPushButton(QStringLiteral("发送"));
        m_coSendBtn->setObjectName("coSendBtn");
        m_coStopBtn = new QPushButton(QStringLiteral("停止"));
        m_coStopBtn->setObjectName("coStopBtn");
        m_coOdBrowserBtn = new QPushButton(QStringLiteral("对象字典…"));
        m_coOdBrowserBtn->setObjectName("coOdBrowserBtn");
        m_coImportEdsBtn = new QPushButton(QStringLiteral("导入EDS…"));
        m_coImportEdsBtn->setObjectName("coImportEdsBtn");
        m_coResetEdsBtn = new QPushButton(QStringLiteral("恢复内置"));
        m_coResetEdsBtn->setObjectName("coResetEdsBtn");
        sop->addWidget(m_coSendBtn);
        sop->addWidget(m_coStopBtn);
        sop->addWidget(m_coOdBrowserBtn);
        sop->addWidget(m_coImportEdsBtn);
        sop->addWidget(m_coResetEdsBtn);
        sop->addStretch();
        lcl->addLayout(sop);
        lcl->addWidget(buildLogGroup(), 1);
        oroot->addWidget(lcol, 3);

        // ---- 右：接收区（与通用CAN/Simple 一致）----
        auto *rxBox = new QGroupBox(QStringLiteral("接收区"));
        auto *rv = new QVBoxLayout(rxBox);
        auto *rtop = new QHBoxLayout;
        m_coModeLbl = new QLabel;
        m_coModeLbl->setObjectName("coModeLbl");
        m_coModeBtn = new QToolButton;
        m_coModeBtn->setObjectName("coModeBtn");
        m_coModeBtn->setText(QStringLiteral("⇄"));
        m_coModeBtn->setAutoRaise(true);
        m_coTsBox = new QCheckBox(QStringLiteral("显示时间戳"));
        m_coTsBox->setObjectName("coTsBox");
        rtop->addWidget(m_coModeLbl);
        rtop->addWidget(m_coModeBtn);
        rtop->addSpacing(12);
        rtop->addWidget(m_coTsBox);
        rtop->addStretch();
        rv->addLayout(rtop);

        m_coRxTable = new QTableWidget(0, RX_COL_COUNT);
        m_coRxTable->setObjectName("coRxTable");
        m_coRxTable->setHorizontalHeaderLabels({
            QStringLiteral("帧ID(hex)"), QStringLiteral("类型"),
            QStringLiteral("长度"), QStringLiteral("数据(hex)"), QStringLiteral("次数")});
        m_coRxTable->horizontalHeader()->setSectionResizeMode(RX_DATA, QHeaderView::Stretch);
        for (int c = 0; c < RX_COL_COUNT; c++)
            if (c != RX_DATA) m_coRxTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        m_coRxTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_coRxTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_coRxTable->setShowGrid(false);
        m_coRxTable->setFrameShape(QFrame::NoFrame);
        m_coRxTable->setStyleSheet("QTableWidget{gridline-color:transparent;}");
        m_coRxTable->verticalHeader()->setVisible(false);
        m_coRxTable->setMinimumWidth(420);
        rv->addWidget(makeRxFilterRow(QStringLiteral("co.rx"), m_coRxTable,
            { QStringLiteral("帧ID"), QStringLiteral("类型"), QStringLiteral("长度"),
              QStringLiteral("数据"), QStringLiteral("次数") }));
        rv->addWidget(m_coRxTable, 1);

        m_coScrollTable = new QTableWidget(0, SC_COL_COUNT);
        m_coScrollTable->setObjectName("coScroll");
        m_coScrollTable->setHorizontalHeaderLabels({
            QStringLiteral("方向"), QStringLiteral("帧ID(hex)"), QStringLiteral("类型"),
            QStringLiteral("长度"), QStringLiteral("数据(hex)")});
        m_coScrollTable->horizontalHeader()->setSectionResizeMode(SC_DATA, QHeaderView::Stretch);
        for (int c = 0; c < SC_COL_COUNT; c++)
            if (c != SC_DATA) m_coScrollTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        m_coScrollTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_coScrollTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_coScrollTable->setShowGrid(false);
        m_coScrollTable->setFrameShape(QFrame::NoFrame);
        m_coScrollTable->setStyleSheet("QTableWidget{gridline-color:transparent;}");
        m_coScrollTable->verticalHeader()->setVisible(false);
        m_coScrollTable->setMinimumWidth(420);
        m_coScrollTable->setVisible(false);
        rv->addWidget(makeRxFilterRow(QStringLiteral("co.scroll"), m_coScrollTable,
            { QStringLiteral("方向"), QStringLiteral("帧ID"), QStringLiteral("类型"),
              QStringLiteral("长度"), QStringLiteral("数据") }));
        rv->addWidget(m_coScrollTable, 1);

        auto *crb = new QHBoxLayout;
        m_coClearRxBtn = new QPushButton(QStringLiteral("清空接收"));
        m_coClearRxBtn->setObjectName("coClearRxBtn");
        crb->addWidget(m_coClearRxBtn);
        crb->addStretch();
        rv->addLayout(crb);
        oroot->addWidget(rxBox, 2);

        rebuildCoParams();
        coSetRxMode(false);
    }
    m_protoStack = new QStackedWidget;
    m_protoStack->setObjectName("protoStack");
    m_protoStack->addWidget(m_pageProtoCyberBeast);   // index 0
    m_protoStack->addWidget(m_pageProtoSimple);        // index 1
    m_protoStack->addWidget(m_pageProtoOpen);          // index 2
    m_protoStack->addWidget(m_pageProtoGeneric);       // index 3 = 通用CAN
    m_protoStack->setCurrentWidget(m_pageProtoGeneric);
    {
        auto *pcLay = new QVBoxLayout(pageCan);
        pcLay->setContentsMargins(0, 0, 0, 0);
        pcLay->addWidget(m_protoStack);
    }

    m_modeStack = new QStackedWidget;
    m_modeStack->setObjectName("modeStack");
    m_modeStack->addWidget(pageCan);        // index 0 = CAN
    m_modeStack->addWidget(m_pageRs485);    // index 1 = RS485
    setCentralWidget(m_modeStack);
    resize(1520, 880);

    // ---------------- 信号连接 ----------------
    // （连接区的 刷新/打开/更新固件/传输切换 由 buildConnGroup() 内部接线）
    connect(m_bitrateBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBitrateChanged);
    connect(m_dataBitrateBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDataBitrateChanged);
    connect(m_intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onIntervalChanged);
    connect(m_resetBtn, &QPushButton::clicked, this, &MainWindow::onResetList);
    connect(m_exportBtn, &QPushButton::clicked, this, &MainWindow::onExportCsv);
    connect(m_importBtn, &QPushButton::clicked, this, &MainWindow::onImportCsv);
    connect(m_clearRxBtn, &QPushButton::clicked, this, &MainWindow::onClearRx);
    connect(m_modeBtn, &QToolButton::clicked, this, [this]() {
        setRxMode(!m_scrollMode);                 // 点击图标切换到另一种模式
    });
    connect(m_sendBtn,  &QPushButton::clicked, this, &MainWindow::onSend);
    connect(m_stopBtn,  &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_statusBtn,&QPushButton::clicked, this, &MainWindow::onQueryStatus);

    // 菜单动作
    connect(m_actionProtoCyberBeast, &QAction::triggered, this, [this]() { setProtocol(0); });
    connect(m_actionProtoSimple,     &QAction::triggered, this, [this]() { setProtocol(1); });
    connect(m_actionProtoOpen,       &QAction::triggered, this, [this]() { setProtocol(2); });
    connect(m_actionProtoGeneric,    &QAction::triggered, this, [this]() { setProtocol(3); });
    connect(m_actionModeCan,   &QAction::triggered, this, [this]() { setWorkMode(0); });
    connect(m_actionModeRs485, &QAction::triggered, this, [this]() { setWorkMode(1); });
    connect(aboutAction,       &QAction::triggered, this, &MainWindow::onAbout);

    // Simple 协议页
    connect(m_simpleCmdBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSimpleCmdChanged);
    connect(m_simpleSendBtn,     &QPushButton::clicked, this, &MainWindow::onSimpleSend);
    connect(m_simpleStopBtn,     &QPushButton::clicked, this, &MainWindow::onSimpleStop);
    connect(m_simpleModeBtn,     &QToolButton::clicked, this, [this]() { simpleSetRxMode(!m_simpleScrollMode); });
    connect(m_simpleClearRxBtn,  &QPushButton::clicked, this, [this]() {
        if (m_simpleRxTable) m_simpleRxTable->setRowCount(0);
        if (m_simpleScrollTable) m_simpleScrollTable->setRowCount(0);
        m_simpleRxRowById.clear();
    });
    connect(m_simpleNodeEdit, &QLineEdit::textChanged, this, [this]() { updateSimplePreview(); });
    connect(m_simpleGearEdit, &QLineEdit::textChanged, this, [this]() { updateSimplePreview(); });

    // Open（CANopen）协议页
    connect(m_coFuncBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        // 选 PDO 功能（RPDO/TPDO）时：按手册/看 EDS 默认映射填行
        const int fn = m_coFuncBox->currentData(Qt::UserRole).toInt();
        if (curCoExample() < 0 &&
            canopen::pdoMappingObjectIndex((canopen::Function)fn) >= 0) {
            if (m_coPdoFunc != fn) {
                m_coPdoRows.clear();
                for (const canopen::PdoMapEntry &mp : canopen::pdoMapping((canopen::Function)fn)) {
                    CoPdoRow r; r.index = mp.index; r.sub = mp.sub;
                    const canopen::OdEntry *od = canopen::findOd(mp.index, mp.sub);
                    r.value = od ? od->dflt : QStringLiteral("0");
                    m_coPdoRows.append(r);
                }
                m_coPdoFunc = fn;
            }
        } else {
            m_coPdoFunc = -1;
        }
        rebuildCoParams();
    });
    connect(m_coNodeEdit, &QLineEdit::textChanged, this, [this]() { updateCoPreview(); });
    connect(m_coSendBtn, &QPushButton::clicked, this, &MainWindow::onCoSend);
    connect(m_coStopBtn, &QPushButton::clicked, this, &MainWindow::onCoStop);
    connect(m_coOdBrowserBtn, &QPushButton::clicked, this, &MainWindow::onOpenObjectDictionary);
    if (m_coImportEdsBtn) connect(m_coImportEdsBtn, &QPushButton::clicked, this, &MainWindow::onImportEds);
    if (m_coResetEdsBtn)  connect(m_coResetEdsBtn,  &QPushButton::clicked, this, &MainWindow::onResetEds);
    if (m_coAddBtn)       connect(m_coAddBtn,       &QPushButton::clicked, this, &MainWindow::coAddCurrentToList);
    if (m_coListClearBtn) connect(m_coListClearBtn, &QPushButton::clicked, this, [this]() {
        m_coListModel.clear(); coRebuildListWidget(); updateCoPreview(); });
    if (m_coList) connect(m_coList->model(), &QAbstractItemModel::rowsMoved,
        this, [this]() { coSnapshotCoList(); coRebuildListWidget(); updateCoPreview(); });
    if (m_coList) connect(m_coList, &QListWidget::itemClicked,
        this, [this](QListWidgetItem *it) { coRestoreFromEntry(m_coList->row(it)); });
    connect(m_coModeBtn, &QToolButton::clicked, this, [this]() { coSetRxMode(!m_coScrollMode); });
    connect(m_coClearRxBtn, &QPushButton::clicked, this, [this]() {
        if (m_coRxTable) m_coRxTable->setRowCount(0);
        if (m_coScrollTable) m_coScrollTable->setRowCount(0);
        m_coRxRowById.clear();
    });
    // RS485 页（布局与通用CAN一致；协议/发送待实现）
    connect(m_rs485SendBtn, &QPushButton::clicked, this, [this]() {
        logWarn(QStringLiteral("RS485 发送尚未实现（当前为布局占位）。"));
    });
    connect(m_rs485StopBtn, &QPushButton::clicked, this, [this]() {
        logWarn(QStringLiteral("RS485 停止尚未实现（当前为布局占位）。"));
    });
    connect(m_rs485ClearRxBtn, &QPushButton::clicked, this, [this]() {
        if (m_rs485RxTable) m_rs485RxTable->setRowCount(0);
        if (m_rs485ScrollTable) m_rs485ScrollTable->setRowCount(0);
    });
    connect(m_rs485ModeBtn, &QToolButton::clicked, this, [this]() {
        m_rs485ScrollMode = !m_rs485ScrollMode;
        if (m_rs485RxTable) m_rs485RxTable->setVisible(!m_rs485ScrollMode);
        if (m_rs485ScrollTable) m_rs485ScrollTable->setVisible(m_rs485ScrollMode);
        if (QWidget *r = filterRowFor(m_rs485RxTable)) r->setVisible(!m_rs485ScrollMode);
        if (QWidget *r = filterRowFor(m_rs485ScrollTable)) r->setVisible(m_rs485ScrollMode);
        if (m_rs485ModeLbl) m_rs485ModeLbl->setText(m_rs485ScrollMode
                                                     ? QStringLiteral("滚动") : QStringLiteral("就地"));
    });
    if (m_rs485ModeLbl) m_rs485ModeLbl->setText(QStringLiteral("就地"));

    // 空格键：发送发送列表中光标所在行（忽略勾选）
    if (m_table) m_table->installEventFilter(this);

    refreshPorts();
    setPortUiEnabled(false);
    logInfo(QStringLiteral("就绪。"));
    setRxMode(false);                             // 默认：就地更新
    loadSettings();                               // 恢复上次填写的内容与参数
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
//  多页面共享的「连接区 / 日志区」
//    每个页面各建一份控件；状态由 [0]（规范/通用CAN）统一广播到其余副本。
// ---------------------------------------------------------------------------
QGroupBox *MainWindow::buildConnGroup()
{
    auto *box = new QGroupBox(QStringLiteral("连接"));
    auto *h = new QHBoxLayout(box);
    h->addWidget(new QLabel(QStringLiteral("传输:")));
    auto *transport = new QComboBox;
    transport->addItem(QStringLiteral("串口(CDC)"), 0);
    transport->addItem(QStringLiteral("USB(libusb)"), 1);
    h->addWidget(transport);
    h->addWidget(new QLabel(QStringLiteral("端口:")));
    auto *port = new QComboBox;
    port->setMinimumWidth(200);
    h->addWidget(port);
    auto *refresh = new QPushButton(QStringLiteral("刷新"));
    h->addWidget(refresh);
    auto *open = new QPushButton(QStringLiteral("打开"));
    h->addWidget(open);
    auto *updateFw = new QPushButton(QStringLiteral("更新固件"));
    updateFw->setObjectName(QStringLiteral("updateFwBtn"));
    h->addWidget(updateFw);
    h->addStretch();

    const int idx = m_connPanels.size();
    if (idx == 0) {
        // 规范面板：对象名保持既有约定（transportBox/portBox），供 UI 自测 findChild 使用
        transport->setObjectName(QStringLiteral("transportBox"));
        port->setObjectName(QStringLiteral("portBox"));
        m_transportBox = transport; m_portBox = port;
        m_refreshBtn = refresh; m_openBtn = open; m_updateFwBtn = updateFw;
    } else {
        transport->setObjectName(QStringLiteral("transportBox%1").arg(idx));
        port->setObjectName(QStringLiteral("portBox%1").arg(idx));
    }
    m_connPanels.append(ConnPanel{transport, port, refresh, open, updateFw});

    connect(transport, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, idx](int) { onTransportChanged(idx); });
    connect(port, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, idx](int) { onPortChanged(idx); });
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(open,    &QPushButton::clicked, this, &MainWindow::togglePort);
    connect(updateFw, &QPushButton::clicked, this, &MainWindow::onUpdateFirmware);
    return box;
}

QGroupBox *MainWindow::buildLogGroup()
{
    auto *box = new QGroupBox(QStringLiteral("日志"));
    auto *v = new QVBoxLayout(box);
    auto *hb = new QHBoxLayout;
    hb->addStretch();
    auto *clear = new QPushButton(QStringLiteral("清空日志"));
    clear->setObjectName(QStringLiteral("clearLogBtn"));
    hb->addWidget(clear);
    v->addLayout(hb);
    auto *log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setMaximumBlockCount(2000);
    v->addWidget(log);

    if (m_logPanels.isEmpty()) {          // 规范份：沿用既有成员名（log）
        log->setObjectName(QStringLiteral("log"));
        m_log = log; m_clearLogBtn = clear;
    } else {
        log->setObjectName(QStringLiteral("log%1").arg(m_logPanels.size()));
    }
    m_logPanels.append(LogPanel{clear, log});
    connect(clear, &QPushButton::clicked, this, &MainWindow::onClearLog);
    return box;
}

// 把 [0]（规范）的传输/端口选择与按钮状态广播到所有副本
void MainWindow::syncConnUi()
{
    if (m_connPanels.isEmpty()) return;
    const ConnPanel &c = m_connPanels.first();
    for (const ConnPanel &p : m_connPanels) {
        { QSignalBlocker b(p.transport); p.transport->setCurrentIndex(c.transport->currentIndex()); }
        { QSignalBlocker b(p.port);      p.port->setCurrentIndex(c.port->currentIndex()); }
        p.transport->setEnabled(c.transport->isEnabled());
        p.port->setEnabled(c.port->isEnabled());
        p.refresh->setEnabled(c.refresh->isEnabled());
        p.open->setText(c.open->text());
    }
}

void MainWindow::onTransportChanged(int panelIdx)
{
    if (panelIdx < 0 || panelIdx >= m_connPanels.size()) return;
    if (panelIdx != 0) {   // 副本改动 -> 先写回规范，再广播
        QSignalBlocker b(m_connPanels[0].transport);
        m_connPanels[0].transport->setCurrentIndex(
            m_connPanels[panelIdx].transport->currentIndex());
    }
    refreshPorts();        // 重扫设备（填充所有副本） + syncConnUi
    const bool usb = m_connPanels.first().transport->currentData().toInt() == 1;
    logInfo(usb ? QStringLiteral("传输方式：USB(libusb)")
                : QStringLiteral("传输方式：串口(CDC)"));
}

void MainWindow::onPortChanged(int panelIdx)
{
    if (panelIdx < 0 || panelIdx >= m_connPanels.size()) return;
    if (panelIdx != 0) {
        QSignalBlocker b(m_connPanels[0].port);
        m_connPanels[0].port->setCurrentIndex(m_connPanels[panelIdx].port->currentIndex());
    }
    syncConnUi();
}

// ---------------------------------------------------------------------------
void MainWindow::refreshPorts()
{
    const bool usb = m_transportBox && m_transportBox->currentData().toInt() == 1;
    QStringList labels, datas;
    if (usb) {
        // USB(libusb)：列出全部可用 libusb 设备
        const auto devs = UsbLink::scanDevices();
        for (const UsbDeviceInfo &d : devs) { labels << d.label(); datas << d.id(); }
        if (devs.isEmpty())
            logInfo(QStringLiteral("未发现 libusb 设备（需装 WinUSB/libusb 驱动）。"));
        else
            logInfo(QStringLiteral("发现 %1 个 libusb 设备。").arg(devs.size()));
    } else {
        // 串口(CDC)
        const auto ports = SerialLink::availablePorts();
        for (const QSerialPortInfo &p : ports) {
            QString label = p.portName();
            if (!p.description().isEmpty())
                label += QStringLiteral("  (%1)").arg(p.description());
            labels << label; datas << p.portName();
        }
        if (ports.isEmpty())
            logInfo(QStringLiteral("未发现可用串口。"));
        else
            logInfo(QStringLiteral("发现 %1 个串口。").arg(ports.size()));
    }
    const int keep = m_portBox ? qMax(0, m_portBox->currentIndex()) : 0;
    for (const ConnPanel &p : m_connPanels) {     // 填充所有副本的端口列表
        QSignalBlocker b(p.port);
        p.port->clear();
        for (int i = 0; i < labels.size(); ++i) p.port->addItem(labels.at(i), datas.at(i));
        if (keep >= 0 && keep < p.port->count()) p.port->setCurrentIndex(keep);
    }
    syncConnUi();
}

void MainWindow::togglePort()
{
    if (m_link->isOpen()) {
        m_link->close();
        return;
    }

    if (m_portBox->currentIndex() < 0) {
        logInfo(m_transportBox->currentData().toInt() == 1
                    ? QStringLiteral("请先选择 USB 设备。")
                    : QStringLiteral("请先选择串口。"));
        return;
    }

    const QString sel = m_portBox->currentData().toString();
    if (m_transportBox->currentData().toInt() == 1) {
        m_link = m_usb;
        m_link->open(sel);           // sel = "bus:addr"
    } else {
        m_link = m_serial;
        m_link->open(sel);           // sel = 端口名
    }
}

void MainWindow::onPortOpened(const QString &name)
{
    setPortUiEnabled(true);
    m_openBtn->setText(QStringLiteral("关闭"));
    syncConnUi();                       // 所有页面的「打开」按钮都显示「关闭」
    m_statusLbl->setText(QStringLiteral("已连接(%1): %2").arg(m_link->kind(), name));

    // 连接后自动下发当前 CAN 波特率 + 发送模式（打包为一条消息）
    queueCommand(proto::CMD_SET_BITRATE,
                 (quint32)m_bitrateBox->currentData().toUInt(),
                 QStringLiteral("设置仲裁域波特率 %1").arg(m_bitrateBox->currentText()));
    queueCommand(proto::CMD_SET_DATA_BITRATE,
                 (quint32)m_dataBitrateBox->currentData().toUInt(),
                 QStringLiteral("设置数据域波特率 %1").arg(m_dataBitrateBox->currentText()));
    queueCommand(proto::CMD_SET_MODE, proto::MODE_SINGLE,
                 QStringLiteral("发送模式 单帧"));
    m_lastMode = (int)proto::MODE_SINGLE;
    // 注意：不在连接时自动查询固件版本（命令 10）。固件 v20.x 尚未实现命令 10，
    // 自动下发会触发 AA AD/ERR_USB_BADFRAME 报错；改为仅在「帮助→关于」点击时查询。
    flushTx(QStringLiteral("连接初始化"));
    logInfo(QStringLiteral("已连接(%1) %2").arg(m_link->kind(), name));
}

void MainWindow::onPortClosed()
{
    setPortUiEnabled(false);
    m_openBtn->setText(QStringLiteral("打开"));
    syncConnUi();                       // 所有页面的「关闭」按钮都恢复「打开」
    m_statusLbl->setText(QStringLiteral("未连接"));
    logInfo(QStringLiteral("已断开。"));
}

void MainWindow::onSerialError(const QString &msg)
{
    logError(msg);
}

void MainWindow::setPortUiEnabled(bool open)
{
    m_bitrateBox->setEnabled(open);
    m_dataBitrateBox->setEnabled(open);
    m_intervalSpin->setEnabled(open);
    m_sendBtn->setEnabled(open);
    m_stopBtn->setEnabled(open);
    m_statusBtn->setEnabled(open);
    m_resetBtn->setEnabled(true);   // 列表可离线编辑
    // 连接期间锁定传输/设备选择，避免误切
    m_transportBox->setEnabled(!open);
    m_portBox->setEnabled(!open);
    m_refreshBtn->setEnabled(!open);
    syncConnUi();                       // 广播到所有页面的连接区
}

// ---------------------------------------------------------------------------
void MainWindow::onBitrateChanged(int)
{
    if (!m_link->isOpen())
        return;
    sendCommand(proto::CMD_SET_BITRATE,
                (quint32)m_bitrateBox->currentData().toUInt(),
                QStringLiteral("设置仲裁域波特率 %1").arg(m_bitrateBox->currentText()));
}

void MainWindow::onDataBitrateChanged(int)
{
    if (!m_link->isOpen())
        return;
    sendCommand(proto::CMD_SET_DATA_BITRATE,
                (quint32)m_dataBitrateBox->currentData().toUInt(),
                QStringLiteral("设置数据域波特率 %1").arg(m_dataBitrateBox->currentText()));
}

void MainWindow::onIntervalChanged(int ms)
{
    if (!m_link->isOpen())
        return;
    sendCommand(proto::CMD_SET_INTERVAL, (quint32)ms,
                QStringLiteral("设置周期 %1 ms").arg(ms));
}

// ---------------------------------------------------------------------------
//  构建一帧：把 (id, idType, len, frameType, dataHex) 组成数据帧
// ---------------------------------------------------------------------------
static QByteArray assembleFrame(const QString &idText, int idType, int lenBytes,
                                int frameType, const QString &dataText, QString *err)
{
    bool ok = false;
    quint32 id = idText.trimmed().toUInt(&ok, 16);
    if (!ok || idText.trimmed().isEmpty()) {
        if (err) *err = QStringLiteral("ID 无效");
        return QByteArray();
    }
    QByteArray data = QByteArray::fromHex(
        QString(dataText).remove(QLatin1Char(' ')).remove(QLatin1Char(':')).toLatin1());

    if (data.size() > lenBytes)
        data.truncate(lenBytes);

    int dlc = proto::bytesToDlc(lenBytes);
    if (dlc < 0) { if (err) *err = QStringLiteral("长度非法"); return QByteArray(); }

    bool ext = (idType == 1);
    bool fd  = (frameType != FT_CLASSIC);
    bool brs = (frameType == FT_FD_BRS);

    return proto::buildDataFrame(id, ext, fd, brs, false, dlc, data);
}

QByteArray MainWindow::buildSingleFrame(QString *err) const
{
    return assembleFrame(m_idEdit->text(), m_idTypeBox->currentData().toInt(),
                         m_lenBox->currentData().toInt(), m_frameTypeBox->currentData().toInt(),
                         m_dataEdit->text(), err);
}

QByteArray MainWindow::buildRowFrame(int row, QString *err) const
{
    auto *idItem = m_table->item(row, COL_ID);
    if (!idItem || idItem->text().trimmed().isEmpty()) {
        if (err) *err = QStringLiteral("第 %1 行 ID 为空").arg(row + 1);
        return QByteArray();
    }
    auto comboData = [&](int col, int def) -> int {
        auto *w = qobject_cast<QComboBox *>(m_table->cellWidget(row, col));
        return w ? w->currentData().toInt() : def;
    };
    int idType = comboData(COL_IDTYPE, 0);
    int len    = comboData(COL_LEN, 8);
    int ftype  = comboData(COL_FRAMETYPE, FT_CLASSIC);
    auto *dataItem = m_table->item(row, COL_DATA);
    QString dataText = dataItem ? dataItem->text() : QString();

    return assembleFrame(idItem->text(), idType, len, ftype, dataText, err);
}

// ---------------------------------------------------------------------------
void MainWindow::initRow(int row)
{
    // 行号（只读，无表头文字）
    auto *no = new QTableWidgetItem(QString::number(row + 1));
    no->setFlags(Qt::ItemIsEnabled);
    no->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, COL_NO, no);

    // 选择列：复选框，默认不勾选（无表头文字）
    auto *chk = new QTableWidgetItem;
    chk->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    chk->setCheckState(Qt::Unchecked);
    m_table->setItem(row, COL_CHK, chk);

    // ID：空，可编辑
    m_table->setItem(row, COL_ID, new QTableWidgetItem(QString()));

    // 标准/扩展：默认标准帧
    m_table->setCellWidget(row, COL_IDTYPE, makeIdTypeBox());

    // 发送字节长度：默认 8 字节
    m_table->setCellWidget(row, COL_LEN, makeLenBox());

    // 帧类型：默认 CAN
    m_table->setCellWidget(row, COL_FRAMETYPE, makeFrameTypeBox());

    // 数据：空，可编辑
    m_table->setItem(row, COL_DATA, new QTableWidgetItem(QString()));
}

void MainWindow::ensureRows(int n)
{
    while (m_table->rowCount() < n) {
        int row = m_table->rowCount();
        m_table->insertRow(row);
        initRow(row);
    }
}

void MainWindow::resetTable()
{
    m_table->setRowCount(0);
    ensureRows(kDefaultRows);
}

void MainWindow::onResetList()
{
    resetTable();
    logInfo(QStringLiteral("列表已重置为 %1 行空表格。").arg(kDefaultRows));
}

// CSV 字段转义（含逗号/引号/换行时加引号）
static QString csvField(const QString &s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) ||
        s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'))) {
        QString t = s;
        t.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + t + QLatin1Char('"');
    }
    return s;
}

// 导出已勾选（且 ID 非空）的记录为 CSV 文本
QString MainWindow::buildExportCsv() const
{
    auto comboData = [&](int row, int col, int def) -> int {
        auto *w = qobject_cast<QComboBox *>(m_table->cellWidget(row, col));
        return w ? w->currentData().toInt() : def;
    };

    QString out;
    out += QStringLiteral("序号,帧ID,帧格式,帧类型,数据长度,数据\r\n");

    int seq = 0;
    for (int r = 0; r < m_table->rowCount(); r++) {
        auto *chk = m_table->item(r, COL_CHK);
        if (!chk || chk->checkState() != Qt::Checked)
            continue;                                   // 仅导出已勾选记录
        auto *idItem = m_table->item(r, COL_ID);
        if (!idItem || idItem->text().trimmed().isEmpty())
            continue;                                   // 跳过空行

        bool ok = false;
        quint32 id = idItem->text().trimmed().toUInt(&ok, 16);
        if (!ok)
            continue;

        int idType = comboData(r, COL_IDTYPE, 0);
        int len    = comboData(r, COL_LEN, 8);
        int ftype  = comboData(r, COL_FRAMETYPE, FT_CLASSIC);
        auto *dataItem = m_table->item(r, COL_DATA);
        QByteArray data = QByteArray::fromHex(
            QString(dataItem ? dataItem->text() : QString())
                .remove(QLatin1Char(' ')).remove(QLatin1Char(':')).toLatin1());
        if (data.size() > len)
            data.truncate(len);

        QString idStr   = QStringLiteral("0x") +
                          QString::number(id, 16).toUpper()
                              .rightJustified(idType == 1 ? 8 : 3, QLatin1Char('0'));
        QString idTypeS = idType == 1 ? QStringLiteral("扩展帧") : QStringLiteral("标准帧");
        QString ftypeS  = ftype == FT_FD_BRS ? QStringLiteral("CAN FD加速")
                        : ftype == FT_FD     ? QStringLiteral("CAN FD")
                                             : QStringLiteral("CAN");
        QString lenStr  = QString::number(len);
        QString dataStr = QString::fromLatin1(data.toHex(' ').toUpper());

        ++seq;
        out += QStringLiteral("%1,%2,%3,%4,%5,%6\r\n")
                   .arg(seq)
                   .arg(csvField(idStr), csvField(idTypeS), csvField(ftypeS),
                        csvField(lenStr), csvField(dataStr));
    }
    return out;
}

void MainWindow::onImportCsv()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入 CAN 记录"), QString(),
        QStringLiteral("CAN 记录 (*.csv *.asc *.trc *.log *.txt);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    const trace::Result res = trace::load(path);
    if (!res.ok) {
        logError(QStringLiteral("导入失败：%1").arg(res.error));
        return;
    }
    if (res.frames.isEmpty()) {
        logWarn(QStringLiteral("未从文件解析到 CAN 帧（识别为 %1）。").arg(res.format));
        return;
    }

    m_table->setRowCount(0);
    ensureRows(res.frames.size());
    for (int i = 0; i < res.frames.size(); ++i) {
        const trace::Frame &fr = res.frames[i];
        m_table->item(i, COL_ID)->setText(QString::number(fr.id, 16).toUpper());

        auto setCombo = [&](int col, int value) {
            if (auto *w = qobject_cast<QComboBox *>(m_table->cellWidget(i, col)))
                if (int idx = w->findData(value); idx >= 0)
                    w->setCurrentIndex(idx);
        };
        setCombo(COL_IDTYPE, fr.extended ? 1 : 0);

        // 发送字节长度：取与数据长度一致的可选项（无则取不超过它的最大值）
        if (auto *w = qobject_cast<QComboBox *>(m_table->cellWidget(i, COL_LEN))) {
            const int want = fr.data.size();
            int idx = w->findData(want);
            if (idx < 0) {
                int best = -1;
                for (int k = 0; k < w->count(); ++k) {
                    const int v = w->itemData(k).toInt();
                    if (v <= want && v > best) best = v;
                }
                if (best >= 0) idx = w->findData(best);
            }
            if (idx >= 0) w->setCurrentIndex(idx);
        }
        setCombo(COL_FRAMETYPE, fr.fd ? (fr.brs ? FT_FD_BRS : FT_FD) : FT_CLASSIC);

        m_table->item(i, COL_DATA)->setText(QString::fromLatin1(fr.data.toHex(' ').toUpper()));
        m_table->item(i, COL_CHK)->setCheckState(Qt::Checked);
    }

    QString msg = QStringLiteral("已导入 %1 帧（识别为 %2）到发送列表并勾选，可直接点“发送”。")
                      .arg(res.frames.size()).arg(res.format);
    if (res.skipped > 0)
        msg += QStringLiteral("（跳过 %1 行：注释/远程帧/错误帧等）").arg(res.skipped);
    logInfo(msg);
}

void MainWindow::onExportCsv()
{
    const QString csv = buildExportCsv();
    int rows = csv.count(QStringLiteral("\r\n")) - 1;   // 去掉表头
    if (rows <= 0) {
        logWarn(QStringLiteral("没有已勾选且已填写 ID 的记录可导出。"));
        return;
    }

    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 CSV"), QStringLiteral("can_frames.csv"),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive))
        path += QStringLiteral(".csv");

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logError(QStringLiteral("导出失败：无法写入 %1").arg(path));
        return;
    }
    // UTF-8 BOM，便于 Excel 正确识别中文
    f.write("\xEF\xBB\xBF");
    f.write(csv.toUtf8());
    f.close();
    logInfo(QStringLiteral("已导出 %1 条记录到 %2").arg(rows).arg(path));
}

void MainWindow::onClearRx()
{
    m_rxTable->setRowCount(0);          // 静止：就地更新表
    m_rxRowById.clear();
    if (m_scrollTable) m_scrollTable->setRowCount(0);   // 滚动：滚动表
}

void MainWindow::onClearLog()
{
    for (const LogPanel &p : m_logPanels)
        if (p.log) p.log->clear();
}

// ---------------------------------------------------------------------------
//  接收区：按帧 id 就地更新
// ---------------------------------------------------------------------------
static QString rxTypeText(const proto::RxFrame &f)
{
    QString s = f.ext ? QStringLiteral("扩展") : QStringLiteral("标准");
    if (f.fd)  s += f.brs ? QStringLiteral(" FD加速") : QStringLiteral(" FD");
    if (f.rtr) s += QStringLiteral(" RTR");
    return s;
}

void MainWindow::feedRx(const proto::RxFrame &f)
{
    int row = m_rxRowById.value(f.id, -1);
    if (row < 0) {
        // 新 id：追加一行
        row = m_rxTable->rowCount();
        m_rxTable->insertRow(row);
        for (int c = 0; c < RX_COL_COUNT; c++) {
            auto *it = new QTableWidgetItem;
            it->setFlags(Qt::ItemIsEnabled);
            m_rxTable->setItem(row, c, it);
        }
        m_rxRowById.insert(f.id, row);
    }

    int count = m_rxTable->item(row, RX_COUNT)->text().toInt() + 1;

    // 就地更新该行（不新增、不滚动）
    m_rxTable->item(row, RX_ID)->setText(
        QStringLiteral("0x%1").arg(f.id, f.ext ? 8 : 3, 16, QLatin1Char('0')).toUpper());
    m_rxTable->item(row, RX_TYPE)->setText(rxTypeText(f));
    m_rxTable->item(row, RX_LEN)->setText(QString::number(f.data.size()));
    m_rxTable->item(row, RX_DATA)->setText(
        QString::fromLatin1(f.data.toHex(' ').toUpper()));
    m_rxTable->item(row, RX_COUNT)->setText(QString::number(count));
    appendScroll(QStringLiteral("RX"), f);            // 收一条
    applyRxFilter(m_rxTable);                         // 筛选随新行刷新
}

int MainWindow::rxRowCount() const
{
    return m_rxTable->rowCount();
}

QString MainWindow::rxCellText(int row, int col) const
{
    auto *it = m_rxTable->item(row, col);
    return it ? it->text() : QString();
}

// ---------------------------------------------------------------------------
//  收集有效载荷
// ---------------------------------------------------------------------------
MainWindow::Payload MainWindow::collectPayload(QString *info) const
{
    Payload p;
    QVector<int> checked;
    for (int r = 0; r < m_table->rowCount(); r++) {
        auto *it = m_table->item(r, COL_CHK);
        if (it && it->checkState() == Qt::Checked)
            checked.append(r);
    }

    if (checked.size() >= 2) {
        p.isList = true;
        for (int r : checked) {
            QString err;
            QByteArray f = buildRowFrame(r, &err);
            if (!f.isEmpty()) p.frames.append(f);
        }
        if (info) *info = QStringLiteral("列表发送：勾选 %1 行，打包 %2 帧")
                              .arg(checked.size()).arg(p.frames.size());
    } else if (checked.size() == 1) {
        p.isList = false;
        QString err;
        QByteArray f = buildRowFrame(checked.first(), &err);
        if (!f.isEmpty()) p.frames.append(f);
        if (info) *info = QStringLiteral("单帧发送：来自第 %1 行").arg(checked.first() + 1);
    } else {
        p.isList = false;
        QString err;
        QByteArray f = buildSingleFrame(&err);
        if (!f.isEmpty()) p.frames.append(f);
        if (info) *info = QStringLiteral("单帧发送：来自编辑区");
    }
    return p;
}

// 仅发送表格第 row 行（单帧，忽略勾选）
void MainWindow::sendRowFrame(int row)
{
    if (!m_link || !m_link->isOpen()) {
        logWarn(QStringLiteral("串口未打开。"));
        return;
    }
    if (row < 0 || row >= m_table->rowCount())
        return;

    QString err;
    QByteArray f = buildRowFrame(row, &err);
    if (f.isEmpty()) {
        logWarn(QStringLiteral("第 %1 行无法发送：%2").arg(row + 1).arg(err));
        return;
    }

    // 单帧模式：确保发送模式为单帧，缓存该帧并触发单次发送
    queueCommand(proto::CMD_SET_MODE, proto::MODE_SINGLE, QStringLiteral("发送模式 单帧"));
    queueBytes(f, QStringLiteral("空格发送第 %1 行").arg(row + 1));
    queueCommand(proto::CMD_TX_START, 0u, QStringLiteral("触发发送 (单次)"));
    flushTx(QStringLiteral("空格发送单帧"));
    m_lastInit = false;   // 下次枚举发送需重新缓存，避免与批量发送状态串味

    // 滚动区回显该帧
    proto::RxFrame rx;
    if (proto::decodeDataFrame(f, &rx))
        appendScroll(QStringLiteral("TX"), rx);
    logInfo(QStringLiteral("已发送第 %1 行（单帧）").arg(row + 1));
}

// 事件过滤：当焦点在发送列表且按下空格时，发送当前行（忽略勾选）
bool MainWindow::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_table && ev->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        if (ke->key() == Qt::Key_Space && !ke->isAutoRepeat()) {
            const int row = m_table->currentRow();
            if (row >= 0)
                sendRowFrame(row);
            return true;   // 吞掉空格，避免触发列表内的其它默认行为
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

void MainWindow::onSend()
{
    if (!m_link->isOpen()) {
        logWarn(QStringLiteral("串口未打开。"));
        return;
    }
    doSend();
}

QString MainWindow::doSend()
{
    QString info;
    Payload p = collectPayload(&info);

    if (p.empty()) {
        logWarn(QStringLiteral("无可发送数据（编辑区为空且未勾选任何行）。"));
        return QStringLiteral("empty; changed=0 ptrig=0");
    }

    bool changed = !m_lastInit ||
                   (m_lastIsList != p.isList) ||
                   (m_lastFrames != p.frames);

    const quint32 trig = m_loopBox->isChecked() ? 1u : 0u;
    const int mode = p.isList ? (int)proto::MODE_FRAMELIST : (int)proto::MODE_SINGLE;
    const int interval = m_intervalSpin->value();

    if (!m_lastInit || m_lastMode != mode) {
        queueCommand(proto::CMD_SET_MODE, (quint32)mode,
                     QStringLiteral("发送模式 %1").arg(p.isList ? "列表" : "单帧"));
        m_lastMode = mode;
    }
    if (!m_lastInit || m_lastInterval != interval) {
        queueCommand(proto::CMD_SET_INTERVAL, (quint32)interval,
                     QStringLiteral("周期 %1 ms").arg(interval));
        m_lastInterval = interval;
    }

    // 无论数据是否变化，都在滚动区打印"要发送的帧"（仅显示，不代表真实下发）。
    for (const QByteArray &f : p.frames) {
        proto::RxFrame rx;
        if (proto::decodeDataFrame(f, &rx))
            appendScroll(QStringLiteral("TX"), rx);
    }

    if (changed) {
        // 仅在数据变化时，才把数据帧打包发给 MCU
        for (const QByteArray &f : p.frames) {
            queueBytes(f, p.isList ? QStringLiteral("列表帧") : QStringLiteral("单帧"));
        }
        m_lastInit = true;
        m_lastIsList = p.isList;
        m_lastFrames = p.frames;
    }
    // 数据未变化：不重发数据帧，实际只下发下面的 TX_START 命令（MCU 重发其缓存帧）

    queueCommand(proto::CMD_TX_START, trig,
                 QStringLiteral("触发发送 (%1)").arg(trig ? "连续" : "单次"));

    flushTx(QStringLiteral("打包发送"));

    return QStringLiteral("%1; changed=%2 ptrig=%3")
        .arg(info).arg(changed ? 1 : 0).arg(trig ? 1 : 0);
}

void MainWindow::onStop()
{
    if (!m_link->isOpen())
        return;
    sendCommand(proto::CMD_TX_STOP, 0, QStringLiteral("停止发送"));
}

void MainWindow::onQueryStatus()
{
    if (!m_link->isOpen())
        return;
    sendCommand(proto::CMD_GET_STATUS, 0, QStringLiteral("查询状态"));
}

// 工作模式切换（模式菜单）：切页面 + 连接时发命令 9
void MainWindow::setWorkMode(int mode)
{
    const bool rs485 = (mode == 1);
    if (m_modeStack)
        m_modeStack->setCurrentWidget(rs485 ? m_pageRs485 : m_pageCan);

    if (m_link && m_link->isOpen()) {
        sendCommand(proto::CMD_SET_WORK_MODE, rs485 ? 1u : 0u,
                    rs485 ? QStringLiteral("切换 RS485 模式") : QStringLiteral("切换 CAN 模式"));
    }
    logInfo(rs485 ? QStringLiteral("工作模式：RS485") : QStringLiteral("工作模式：CAN"));
}

// 协议菜单：切换 CAN 页内的协议调试页（0=CyberBeast 1=Simple 2=Open）
void MainWindow::setProtocol(int which)
{
    if (m_protoStack && which >= 0 && which < m_protoStack->count())
        m_protoStack->setCurrentIndex(which);
    static const char *const names[] = { "CyberBeast", "Simple", "Open", "通用CAN" };
    if (which >= 0 && which < 4)
        logInfo(QStringLiteral("协议：%1").arg(QString::fromLatin1(names[which])));
}

// ===========================================================================
//  Simple 协议页（CAN Simple）
// ===========================================================================

// 当前命令（无则 nullptr）
static const simple::Cmd *curSimpleCmd(QComboBox *box)
{
    if (!box) return nullptr;
    const quint8 id = (quint8)box->currentData().toUInt();
    for (const simple::Cmd &c : simple::commands())
        if (c.id == id) return &c;
    return nullptr;
}

// 按当前命令重建参数输入框
void MainWindow::rebuildSimpleParams()
{
    if (!m_simpleParamArea) return;
    if (auto *lay = m_simpleParamArea->layout()) {
        QLayoutItem *it;
        while ((it = lay->takeAt(0)) != nullptr) {
            if (it->widget()) delete it->widget();   // 立即删除：避免无事件循环时残留同名控件
            delete it;
        }
        delete lay;
    }
    m_simpleParamEdits.clear();
    m_simpleUnitBoxes.clear();
    m_sdoOpcodeBox = nullptr; m_sdoEndpointEdit = nullptr;
    m_sdoTypeBox = nullptr;   m_sdoValueEdit = nullptr;
    auto *g = new QGridLayout(m_simpleParamArea);
    const simple::Cmd *c = curSimpleCmd(m_simpleCmdBox);
    if (c && c->id == 0x04) {   // RxSdo：opcode + Endpoint + 写类型 + Value(仅写模式)
        int row = 0;
        m_sdoOpcodeBox = new QComboBox;
        m_sdoOpcodeBox->addItem(QString::fromUtf8("读 (0)"), 0);
        m_sdoOpcodeBox->addItem(QString::fromUtf8("写 (1)"), 1);
        m_sdoOpcodeBox->setObjectName(QStringLiteral("sdoOpcodeBox"));
        g->addWidget(new QLabel(QString::fromUtf8("opcode:")), row, 0);
        g->addWidget(m_sdoOpcodeBox, row++, 1);

        m_sdoEndpointEdit = new QLineEdit(QStringLiteral("0"));
        m_sdoEndpointEdit->setObjectName(QStringLiteral("sdoEndpointEdit"));
        g->addWidget(new QLabel(QStringLiteral("Endpoint_ID (可 0x..):")), row, 0);
        g->addWidget(m_sdoEndpointEdit, row++, 1);

        m_sdoTypeBox = new QComboBox;
        for (int i = 0; i < simple::sdoTypeNames().size(); ++i)
            m_sdoTypeBox->addItem(simple::sdoTypeNames().at(i), i);
        m_sdoTypeBox->setObjectName(QStringLiteral("sdoTypeBox"));
        g->addWidget(new QLabel(QString::fromUtf8("写类型:")), row, 0);
        g->addWidget(m_sdoTypeBox, row++, 1);

        m_sdoValueEdit = new QLineEdit(QStringLiteral("0"));
        m_sdoValueEdit->setObjectName(QStringLiteral("sdoValueEdit"));
        g->addWidget(new QLabel(QStringLiteral("Value:")), row, 0);
        g->addWidget(m_sdoValueEdit, row++, 1);

        auto setWriteEnabled = [this]() {
            const bool w = m_sdoOpcodeBox && m_sdoOpcodeBox->currentData().toInt() == 1;
            if (m_sdoTypeBox)  m_sdoTypeBox->setEnabled(w);
            if (m_sdoValueEdit) m_sdoValueEdit->setEnabled(w);
        };
        connect(m_sdoOpcodeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, setWriteEnabled](int) { setWriteEnabled(); updateSimplePreview(); });
        connect(m_sdoEndpointEdit, &QLineEdit::textChanged, this, [this]() { updateSimplePreview(); });
        connect(m_sdoTypeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { updateSimplePreview(); });
        connect(m_sdoValueEdit, &QLineEdit::textChanged, this, [this]() { updateSimplePreview(); });
        setWriteEnabled();   // 默认「读」→ 类型/Value 置灰
        if (!c->desc.isEmpty())
            g->addWidget(new QLabel(QStringLiteral("说明：%1").arg(c->desc)), row, 0, 1, 2);
        updateSimplePreview();
        return;
    }
    if (c) {
        int row = 0;
        for (int i = 0; i < c->params.size(); ++i) {
            const simple::Param &p = c->params.at(i);
            if (p.type == simple::P_RESV) {   // 保留字节：不显示，占位保持下标对齐
                m_simpleParamEdits.append(nullptr);
                m_simpleUnitBoxes.append(nullptr);
                continue;
            }
            const QString u = p.unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(p.unit);
            g->addWidget(new QLabel(QStringLiteral("%1%2:").arg(p.name, u)), row, 0);
            auto *e = new QLineEdit(QStringLiteral("0"));
            e->setObjectName(QStringLiteral("simpleParam%1").arg(i));
            g->addWidget(e, row, 1);
            m_simpleParamEdits.append(e);
            connect(e, &QLineEdit::textChanged, this, [this]() { updateSimplePreview(); });

            // 位置/速度类参数：额外一个单位下拉（默认取 Param.defaultUnit）
            QComboBox *ub = nullptr;
            if (p.kind != simple::U_NONE) {
                ub = new QComboBox;
                const bool isPos = (p.kind == simple::U_POS || p.kind == simple::U_MIT_POS);
                const QVector<QString> &names = isPos ? simple::positionUnitNames()
                                                      : simple::velocityUnitNames();
                for (const QString &nm : names) ub->addItem(nm);
                ub->setCurrentIndex(p.defaultUnit >= 0 && p.defaultUnit < names.size()
                                        ? p.defaultUnit : 0);
                ub->setObjectName(QStringLiteral("simpleUnit%1").arg(i));
                g->addWidget(ub, row, 2);
                connect(ub, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this](int) { updateSimplePreview(); });
            }
            m_simpleUnitBoxes.append(ub);
            ++row;
        }
        if (!c->desc.isEmpty())
            g->addWidget(new QLabel(QStringLiteral("说明：%1").arg(c->desc)), row, 0, 1, 2);
    }
    updateSimplePreview();
}

void MainWindow::onSimpleCmdChanged(int) { rebuildSimpleParams(); }

// 刷新「预览」：普通命令显示 ID/DATA 并附换算后的电机侧值；实战示例显示整段帧序列
void MainWindow::updateSimplePreview()
{
    if (!m_simplePreview) return;
    QString err;
    const QVector<QByteArray> frames = buildSimpleFrames(&err);
    if (frames.isEmpty()) { m_simplePreview->setText(err); return; }

    auto frameLabel = [](const QByteArray &f) {
        proto::RxFrame rx;
        if (!proto::decodeDataFrame(f, &rx)) return QString();
        return QStringLiteral("0x%1:%2")
            .arg(QString::number(rx.id, 16).toUpper().rightJustified(3, QLatin1Char('0')),
                 QString::fromLatin1(rx.data.toHex(' ').toUpper()));
    };

    const int ex = curSimpleExample();
    if (ex >= 0) {                       // 实战示例：整段序列
        QStringList parts;
        for (const QByteArray &f : frames) parts << frameLabel(f);
        m_simplePreview->setText(QStringLiteral("示例「%1」 %2")
            .arg(simple::examples().at(ex).name, parts.join(QStringLiteral("  →  "))));
        return;
    }

    // 普通命令
    proto::RxFrame rx;
    proto::decodeDataFrame(frames.first(), &rx);
    QString s = QStringLiteral("ID=0x%1  DATA=%2")
        .arg(QString::number(rx.id, 16).toUpper().rightJustified(3, QLatin1Char('0')),
             QString::fromLatin1(rx.data.toHex(' ').toUpper()));
    const simple::Cmd *c = curSimpleCmd(m_simpleCmdBox);
    const double gear = m_simpleGearEdit ? m_simpleGearEdit->text().toDouble() : 1.0;
    if (c) {                             // 附：换算后的电机侧值（转/转每秒）
        QStringList conv;
        for (int i = 0; i < c->params.size(); ++i) {
            const simple::Param &p = c->params.at(i);
            if (p.kind == simple::U_NONE) continue;
            QLineEdit *e = (i < m_simpleParamEdits.size()) ? m_simpleParamEdits.at(i) : nullptr;
            if (!e) continue;
            QComboBox *ub = (i < m_simpleUnitBoxes.size()) ? m_simpleUnitBoxes.at(i) : nullptr;
            const int u = ub ? ub->currentIndex() : p.defaultUnit;
            const double motor = simple::toWire(p, e->text().toDouble(), u, gear);
            conv << QStringLiteral("%1=%2").arg(p.name).arg(motor, 0, 'g', 6);
        }
        if (!conv.isEmpty()) s += QStringLiteral("  电机: ") + conv.join(QStringLiteral(", "));
    }
    m_simplePreview->setText(s);
}

// 生成 Simple 线上帧：Simple 帧 = 11-bit 标准数据帧，8 字节
QByteArray MainWindow::buildSimpleFrame(QString *err) const
{
    const simple::Cmd *c = curSimpleCmd(m_simpleCmdBox);
    if (!c) { if (err) *err = QStringLiteral("未选择命令"); return QByteArray(); }

    const int node = m_simpleNodeEdit ? m_simpleNodeEdit->text().toInt() : 1;
    const double gear = m_simpleGearEdit ? m_simpleGearEdit->text().toDouble() : 1.0;
    if (node < 0 || node > 31) { if (err) *err = QStringLiteral("节点ID 需在 0..31"); return QByteArray(); }

    if (c->id == 0x04) {   // RxSdo：opcode + Endpoint + 写类型 + Value（读模式 Value 忽略）
        const int opcode   = m_sdoOpcodeBox ? m_sdoOpcodeBox->currentData().toInt() : 0;
        const quint16 ep   = m_sdoEndpointEdit
                                 ? (quint16)m_sdoEndpointEdit->text().toUInt(nullptr, 0) : 0;
        const int sdoType  = m_sdoTypeBox ? m_sdoTypeBox->currentData().toInt() : simple::SDO_FLOAT;
        const double value = m_sdoValueEdit ? m_sdoValueEdit->text().toDouble() : 0.0;
        simple::Wire w = simple::buildRxSdo(node, opcode, ep, value, sdoType);
        return proto::buildDataFrame(w.id, false, false, false, false, proto::bytesToDlc(8), w.data);
    }

    QVector<double> vals;
    QVector<int>    units;
    for (int i = 0; i < m_simpleParamEdits.size(); ++i) {
        QLineEdit *e = m_simpleParamEdits.at(i);
        vals.append(e ? e->text().toDouble() : 0.0);
        QComboBox *ub = (i < m_simpleUnitBoxes.size()) ? m_simpleUnitBoxes.at(i) : nullptr;
        units.append(ub ? ub->currentIndex() : c->params.at(i).defaultUnit);
    }

    simple::Wire w = simple::buildWire(node, *c, vals, gear, units);
    return proto::buildDataFrame(w.id, /*ext*/false, /*fd*/false, /*brs*/false,
                                 /*rtr*/false, proto::bytesToDlc(8), w.data);
}

// 当前选中的实战示例索引（-1=普通命令）
int MainWindow::curSimpleExample() const
{
    if (!m_simpleCmdBox) return -1;
    const QVariant v = m_simpleCmdBox->currentData(Qt::UserRole + 1);
    return v.isValid() ? v.toInt() : -1;
}

// 生成待发送的帧序列：普通命令=1 帧；实战示例=固定前缀帧 + 主命令帧
QVector<QByteArray> MainWindow::buildSimpleFrames(QString *err) const
{
    QVector<QByteArray> out;
    const int ex = curSimpleExample();
    if (ex >= 0 && ex < simple::examples().size()) {
        const simple::Example &e = simple::examples().at(ex);
        const int node = m_simpleNodeEdit ? m_simpleNodeEdit->text().toInt() : 1;
        if (node < 0 || node > 31) { if (err) *err = QStringLiteral("节点ID 需在 0..31"); return out; }
        for (int i = 0; i < e.cmdIds.size(); ++i) {
            out.append(proto::buildDataFrame(simple::canId(node, e.cmdIds.at(i)), false, false,
                                             false, false, proto::bytesToDlc(8), e.payloads.at(i)));
        }
        if (e.mainCmd != 0) {          // 主命令：参数由用户输入（含单位换算）
            QString e2;
            QByteArray f = buildSimpleFrame(&e2);
            if (f.isEmpty()) { if (err) *err = e2; return out; }
            out.append(f);
        }
        return out;
    }
    QString e2;
    QByteArray f = buildSimpleFrame(&e2);
    if (f.isEmpty()) { if (err) *err = e2; return out; }
    out.append(f);
    return out;
}

void MainWindow::onSimpleSend()
{
    if (!m_link || !m_link->isOpen()) { logWarn(QStringLiteral("串口未打开。")); return; }
    QString err;
    const QVector<QByteArray> frames = buildSimpleFrames(&err);
    if (frames.isEmpty()) { logWarn(QStringLiteral("无法发送：%1").arg(err)); return; }

    const int ex = curSimpleExample();
    if (ex >= 0) {                       // 实战示例：多帧序列（列表模式）
        queueCommand(proto::CMD_SET_MODE, proto::MODE_FRAMELIST, QStringLiteral("发送模式 列表"));
        for (const QByteArray &f : frames) queueBytes(f, QStringLiteral("示例帧"));
        queueCommand(proto::CMD_TX_START, 0u, QStringLiteral("触发发送 (单次)"));
        flushTx(QStringLiteral("实战示例发送"));
    } else {                             // 普通命令：单帧（数据变才打包）
        const QByteArray &f = frames.first();
        const bool changed = (!m_simpleLastInit || m_simpleLastFrame != f);
        queueCommand(proto::CMD_SET_MODE, proto::MODE_SINGLE, QStringLiteral("发送模式 单帧"));
        if (changed) {
            queueBytes(f, QStringLiteral("Simple 数据帧"));
            m_simpleLastFrame = f; m_simpleLastInit = true;
        }
        queueCommand(proto::CMD_TX_START, 0u, QStringLiteral("触发发送 (单次)"));
        flushTx(QStringLiteral("Simple 发送"));
    }

    for (const QByteArray &f : frames) {
        proto::RxFrame rx;
        if (proto::decodeDataFrame(f, &rx)) simpleAppendScroll(QStringLiteral("TX"), rx);
    }
    logInfo(QStringLiteral("已发送 %1（%2 帧）").arg(
        m_simpleCmdBox->currentText()).arg(frames.size()));
}

void MainWindow::onSimpleStop()
{
    if (!m_link || !m_link->isOpen()) return;
    sendCommand(proto::CMD_TX_STOP, 0, QStringLiteral("停止发送"));
}

// 接收：就地汇总（按帧 id）+ 滚动
void MainWindow::simpleFeedRx(const proto::RxFrame &f)
{
    if (!m_simpleRxTable) return;
    int row = m_simpleRxRowById.value(f.id, -1);
    if (row < 0) {
        row = m_simpleRxTable->rowCount();
        m_simpleRxTable->insertRow(row);
        for (int c = 0; c < RX_COL_COUNT; c++) {
            auto *it = new QTableWidgetItem;
            it->setFlags(Qt::ItemIsEnabled);
            m_simpleRxTable->setItem(row, c, it);
        }
        m_simpleRxRowById.insert(f.id, row);
    }
    const int count = m_simpleRxTable->item(row, RX_COUNT)->text().toInt() + 1;
    m_simpleRxTable->item(row, RX_ID)->setText(
        QStringLiteral("0x%1").arg(f.id, f.ext ? 8 : 3, 16, QLatin1Char('0')).toUpper());
    m_simpleRxTable->item(row, RX_TYPE)->setText(rxTypeText(f));
    m_simpleRxTable->item(row, RX_LEN)->setText(QString::number(f.data.size()));
    m_simpleRxTable->item(row, RX_DATA)->setText(QString::fromLatin1(f.data.toHex(' ').toUpper()));
    m_simpleRxTable->item(row, RX_COUNT)->setText(QString::number(count));
    simpleAppendScroll(QStringLiteral("RX"), f);
    applyRxFilter(m_simpleRxTable);
}

void MainWindow::simpleAppendScroll(const QString &dir, const proto::RxFrame &f)
{
    if (!m_simpleScrollTable || !m_simpleScrollMode) return;
    QScrollBar *vsb = m_simpleScrollTable->verticalScrollBar();
    const bool follow = (vsb->value() >= vsb->maximum());
    const int row = m_simpleScrollTable->rowCount();
    m_simpleScrollTable->insertRow(row);
    QString dirTxt = dir;
    if (m_simpleTsBox && m_simpleTsBox->isChecked())
        dirTxt = QStringLiteral("%1 %2").arg(QTime::currentTime().toString("HH:mm:ss.zzz"), dir);
    auto set = [&](int c, const QString &t) {
        auto *it = new QTableWidgetItem(t);
        it->setFlags(Qt::ItemIsEnabled);
        m_simpleScrollTable->setItem(row, c, it);
    };
    set(SC_DIR, dirTxt);
    set(SC_ID, QStringLiteral("0x%1").arg(f.id, f.ext ? 8 : 3, 16, QLatin1Char('0')).toUpper());
    set(SC_TYPE, rxTypeText(f));
    set(SC_LEN, QString::number(f.data.size()));
    set(SC_DATA, QString::fromLatin1(f.data.toHex(' ').toUpper()));
    if (m_simpleScrollTable->rowCount() > 5000) m_simpleScrollTable->removeRow(0);
    applyRxFilter(m_simpleScrollTable);
    if (follow) m_simpleScrollTable->scrollToBottom();
}

void MainWindow::simpleSetRxMode(bool scroll)
{
    m_simpleScrollMode = scroll;
    if (m_simpleRxTable) m_simpleRxTable->setVisible(!scroll);
    if (m_simpleScrollTable) m_simpleScrollTable->setVisible(scroll);
    if (QWidget *r = filterRowFor(m_simpleRxTable)) r->setVisible(!scroll);
    if (QWidget *r = filterRowFor(m_simpleScrollTable)) r->setVisible(scroll);
    if (m_simpleModeLbl) m_simpleModeLbl->setText(scroll ? QStringLiteral("滚动") : QStringLiteral("就地"));
}


// 帮助：关于（软件版本 + 设备固件版本）。点「关于」时自动发起固件版本查询（命令 10），
// 稍等回包后再弹窗，以显示刚查到的设备版本。

// ===========================================================================
//  Open（CANopen）协议页（CiA301/302/402）
// ===========================================================================

int MainWindow::curCoExample() const
{
    if (!m_coFuncBox) return -1;
    const QVariant v = m_coFuncBox->currentData(Qt::UserRole + 1);
    return v.isValid() ? v.toInt() : -1;
}

// 按当前功能重建参数输入
void MainWindow::rebuildCoParams()
{
    if (!m_coParamArea) return;
    if (auto *lay = m_coParamArea->layout()) {
        QLayoutItem *it;
        while ((it = lay->takeAt(0)) != nullptr) {
            if (it->widget()) delete it->widget();   // 立即删除：避免无事件循环时残留同名控件
            delete it;
        }
        delete lay;
    }
    m_coNmtCmdBox = nullptr; m_coSdoOpBox = nullptr;
    m_coSdoIdxBox = nullptr;    m_coSdoValueEdit = nullptr;
    m_coHbStateBox = nullptr;
    m_coPdoIdxBoxes.clear(); m_coPdoValEdits.clear(); m_coPdoAddBtn = nullptr;

    m_coExValEdits.clear();
    m_coExUnitBoxes.clear();
    m_coMapPdoBox = nullptr; m_coMapObjBoxes.clear(); m_coMapAddBtn = nullptr;
    if (m_coListBox) m_coListBox->setVisible(curCoExample() < 0);   // 实战示例不显示命令列表
    auto *g = new QGridLayout(m_coParamArea);
    const int ex = curCoExample();
    if (ex >= 0 && ex < canopen::examples().size()) {
        const canopen::Example &e = canopen::examples().at(ex);
        int row = 0;
        for (int k = 0; k < e.inputs.size(); ++k) {
            const canopen::ExampleInput &in = e.inputs.at(k);
            g->addWidget(new QLabel(in.name + QLatin1Char(':')), row, 0);
            auto *ve = new QLineEdit(QString::number(in.defValue));
            ve->setObjectName(QStringLiteral("coExVal%1").arg(k));
            g->addWidget(ve, row, 1);
            m_coExValEdits.append(ve);
            connect(ve, &QLineEdit::textChanged, this, [this]() { updateCoPreview(); });

            QComboBox *ub = nullptr;
            const QVector<QString> units = canopen::coUnitNames(in.kind);
            if (!units.isEmpty()) {
                ub = new QComboBox;
                for (const QString &u : units) ub->addItem(u);
                ub->setCurrentIndex(in.defUnit >= 0 && in.defUnit < units.size() ? in.defUnit : 0);
                ub->setObjectName(QStringLiteral("coExUnit%1").arg(k));
                g->addWidget(ub, row, 2);
                connect(ub, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this](int) { updateCoPreview(); });
            }
            m_coExUnitBoxes.append(ub);
            ++row;
        }
        // 「PDO 映射修改」示例：可配置（选择要修改的 PDO + 目标映射对象）
        if (e.name.contains(QStringLiteral("映射修改"))) {
            g->addWidget(new QLabel(QStringLiteral("要修改的 PDO:")), row, 0);
            m_coMapPdoBox = new QComboBox;
            m_coMapPdoBox->setObjectName("coMapPdoBox");
            const int pdos[] = { (int)canopen::F_RPDO1, (int)canopen::F_RPDO2,
                                 (int)canopen::F_RPDO3, (int)canopen::F_RPDO4,
                                 (int)canopen::F_TPDO1, (int)canopen::F_TPDO2,
                                 (int)canopen::F_TPDO3, (int)canopen::F_TPDO4 };
            const QVector<QString> names = canopen::functionNames();
            for (int f : pdos) m_coMapPdoBox->addItem(names.at(f), f);
            const int curPdo = (m_coMapCurPdo >= 0) ? m_coMapCurPdo : (int)canopen::F_TPDO1;
            m_coMapPdoBox->setCurrentIndex(m_coMapPdoBox->findData(curPdo));
            m_coMapCurPdo = curPdo;
            g->addWidget(m_coMapPdoBox, row, 1, 1, 3);
            connect(m_coMapPdoBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int) {
                if (m_coMapCurPdo >= 0) m_coMapCache[m_coMapCurPdo] = m_coMapRows;
                const int np = m_coMapPdoBox->currentData().toInt();
                if (m_coMapCache.contains(np)) {
                    m_coMapRows = m_coMapCache.value(np);
                } else {
                    m_coMapRows.clear();
                    for (const canopen::PdoMapEntry &mp : canopen::pdoMapping((canopen::Function)np)) {
                        CoPdoRow r; r.index = mp.index; r.sub = mp.sub;
                        m_coMapRows.append(r);
                    }
                }
                m_coMapCurPdo = np;
                // 延后重建：避免在自身信号回调里删除发送者
                QTimer::singleShot(0, this, [this]() { rebuildCoParams(); });
            });
            ++row;

            g->addWidget(new QLabel(QStringLiteral("目标映射对象:")), row++, 0, 1, 4);
            if (m_coMapRows.isEmpty()) {   // 默认：该 PDO 的当前（EDS）映射
                for (const canopen::PdoMapEntry &mp : canopen::pdoMapping((canopen::Function)m_coMapCurPdo)) {
                    CoPdoRow r; r.index = mp.index; r.sub = mp.sub;
                    m_coMapRows.append(r);
                }
            }
            const QVector<canopen::OdEntry> od = canopen::objectDictionary();
            for (int i = 0; i < m_coMapRows.size(); ++i) {
                const CoPdoRow &r = m_coMapRows.at(i);
                auto *ib = new QComboBox;
                ib->setObjectName(QStringLiteral("coMapObj%1").arg(i));
                ib->setMinimumWidth(240);
                int sel = -1;
                for (int k = 0; k < od.size(); ++k) {
                    const canopen::OdEntry &oe = od.at(k);
                    ib->addItem(canopen::odDisplay(oe),
                                QVariant((uint)((oe.index << 8) | (oe.sub < 0 ? 0 : (oe.sub & 0xFF)))));
                    if (oe.index == r.index && ((oe.sub < 0 ? 0 : oe.sub) == r.sub)) sel = k;
                }
                if (sel >= 0) ib->setCurrentIndex(sel);
                g->addWidget(ib, row, 0, 1, 3);
                m_coMapObjBoxes.append(ib);
                connect(ib, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this](int) { updateCoPreview(); });
                auto *rm = new QPushButton(QStringLiteral("×"));
                rm->setObjectName(QStringLiteral("coMapRm%1").arg(i));
                rm->setFixedWidth(28);
                g->addWidget(rm, row, 3);
                connect(rm, &QPushButton::clicked, this, [this, i]() { coMapRemoveRow(i); });
                ++row;
            }
            m_coMapAddBtn = new QPushButton(QStringLiteral("＋ 添加映射对象"));
            m_coMapAddBtn->setObjectName("coMapAddBtn");
            g->addWidget(m_coMapAddBtn, row++, 0, 1, 4);
            connect(m_coMapAddBtn, &QPushButton::clicked, this, &MainWindow::coMapAddRow);
        }
        updateCoPreview();
        return;
    }

    const int fn = m_coFuncBox ? m_coFuncBox->currentData(Qt::UserRole).toInt() : (int)canopen::F_NMT;
    int row = 0;
    auto lab = [&](const QString &t) { g->addWidget(new QLabel(t), row, 0); };

    if (fn == (int)canopen::F_NMT) {
        lab(QStringLiteral("状态命令字:"));
        m_coNmtCmdBox = new QComboBox;
        m_coNmtCmdBox->setObjectName("coNmtCmdBox");
        m_coNmtCmdBox->addItem(QStringLiteral("0x01 operational"), 0x01);
        m_coNmtCmdBox->addItem(QStringLiteral("0x02 stopped"), 0x02);
        m_coNmtCmdBox->addItem(QStringLiteral("0x80 pre-operational"), 0x80);
        m_coNmtCmdBox->addItem(QStringLiteral("0x81 reset node"), 0x81);
        m_coNmtCmdBox->addItem(QStringLiteral("0x82 reset communication"), 0x82);
        g->addWidget(m_coNmtCmdBox, row++, 1);
    } else if (fn == (int)canopen::F_SYNC) {
        g->addWidget(new QLabel(QStringLiteral("（SYNC 无数据）")), row++, 0, 1, 2);
    } else if (fn == (int)canopen::F_HEARTBEAT) {
        lab(QStringLiteral("状态字节:"));
        m_coHbStateBox = new QComboBox;
        m_coHbStateBox->setObjectName("coHbStateBox");
        m_coHbStateBox->addItem(QStringLiteral("0x00 boot-up"), 0x00);
        m_coHbStateBox->addItem(QStringLiteral("0x04 stopped"), 0x04);
        m_coHbStateBox->addItem(QStringLiteral("0x05 operational"), 0x05);
        m_coHbStateBox->addItem(QStringLiteral("0x7F pre-operational"), 0x7F);
        g->addWidget(m_coHbStateBox, row++, 1);
    } else if (fn == (int)canopen::F_RSDO) {
        lab(QStringLiteral("操作:"));
        m_coSdoOpBox = new QComboBox;
        m_coSdoOpBox->setObjectName("coSdoOpBox");
        m_coSdoOpBox->addItem(QStringLiteral("写 (download)"), 0);
        m_coSdoOpBox->addItem(QStringLiteral("读 (upload)"), 1);
        g->addWidget(m_coSdoOpBox, row++, 1);
        lab(QStringLiteral("索引 (EDS):"));
        m_coSdoIdxBox = new QComboBox;
        m_coSdoIdxBox->setObjectName("coSdoIdxBox");
        m_coSdoIdxBox->setMinimumWidth(300);
        {
            const QVector<canopen::OdEntry> od = canopen::objectDictionary();
            int dflt = 0;
            for (int k = 0; k < od.size(); ++k) {
                const canopen::OdEntry &e = od.at(k);
                m_coSdoIdxBox->addItem(canopen::odDisplay(e),
                    QVariant((uint)((e.index << 8) | (e.sub < 0 ? 0 : (e.sub & 0xFF)))));
                if (e.index == 0x6040 && (e.sub < 0 || e.sub == 0)) dflt = k;
            }
            m_coSdoIdxBox->setCurrentIndex(dflt);
        }
        g->addWidget(m_coSdoIdxBox, row++, 1);
        lab(QStringLiteral("值:"));
        m_coSdoValueEdit = new QLineEdit(QStringLiteral("0"));
        m_coSdoValueEdit->setObjectName("coSdoValueEdit");
        g->addWidget(m_coSdoValueEdit, row++, 1);

        auto setEn = [this]() {
            const bool w = !m_coSdoOpBox || m_coSdoOpBox->currentData().toInt() == 0;
            if (m_coSdoValueEdit) m_coSdoValueEdit->setEnabled(w);
        };
        connect(m_coSdoOpBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, setEn](int) { setEn(); updateCoPreview(); });
        connect(m_coSdoIdxBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { updateCoPreview(); });
        connect(m_coSdoValueEdit, &QLineEdit::textChanged, this, [this]() { updateCoPreview(); });
        setEn();
    } else {
        // RPDO / TPDO：**映射可配置**——每个映射对象的索引可选（取自 EDS），数值可填；
        // 默认取该 PDO 的 EDS 默认映射，用户可增删/替换为自定义映射关系。
        if (m_coPdoRows.isEmpty()) m_coPdoRows.append(CoPdoRow{});
        g->addWidget(new QLabel(QStringLiteral("（映射可配置：选择映射对象并填写数值；默认取 EDS 映射）")),
                     row++, 0, 1, 4);
        const QVector<canopen::OdEntry> od = canopen::objectDictionary();
        for (int i = 0; i < m_coPdoRows.size(); ++i) {
            const CoPdoRow &r = m_coPdoRows.at(i);
            auto *ib = new QComboBox;
            ib->setObjectName(QStringLiteral("coPdoIdx%1").arg(i));
            ib->setMinimumWidth(240);
            int sel = -1;
            for (int k = 0; k < od.size(); ++k) {
                const canopen::OdEntry &e = od.at(k);
                ib->addItem(canopen::odDisplay(e),
                            QVariant((uint)((e.index << 8) | (e.sub < 0 ? 0 : (e.sub & 0xFF)))));
                if (e.index == r.index && ((e.sub < 0 ? 0 : e.sub) == r.sub)) sel = k;
            }
            if (sel >= 0) ib->setCurrentIndex(sel);
            g->addWidget(ib, row, 0, 1, 2);
            m_coPdoIdxBoxes.append(ib);
            connect(ib, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int) { updateCoPreview(); });

            auto *ve = new QLineEdit(r.value);
            ve->setObjectName(QStringLiteral("coPdoVal%1").arg(i));
            g->addWidget(ve, row, 2);
            m_coPdoValEdits.append(ve);
            connect(ve, &QLineEdit::textChanged, this, [this]() { updateCoPreview(); });

            auto *rm = new QPushButton(QStringLiteral("×"));
            rm->setObjectName(QStringLiteral("coPdoRm%1").arg(i));
            rm->setFixedWidth(28);
            g->addWidget(rm, row, 3);
            connect(rm, &QPushButton::clicked, this, [this, i]() { coRemovePdoRow(i); });
            ++row;
        }
        m_coPdoAddBtn = new QPushButton(QStringLiteral("＋ 添加映射对象"));
        m_coPdoAddBtn->setObjectName("coPdoAddBtn");
        g->addWidget(m_coPdoAddBtn, row++, 0, 1, 4);
        connect(m_coPdoAddBtn, &QPushButton::clicked, this, &MainWindow::coAddPdoRow);
    }
    updateCoPreview();
}

// 从控件收集当前 PDO 行到模型（重建/持久化前调用）
void MainWindow::coSnapshotPdoRows()
{
    if (m_coPdoIdxBoxes.isEmpty()) return;
    m_coPdoRows.clear();
    for (int i = 0; i < m_coPdoIdxBoxes.size(); ++i) {
        CoPdoRow r;
        const uint packed = m_coPdoIdxBoxes.at(i)->currentData().toUInt();
        r.index = (packed >> 8) & 0xFFFF;
        r.sub   = packed & 0xFF;
        r.value = (i < m_coPdoValEdits.size() && m_coPdoValEdits.at(i))
                      ? m_coPdoValEdits.at(i)->text() : QStringLiteral("0");
        m_coPdoRows.append(r);
    }
}

void MainWindow::coMapAddRow()
{
    if (!m_coMapObjBoxes.isEmpty()) {   // 从控件收集当前行
        QVector<CoPdoRow> rows;
        for (QComboBox *ib : m_coMapObjBoxes) {
            const uint packed = ib->currentData().toUInt();
            CoPdoRow r; r.index = (packed >> 8) & 0xFFFF; r.sub = packed & 0xFF;
            rows.append(r);
        }
        m_coMapRows = rows;
    }
    CoPdoRow r; r.index = 0x6064; r.sub = 0;
    m_coMapRows.append(r);
    rebuildCoParams();
}

void MainWindow::coMapRemoveRow(int i)
{
    if (!m_coMapObjBoxes.isEmpty()) {
        QVector<CoPdoRow> rows;
        for (QComboBox *ib : m_coMapObjBoxes) {
            const uint packed = ib->currentData().toUInt();
            CoPdoRow r; r.index = (packed >> 8) & 0xFFFF; r.sub = packed & 0xFF;
            rows.append(r);
        }
        m_coMapRows = rows;
    }
    if (i >= 0 && i < m_coMapRows.size()) m_coMapRows.remove(i);
    rebuildCoParams();
}

void MainWindow::coAddPdoRow()
{
    coSnapshotPdoRows();
    m_coPdoRows.append(CoPdoRow{});
    rebuildCoParams();
}

void MainWindow::coRemovePdoRow(int i)
{
    coSnapshotPdoRows();
    if (i >= 0 && i < m_coPdoRows.size()) m_coPdoRows.remove(i);
    rebuildCoParams();
}



// ===========================================================================
//  Open 页「命令列表」：可追加、按顺序发送、上下调序/拖动
// ===========================================================================

// 按模型（m_coListModel）重建 QListWidget：左侧标签 + 右侧紧凑 ↑/↓/× 按钮
void MainWindow::coRebuildListWidget()
{
    if (!m_coList) return;
    // 先删除旧行控件（setItemWidget 的 widget 不会随 clear() 释放）
    for (int k = m_coList->count() - 1; k >= 0; --k) {
        QListWidgetItem *it = m_coList->item(k);
        if (QWidget *w = m_coList->itemWidget(it)) { m_coList->removeItemWidget(it); delete w; }
        delete m_coList->takeItem(k);
    }
    for (int i = 0; i < m_coListModel.size(); ++i) {
        // 占位 item：内容由 setItemWidget 的行控件渲染（含标签文本）
        auto *it = new QListWidgetItem(m_coList);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        it->setData(Qt::UserRole, i);        // 记录原始模型序号（拖动后据此重排）
        it->setSizeHint(QSize(0, m_coList->fontMetrics().height() + 8));

        auto *w = new QWidget;
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(2, 0, 2, 0);
        h->setSpacing(0);

        auto *lbl = new QLabel(QStringLiteral("%1. %2")
                                   .arg(i + 1).arg(m_coListModel.at(i).label));
        lbl->setObjectName(QStringLiteral("coListLbl%1").arg(i));
        h->addWidget(lbl, 1);                // 标签占满，按钮紧贴右侧

        auto mkBtn = [&](const QString &txt, const QString &on, int delta, bool en) {
            auto *b = new QToolButton;
            b->setText(txt);
            b->setObjectName(on);
            b->setAutoRaise(true);
            b->setFixedSize(20, 20);
            b->setEnabled(en);
            h->addWidget(b);
            return b;
        };
        auto *up = mkBtn(QStringLiteral("↑"), QStringLiteral("coListUp%1").arg(i), -1, i > 0);
        auto *dn = mkBtn(QStringLiteral("↓"), QStringLiteral("coListDown%1").arg(i), +1,
                         i < m_coListModel.size() - 1);
        auto *rm = mkBtn(QStringLiteral("×"), QStringLiteral("coListDel%1").arg(i), 0, true);
        connect(up, &QToolButton::clicked, this, [this, i]() { coMoveListRow(i, -1); });
        connect(dn, &QToolButton::clicked, this, [this, i]() { coMoveListRow(i, +1); });
        connect(rm, &QToolButton::clicked, this, [this, i]() { coRemoveListRow(i); });
        m_coList->setItemWidget(it, w);
    }
}

// 从控件同步模型（拖动后 QListWidget 顺序变了，label 里的序号需要重排）
void MainWindow::coSnapshotCoList()
{
    if (!m_coList) return;
    QVector<CoListEntry> reordered;
    for (int r = 0; r < m_coList->count(); ++r) {
        // 行文本形如 "n. label"；取 ']' 无，用首个 ". " 之后的原文不可靠，
        // 因此改为按当前控件顺序，用模型里 label 内容匹配重排：
        // 记录拖动前每个 item 通过 setData 携带的原始 index。
        auto *it = m_coList->item(r);
        const int orig = it->data(Qt::UserRole).toInt();
        if (orig >= 0 && orig < m_coListModel.size())
            reordered.append(m_coListModel.at(orig));
    }
    if (reordered.size() == m_coListModel.size())
        m_coListModel = reordered;
}

void MainWindow::coAddCurrentToList()
{
    if (curCoExample() >= 0) return;   // 实战示例不加入命令列表
    QString err;
    const QVector<QByteArray> frames = buildCoFrames(&err);
    if (frames.isEmpty()) { logWarn(QStringLiteral("无法追加：%1").arg(err)); return; }

    CoListEntry e;
    e.frames = frames;
    e.func = m_coFuncBox ? m_coFuncBox->currentData(Qt::UserRole).toInt() : -1;
    e.example = curCoExample();
    const int fn = e.func;
    if (fn == (int)canopen::F_RSDO) {
        const uint packed = m_coSdoIdxBox ? m_coSdoIdxBox->currentData().toUInt() : (0x6040u << 8);
        e.pv = { double(m_coSdoOpBox ? m_coSdoOpBox->currentData().toInt() : 0),
                 double((packed >> 8) & 0xFFFF), double(packed & 0xFF),
                 m_coSdoValueEdit ? m_coSdoValueEdit->text().toDouble() : 0.0 };
    } else if (fn == (int)canopen::F_NMT) {
        e.pv = { double(m_coNmtCmdBox ? m_coNmtCmdBox->currentData().toInt() : 1) };
    } else if (fn == (int)canopen::F_HEARTBEAT) {
        e.pv = { double(m_coHbStateBox ? m_coHbStateBox->currentData().toInt() : 5) };
    } else if (e.example >= 0) {
        for (int k = 0; k < m_coExValEdits.size(); ++k) {
            e.pv.append(m_coExValEdits.at(k) ? m_coExValEdits.at(k)->text().toDouble() : 0.0);
            QComboBox *ub = (k < m_coExUnitBoxes.size()) ? m_coExUnitBoxes.at(k) : nullptr;
            e.pu.append(ub ? ub->currentIndex() : 0);
        }
    } else {   // PDO：仅数值（映射固定）
        for (int k = 0; k < m_coPdoValEdits.size(); ++k)
            e.pv.append(m_coPdoValEdits.at(k) ? m_coPdoValEdits.at(k)->text().toDouble() : 0.0);
    }
    // 标签：普通命令=「功能名 ID=0x.. DATA=..」；示例=「功能名（N 帧）」
    QString fnName = m_coFuncBox ? m_coFuncBox->currentText() : QString();
    if (frames.size() == 1) {
        proto::RxFrame rx;
        if (proto::decodeDataFrame(frames.first(), &rx))
            e.label = QStringLiteral("%1 ID=0x%2 DATA=%3")
                          .arg(fnName,
                               QString::number(rx.id, 16).toUpper().rightJustified(3, QLatin1Char('0')),
                               QString::fromLatin1(rx.data.toHex(' ').toUpper()));
        else
            e.label = fnName;
    } else {
        e.label = QStringLiteral("%1（%2 帧）").arg(fnName).arg(frames.size());
    }
    if (m_coEditRow >= 0 && m_coEditRow < m_coListModel.size()) {
        m_coListModel[m_coEditRow] = e;             // 覆盖选中的列表项
        logInfo(QStringLiteral("已更新命令列表第 %1 项：%2").arg(m_coEditRow + 1).arg(e.label));
    } else {
        m_coListModel.append(e);
        logInfo(QStringLiteral("已追加到命令列表：%1").arg(e.label));
    }
    m_coEditRow = -1;
    if (m_coAddBtn) m_coAddBtn->setText(QStringLiteral("＋ 追加到列表"));
    coRebuildListWidget();
    updateCoPreview();
}

// 点击列表项：把该命令回填到编辑器（供再次修改；再次「更新」将覆盖该项）
void MainWindow::coRestoreFromEntry(int row)
{
    if (row < 0 || row >= m_coListModel.size()) return;
    const CoListEntry &e = m_coListModel.at(row);
    if (m_coFuncBox) {
        int r = -1;
        if (e.example >= 0) {
            for (int i = 0; i < m_coFuncBox->count(); ++i) {
                const QVariant v = m_coFuncBox->itemData(i, Qt::UserRole + 1);
                if (v.isValid() && v.toInt() == e.example) { r = i; break; }
            }
        } else {
            r = m_coFuncBox->findData(e.func, Qt::UserRole);
        }
        if (r >= 0) m_coFuncBox->setCurrentIndex(r);
    }
    rebuildCoParams();   // 按功能重建控件

    const int fn = m_coFuncBox ? m_coFuncBox->currentData(Qt::UserRole).toInt() : -1;
    if (fn == (int)canopen::F_RSDO) {
        if (m_coSdoOpBox && e.pv.size() > 0) {
            int i = m_coSdoOpBox->findData((int)e.pv.at(0)); if (i >= 0) m_coSdoOpBox->setCurrentIndex(i);
        }
        if (m_coSdoIdxBox && e.pv.size() > 2) {
            const uint packed = ((uint)e.pv.at(1) << 8) | ((uint)e.pv.at(2) & 0xFF);
            int i = m_coSdoIdxBox->findData(packed); if (i >= 0) m_coSdoIdxBox->setCurrentIndex(i);
        }
        if (m_coSdoValueEdit && e.pv.size() > 3)
            m_coSdoValueEdit->setText(QString::number(e.pv.at(3)));
    } else if (fn == (int)canopen::F_NMT) {
        if (m_coNmtCmdBox && !e.pv.isEmpty()) {
            int i = m_coNmtCmdBox->findData((int)e.pv.at(0)); if (i >= 0) m_coNmtCmdBox->setCurrentIndex(i);
        }
    } else if (fn == (int)canopen::F_HEARTBEAT) {
        if (m_coHbStateBox && !e.pv.isEmpty()) {
            int i = m_coHbStateBox->findData((int)e.pv.at(0)); if (i >= 0) m_coHbStateBox->setCurrentIndex(i);
        }
    } else if (e.example >= 0) {
        for (int k = 0; k < m_coExValEdits.size() && k < e.pv.size(); ++k)
            if (m_coExValEdits.at(k)) m_coExValEdits.at(k)->setText(QString::number(e.pv.at(k)));
        for (int k = 0; k < m_coExUnitBoxes.size() && k < e.pu.size(); ++k)
            if (m_coExUnitBoxes.at(k) && e.pu.at(k) >= 0) m_coExUnitBoxes.at(k)->setCurrentIndex(e.pu.at(k));
    } else {   // PDO
        for (int k = 0; k < m_coPdoValEdits.size() && k < e.pv.size(); ++k)
            if (m_coPdoValEdits.at(k)) m_coPdoValEdits.at(k)->setText(QString::number(e.pv.at(k)));
    }
    m_coEditRow = row;
    if (m_coAddBtn) m_coAddBtn->setText(QStringLiteral("更新选中项"));
    updateCoPreview();
}

void MainWindow::coMoveListRow(int row, int delta)
{
    if (!m_coList) return;
    coSnapshotCoList();                 // 先把拖动后的顺序落定
    const int to = row + delta;
    if (row < 0 || row >= m_coListModel.size() || to < 0 || to >= m_coListModel.size()) return;
    m_coListModel.move(row, to);
    coRebuildListWidget();
    updateCoPreview();
}

void MainWindow::coRemoveListRow(int row)
{
    if (!m_coList) return;
    coSnapshotCoList();
    if (row >= 0 && row < m_coListModel.size()) m_coListModel.remove(row);
    coRebuildListWidget();
    updateCoPreview();
}

// 从 QSettings 恢复命令列表
void MainWindow::coLoadCoList()
{
    QSettings st;
    m_coListModel.clear();
    const int n = st.value("co/listN", 0).toInt();
    for (int i = 0; i < n; ++i) {
        CoListEntry e;
        e.label = st.value(QStringLiteral("co/listLabel%1").arg(i), QString()).toString();
        e.func = st.value(QStringLiteral("co/listFunc%1").arg(i), -1).toInt();
        e.example = st.value(QStringLiteral("co/listEx%1").arg(i), -1).toInt();
        const int pvn = st.value(QStringLiteral("co/listPvN%1").arg(i), 0).toInt();
        for (int k = 0; k < pvn; ++k) e.pv.append(st.value(QStringLiteral("co/listPv%1_%2").arg(i).arg(k), 0.0).toDouble());
        const int pun = st.value(QStringLiteral("co/listPuN%1").arg(i), 0).toInt();
        for (int k = 0; k < pun; ++k) e.pu.append(st.value(QStringLiteral("co/listPu%1_%2").arg(i).arg(k), 0).toInt());
        const int fn = st.value(QStringLiteral("co/listFrameN%1").arg(i), 0).toInt();
        for (int j = 0; j < fn; ++j)
            e.frames.append(QByteArray::fromHex(
                st.value(QStringLiteral("co/listF%1_%2").arg(i).arg(j), QString()).toString().toLatin1()));
        m_coListModel.append(e);
    }
    coRebuildListWidget();
    updateCoPreview();
}

// 生成待发送帧序列：普通功能=1 帧；实战示例=固定序列
QVector<QByteArray> MainWindow::buildCoFrames(QString *err) const
{
    QVector<QByteArray> out;
    auto dataFrame = [](quint32 id, const QByteArray &payload) {
        return proto::buildDataFrame(id, false, false, false, false,
                                     proto::bytesToDlc((uint8_t)payload.size()), payload);
    };
    const int ex = curCoExample();
    if (ex >= 0 && ex < canopen::examples().size()) {
        const canopen::Example &e = canopen::examples().at(ex);
        if (e.name.contains(QStringLiteral("映射修改")) && m_coMapPdoBox) {
            const int n2 = m_coNodeEdit ? m_coNodeEdit->text().toInt() : 1;
            const canopen::Function pdo = (canopen::Function)m_coMapPdoBox->currentData().toInt();
            QVector<canopen::PdoMapEntry> objs;
            for (QComboBox *ib : m_coMapObjBoxes) {
                const uint packed = ib->currentData().toUInt();
                canopen::PdoMapEntry m;
                m.index = (packed >> 8) & 0xFFFF;
                m.sub   = packed & 0xFF;
                const canopen::OdEntry *oe = canopen::findOd(m.index, m.sub);
                m.bits  = oe ? canopen::odBitLen(*oe) : 32;
                objs.append(m);
            }
            for (const canopen::Wire &w : canopen::buildPdoMapChange(n2, pdo, objs))
                out.append(dataFrame(w.id, w.data));
            return out;
        }
        const double gear = m_coGearEdit ? m_coGearEdit->text().toDouble() : 1.0;
        QVector<double> vals;
        QVector<int>    units;
        for (int k = 0; k < e.inputs.size(); ++k) {
            vals.append((k < m_coExValEdits.size() && m_coExValEdits.at(k))
                            ? m_coExValEdits.at(k)->text().toDouble() : e.inputs.at(k).defValue);
            QComboBox *ub = (k < m_coExUnitBoxes.size()) ? m_coExUnitBoxes.at(k) : nullptr;
            units.append(ub ? ub->currentIndex() : e.inputs.at(k).defUnit);
        }
        for (const canopen::Wire &w : canopen::buildExampleFrames(e, vals, units, gear))
            out.append(dataFrame(w.id, w.data));
        return out;
    }
    const int node = m_coNodeEdit ? m_coNodeEdit->text().toInt() : 1;
    if (node < 0 || node > 127) { if (err) *err = QStringLiteral("node_id 需在 0..127"); return out; }
    const int fn = m_coFuncBox ? m_coFuncBox->currentData(Qt::UserRole).toInt() : (int)canopen::F_NMT;
    const canopen::Function f = (canopen::Function)fn;

    if (f == canopen::F_NMT) {
        const quint8 cmd = m_coNmtCmdBox ? (quint8)m_coNmtCmdBox->currentData().toInt() : 0x01;
        out.append(dataFrame(canopen::nmtId(), canopen::buildNmt(node, cmd)));
    } else if (f == canopen::F_SYNC) {
        out.append(dataFrame(canopen::syncId(), QByteArray()));
    } else if (f == canopen::F_HEARTBEAT) {
        const quint8 st = m_coHbStateBox ? (quint8)m_coHbStateBox->currentData().toInt() : 0x05;
        out.append(dataFrame(canopen::cobId(f, node), canopen::buildHeartbeat(node, st)));
    } else if (f == canopen::F_RSDO) {
        const uint packed = m_coSdoIdxBox ? m_coSdoIdxBox->currentData().toUInt() : (0x6040u << 8);
        const quint16 idx = (packed >> 8) & 0xFFFF;
        const quint8  sub = packed & 0xFF;
        const bool write = !m_coSdoOpBox || m_coSdoOpBox->currentData().toInt() == 0;
        QByteArray payload;
        if (write) {
            const canopen::OdEntry *od = canopen::findOd(idx, sub);
            const int ty = od ? canopen::sdoTypeFromOd(od->dataType) : canopen::SDO_UINT32;
            payload = canopen::buildSdoWrite(node, idx, sub,
                          m_coSdoValueEdit ? m_coSdoValueEdit->text().toDouble() : 0.0, ty);
        } else {
            payload = canopen::buildSdoRead(node, idx, sub);
        }
        out.append(dataFrame(canopen::cobId(f, node), payload));
    } else {
        // PDO：按映射（用户可选对象）+ 数值 → 编码后拼接（≤8B）
        QByteArray data;
        for (int i = 0; i < m_coPdoIdxBoxes.size(); ++i) {
            const uint packed = m_coPdoIdxBoxes.at(i)->currentData().toUInt();
            const quint16 idx = (packed >> 8) & 0xFFFF;
            const int     sub = packed & 0xFF;
            const canopen::OdEntry *e = canopen::findOd(idx, sub);
            if (!e) continue;
            const double v = (i < m_coPdoValEdits.size() && m_coPdoValEdits.at(i))
                                 ? m_coPdoValEdits.at(i)->text().toDouble() : 0.0;
            data += canopen::encodeOdValue(*e, v);
        }
        out.append(dataFrame(canopen::cobId(f, node), data.left(8)));
    }
    return out;
}

// 刷新预览：列表非空时展示整个列表的帧（按顺序 → 拼接，前方显示总帧数）
void MainWindow::updateCoPreview()
{
    if (!m_coPreview) return;
    auto label = [](const QByteArray &fr) {
        proto::RxFrame rx;
        if (!proto::decodeDataFrame(fr, &rx)) return QString();
        return QStringLiteral("ID=0x%1 DATA=%2")
            .arg(QString::number(rx.id, 16).toUpper().rightJustified(3, QLatin1Char('0')),
                 QString::fromLatin1(rx.data.toHex(' ').toUpper()));
    };
    // 实战示例：单独呈现（其自身序列），不并入命令列表
    QVector<QByteArray> frames;
    if (curCoExample() >= 0) {
        QString err;
        frames = buildCoFrames(&err);
        if (frames.isEmpty()) { m_coPreview->setText(err); return; }
    } else {
        // 命令列表非空：按列表顺序展示全部帧
        for (const CoListEntry &e : m_coListModel)
            for (const QByteArray &f : e.frames) frames.append(f);
        if (frames.isEmpty()) {             // 列表为空：回退到当前编辑器命令
            QString err;
            frames = buildCoFrames(&err);
            if (frames.isEmpty()) { m_coPreview->setText(err); return; }
        }
    }
    QStringList parts;
    for (const QByteArray &fr : frames) parts << label(fr);
    m_coPreview->setText(QStringLiteral("共 %1 帧： %2")
        .arg(frames.size()).arg(parts.join(QStringLiteral("  →  "))));
}

void MainWindow::onCoSend()
{
    if (!m_link || !m_link->isOpen()) { logWarn(QStringLiteral("串口未打开。")); return; }
    // 实战示例：单独发送其序列（不并入命令列表）
    if (curCoExample() >= 0) {
        QString err;
        const QVector<QByteArray> frames = buildCoFrames(&err);
        if (frames.isEmpty()) { logWarn(QStringLiteral("无法发送：%1").arg(err)); return; }
        queueCommand(proto::CMD_SET_MODE, proto::MODE_FRAMELIST, QStringLiteral("发送模式 列表"));
        for (const QByteArray &f : frames) queueBytes(f, QStringLiteral("示例帧"));
        queueCommand(proto::CMD_TX_START, 0u, QStringLiteral("触发发送 (单次)"));
        flushTx(QStringLiteral("CANopen 示例发送"));
        for (const QByteArray &f : frames) {
            proto::RxFrame rx;
            if (proto::decodeDataFrame(f, &rx)) coAppendScroll(QStringLiteral("TX"), rx);
        }
        logInfo(QStringLiteral("已发送实战示例（%1 帧）").arg(frames.size()));
        return;
    }
    // 命令列表非空：按列表顺序依次发送全部帧
    if (!m_coListModel.isEmpty()) {
        QVector<QByteArray> all;
        for (const CoListEntry &e : m_coListModel)
            for (const QByteArray &f : e.frames) all.append(f);
        queueCommand(proto::CMD_SET_MODE, proto::MODE_FRAMELIST, QStringLiteral("发送模式 列表"));
        for (const QByteArray &f : all) queueBytes(f, QStringLiteral("命令列表帧"));
        queueCommand(proto::CMD_TX_START, 0u, QStringLiteral("触发发送 (单次)"));
        flushTx(QStringLiteral("CANopen 列表发送"));
        for (const QByteArray &f : all) {
            proto::RxFrame rx;
            if (proto::decodeDataFrame(f, &rx)) coAppendScroll(QStringLiteral("TX"), rx);
        }
        logInfo(QStringLiteral("已按列表发送 %1 条 / %2 帧")
                    .arg(m_coListModel.size()).arg(all.size()));
        return;
    }
    QString err;
    const QVector<QByteArray> frames = buildCoFrames(&err);
    if (frames.isEmpty()) { logWarn(QStringLiteral("无法发送：%1").arg(err)); return; }

    if (curCoExample() >= 0) {                       // 实战示例：序列（列表模式）
        queueCommand(proto::CMD_SET_MODE, proto::MODE_FRAMELIST, QStringLiteral("发送模式 列表"));
        for (const QByteArray &f : frames) queueBytes(f, QStringLiteral("示例帧"));
        queueCommand(proto::CMD_TX_START, 0u, QStringLiteral("触发发送 (单次)"));
        flushTx(QStringLiteral("CANopen 示例发送"));
    } else {                                         // 普通功能：单帧（数据变才打包）
        const QByteArray &f = frames.first();
        const bool changed = (!m_coLastInit || m_coLastFrame != f);
        queueCommand(proto::CMD_SET_MODE, proto::MODE_SINGLE, QStringLiteral("发送模式 单帧"));
        if (changed) { queueBytes(f, QStringLiteral("CANopen 帧")); m_coLastFrame = f; m_coLastInit = true; }
        queueCommand(proto::CMD_TX_START, 0u, QStringLiteral("触发发送 (单次)"));
        flushTx(QStringLiteral("CANopen 发送"));
    }
    for (const QByteArray &f : frames) {
        proto::RxFrame rx;
        if (proto::decodeDataFrame(f, &rx)) coAppendScroll(QStringLiteral("TX"), rx);
    }
    logInfo(QStringLiteral("已发送 %1（%2 帧）")
        .arg(m_coFuncBox ? m_coFuncBox->currentText() : QString()).arg(frames.size()));
}

void MainWindow::onCoStop()
{
    if (!m_link || !m_link->isOpen()) return;
    sendCommand(proto::CMD_TX_STOP, 0, QStringLiteral("停止发送"));
}

// 导入外部 EDS（复制到 <exe目录>/eds/ds402.eds；重启后仍生效）
void MainWindow::onImportEds()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入 EDS 文件"), QString(),
        QStringLiteral("EDS 文件 (*.eds *.eda);;所有文件 (*)"));
    if (path.isEmpty()) return;
    QString err;
    if (!canopen::importEdsFile(path, &err)) {
        logError(QStringLiteral("导入 EDS 失败：%1").arg(err));
        return;
    }
    logInfo(QStringLiteral("已导入 EDS：%1（%2 个对象）")
                .arg(canopen::edsSourceName()).arg(canopen::objectDictionary().size()));
    coSnapshotPdoRows();
    rebuildCoParams();          // 刷新 EDS 索引下拉
}

// 恢复内置 EDS（删除外部文件）
void MainWindow::onResetEds()
{
    canopen::resetToEmbeddedEds();
    logInfo(QStringLiteral("已恢复内置 EDS（%1 个对象）").arg(canopen::objectDictionary().size()));
    coSnapshotPdoRows();
    rebuildCoParams();
}

// 对象字典浏览器：读/写 → 经当前链路发 SDO 请求
void MainWindow::onOpenObjectDictionary()
{
    ObjectDictionaryDialog dlg(this);
    auto sendSdo = [this](const QByteArray &frame, const QString &note) {
        if (!m_link || !m_link->isOpen()) { logWarn(QStringLiteral("串口未打开。")); return; }
        queueCommand(proto::CMD_SET_MODE, proto::MODE_SINGLE, QStringLiteral("发送模式 单帧"));
        queueBytes(frame, note);
        queueCommand(proto::CMD_TX_START, 0u, QStringLiteral("触发发送 (单次)"));
        flushTx(note);
        proto::RxFrame rx;
        if (proto::decodeDataFrame(frame, &rx)) coAppendScroll(QStringLiteral("TX"), rx);
    };
    connect(&dlg, &ObjectDictionaryDialog::sdoRead, this,
            [this, sendSdo](quint16 idx, quint8 sub) {
        const int node = m_coNodeEdit ? m_coNodeEdit->text().toInt() : 1;
        sendSdo(proto::buildDataFrame(canopen::cobId(canopen::F_RSDO, node), false, false, false,
                                      false, proto::bytesToDlc(8), canopen::buildSdoRead(node, idx, sub)),
                QStringLiteral("SDO 读"));
    });
    connect(&dlg, &ObjectDictionaryDialog::sdoWrite, this,
            [this, sendSdo](quint16 idx, quint8 sub, double value, int type) {
        const int node = m_coNodeEdit ? m_coNodeEdit->text().toInt() : 1;
        sendSdo(proto::buildDataFrame(canopen::cobId(canopen::F_RSDO, node), false, false, false,
                                      false, proto::bytesToDlc(8), canopen::buildSdoWrite(node, idx, sub, value, type)),
                QStringLiteral("SDO 写"));
    });
    dlg.exec();
}

// 接收：就地汇总（按帧 id）+ 滚动
void MainWindow::coFeedRx(const proto::RxFrame &f)
{
    if (!m_coRxTable) return;
    int row = m_coRxRowById.value(f.id, -1);
    if (row < 0) {
        row = m_coRxTable->rowCount();
        m_coRxTable->insertRow(row);
        for (int c = 0; c < RX_COL_COUNT; c++) {
            auto *it = new QTableWidgetItem;
            it->setFlags(Qt::ItemIsEnabled);
            m_coRxTable->setItem(row, c, it);
        }
        m_coRxRowById.insert(f.id, row);
    }
    const int count = m_coRxTable->item(row, RX_COUNT)->text().toInt() + 1;
    m_coRxTable->item(row, RX_ID)->setText(
        QStringLiteral("0x%1").arg(f.id, f.ext ? 8 : 3, 16, QLatin1Char('0')).toUpper());
    m_coRxTable->item(row, RX_TYPE)->setText(rxTypeText(f));
    m_coRxTable->item(row, RX_LEN)->setText(QString::number(f.data.size()));
    m_coRxTable->item(row, RX_DATA)->setText(QString::fromLatin1(f.data.toHex(' ').toUpper()));
    m_coRxTable->item(row, RX_COUNT)->setText(QString::number(count));
    coAppendScroll(QStringLiteral("RX"), f);
    applyRxFilter(m_coRxTable);
}

void MainWindow::coAppendScroll(const QString &dir, const proto::RxFrame &f)
{
    if (!m_coScrollTable || !m_coScrollMode) return;
    QScrollBar *vsb = m_coScrollTable->verticalScrollBar();
    const bool follow = (vsb->value() >= vsb->maximum());
    const int row = m_coScrollTable->rowCount();
    m_coScrollTable->insertRow(row);
    QString dirTxt = dir;
    if (m_coTsBox && m_coTsBox->isChecked())
        dirTxt = QStringLiteral("%1 %2").arg(QTime::currentTime().toString("HH:mm:ss.zzz"), dir);
    auto set = [&](int c, const QString &t) {
        auto *it = new QTableWidgetItem(t);
        it->setFlags(Qt::ItemIsEnabled);
        m_coScrollTable->setItem(row, c, it);
    };
    set(SC_DIR, dirTxt);
    set(SC_ID,  QStringLiteral("0x%1").arg(f.id, f.ext ? 8 : 3, 16, QLatin1Char('0')).toUpper());
    set(SC_TYPE, rxTypeText(f));
    set(SC_LEN, QString::number(f.data.size()));
    set(SC_DATA, QString::fromLatin1(f.data.toHex(' ').toUpper()));
    if (m_coScrollTable->rowCount() > 5000) m_coScrollTable->removeRow(0);
    applyRxFilter(m_coScrollTable);
    if (follow) m_coScrollTable->scrollToBottom();
}

void MainWindow::coSetRxMode(bool scroll)
{
    m_coScrollMode = scroll;
    if (m_coRxTable) m_coRxTable->setVisible(!scroll);
    if (m_coScrollTable) m_coScrollTable->setVisible(scroll);
    if (QWidget *r = filterRowFor(m_coRxTable)) r->setVisible(!scroll);
    if (QWidget *r = filterRowFor(m_coScrollTable)) r->setVisible(scroll);
    if (m_coModeLbl) m_coModeLbl->setText(scroll ? QStringLiteral("滚动") : QStringLiteral("就地"));
}

void MainWindow::onAbout()
{
    const bool connected = (m_link && m_link->isOpen());
    if (connected)
        sendCommand(proto::CMD_GET_VERSION, 0, QStringLiteral("查询固件版本"));

    QTimer::singleShot(connected ? 300 : 0, this, [this]() {
#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif
        const QString devFw = m_deviceFwVersion.isEmpty()
            ? QStringLiteral("未知（固件未实现命令 10）") : m_deviceFwVersion;
        QMessageBox::about(this, QStringLiteral("关于"),
            QStringLiteral("<b>CyberBeast CANable 2 上位机</b><br>软件版本：%1<br>设备固件版本：%2")
                .arg(QStringLiteral(APP_VERSION), devFw));
    });
}

// ---------------------------------------------------------------------------
//  更新固件：进入 DFU → 轮询检测 DFU 设备 → 打开固件更新对话框
// ---------------------------------------------------------------------------
void MainWindow::onUpdateFirmware()
{
    // 1) 若已打开，发 ENTER_DFU 命令让 MCU 进入 DFU，然后关闭连接
    if (m_link->isOpen()) {
        sendCommand(proto::CMD_ENTER_DFU, 0, QStringLiteral("进入 DFU"));
        m_link->flush();   // 确保命令真正写出后再关闭（串口写是异步的）
        logInfo(QStringLiteral("已发送进入 DFU 命令，等待设备重新枚举..."));
        m_link->close();
    }

    // 2) 轮询检测 DFU 设备（最多约 9 秒），检测到或超时后打开固件更新对话框
    if (!m_dfuPollTimer) {
        m_dfuPollTimer = new QTimer(this);
        connect(m_dfuPollTimer, &QTimer::timeout, this, [this]() {
            dfu::Info info = dfu::findDevice();
            if (info.found) {
                m_dfuPollTimer->stop();
                logInfo(QStringLiteral("检测到 DFU 设备 %1:%2，总线%3 地址%4")
                            .arg(info.vid, 4, 16, QLatin1Char('0'))
                            .arg(info.pid, 4, 16, QLatin1Char('0'))
                            .arg(info.bus).arg(info.address));
                FirmwareDialog dlg(info, this);
                dlg.exec();
            } else if (++m_dfuPollCount * 300 >= 9000) {
                m_dfuPollTimer->stop();
                logInfo(QStringLiteral("未检测到 DFU 设备，仍打开固件更新对话框（可手动确认）。"));
                FirmwareDialog dlg(dfu::Info{}, this);
                dlg.exec();
            }
        });
    }
    m_dfuPollCount = 0;
    logInfo(QStringLiteral("正在等待 DFU 设备..."));
    m_dfuPollTimer->start(300);
}

// ---------------------------------------------------------------------------
void MainWindow::queueBytes(const QByteArray &bytes, const QString &note)
{
    if (bytes.isEmpty())
        return;
    m_txBuf.append(bytes);   // 拼接到同一条消息
}

void MainWindow::queueCommand(proto::Cmd cmd, quint32 value, const QString &note)
{
    queueBytes(proto::buildCommand(cmd, value), note);
}

void MainWindow::flushTx(const QString &note)
{
    if (m_txBuf.isEmpty())
        return;
    m_link->send(m_txBuf);          // 一次性写出整条消息
    m_lastFlush = m_txBuf;
    m_txFlushCount++;
    (void)note;
    m_txBuf.clear();
}

void MainWindow::sendCommand(proto::Cmd cmd, quint32 value, const QString &note)
{
    queueCommand(cmd, value, note);
    flushTx(note);
}

void MainWindow::logWarn(const QString &text)
{
    const QString line = QStringLiteral("[%1] 警告  %2")
        .arg(QTime::currentTime().toString("HH:mm:ss.zzz"), text);
    for (const LogPanel &p : m_logPanels)
        if (p.log) p.log->appendPlainText(line);
}

void MainWindow::logError(const QString &text)
{
    const QString line = QStringLiteral("[%1] 错误  %2")
        .arg(QTime::currentTime().toString("HH:mm:ss.zzz"), text);
    for (const LogPanel &p : m_logPanels)
        if (p.log) p.log->appendPlainText(line);
}

// 状态提示不进日志区，改写到状态栏
void MainWindow::logInfo(const QString &text)
{
    if (m_statusLbl) m_statusLbl->setText(text);
}

void MainWindow::appendScroll(const QString &dir, const proto::RxFrame &f)
{
    if (!m_scrollTable || !m_scrollMode) return;

    // 仅当滑条已在最底端时才自动跟随滚动；否则保留用户当前查看位置
    // （数据仍在下方追加，只是不强制把视图拉到底部）
    QScrollBar *vsb = m_scrollTable->verticalScrollBar();
    const bool follow = (vsb->value() >= vsb->maximum());

    const int row = m_scrollTable->rowCount();
    m_scrollTable->insertRow(row);
    QString dirTxt = dir;
    if (m_tsBox && m_tsBox->isChecked())
        dirTxt = QStringLiteral("%1 %2").arg(QTime::currentTime().toString("HH:mm:ss.zzz"), dir);
    auto set = [&](int c, const QString &t) {
        auto *it = new QTableWidgetItem(t);
        it->setFlags(Qt::ItemIsEnabled);
        m_scrollTable->setItem(row, c, it);
    };
    set(SC_DIR, dirTxt);
    set(SC_ID,  QStringLiteral("0x%1").arg(f.id, f.ext ? 8 : 3, 16, QLatin1Char('0')).toUpper());
    set(SC_TYPE, rxTypeText(f));
    set(SC_LEN, QString::number(f.data.size()));
    set(SC_DATA, QString::fromLatin1(f.data.toHex(' ').toUpper()));
    if (m_scrollTable->rowCount() > 5000) m_scrollTable->removeRow(0);   // 上限，滚掉最旧行
    applyRxFilter(m_scrollTable);
    if (follow) m_scrollTable->scrollToBottom();   // 仅当原本在底部时才跟随
}

// ===========================================================================
//  接收区筛选：每列表头上方一个输入框，隐藏不匹配的行（包含匹配，忽略大小写）
// ===========================================================================
QWidget *MainWindow::makeRxFilterRow(const QString &name, QTableWidget *table,
                                     const QStringList &labels)
{
    RxFilterBar bar;
    bar.name = name;
    bar.table = table;
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);
    const int idx = m_rxFilters.size();
    for (int c = 0; c < labels.size(); ++c) {
        auto *e = new QLineEdit;
        e->setObjectName(name + QStringLiteral(".f%1").arg(c));
        e->setPlaceholderText(labels.at(c));
        e->setClearButtonEnabled(true);
        e->setMinimumWidth(52);
        // 数据列给更宽
        h->addWidget(e, labels.at(c).contains(QStringLiteral("数据")) ? 3 : 1);
        bar.edits.append(e);
        connect(e, &QLineEdit::textChanged, this, [this, idx]() { applyRxFilterIdx(idx); });
    }
    m_rxFilters.append(bar);
    m_rxFilters[idx].row = w;
    w->setVisible(table->isVisible());   // 随表初始可见性
    return w;
}

void MainWindow::applyRxFilterIdx(int idx)
{
    if (idx < 0 || idx >= m_rxFilters.size()) return;
    RxFilterBar &b = m_rxFilters[idx];
    if (!b.table) return;
    for (int r = 0; r < b.table->rowCount(); ++r) {
        bool show = true;
        for (int c = 0; c < b.edits.size(); ++c) {
            const QString f = b.edits.at(c)->text().trimmed();
            if (f.isEmpty()) continue;
            auto *it = b.table->item(r, c);
            const QString t = it ? it->text() : QString();
            if (!t.contains(f, Qt::CaseInsensitive)) { show = false; break; }
        }
        b.table->setRowHidden(r, !show);
    }
}

void MainWindow::applyRxFilter(QTableWidget *table)
{
    for (int i = 0; i < m_rxFilters.size(); ++i)
        if (m_rxFilters.at(i).table == table) { applyRxFilterIdx(i); return; }
}

QWidget *MainWindow::filterRowFor(QTableWidget *table)
{
    for (const RxFilterBar &b : m_rxFilters)
        if (b.table == table) return b.row;
    return nullptr;
}


// 切换接收区显示模式：false=就地更新（按 ID 汇总）, true=滚动显示（发一条收一条）
void MainWindow::setRxMode(bool scroll)
{
    m_scrollMode = scroll;
    if (m_rxTable) m_rxTable->setVisible(!scroll);
    if (m_scrollTable) m_scrollTable->setVisible(scroll);
    if (QWidget *r = filterRowFor(m_rxTable)) r->setVisible(!scroll);
    if (QWidget *r = filterRowFor(m_scrollTable)) r->setVisible(scroll);
    if (m_modeLbl)
        m_modeLbl->setText(scroll ? QStringLiteral("滚动") : QStringLiteral("就地"));
    if (m_modeBtn)
        m_modeBtn->setToolTip(scroll ? QStringLiteral("点击切换为：就地更新")
                                     : QStringLiteral("点击切换为：滚动显示"));
}

void MainWindow::onFrameReceived(const proto::RxFrame &f)
{
    feedRx(f);                                        // 通用接收表就地更新 + 滚动回显
    simpleFeedRx(f);                                  // Simple 页接收区同步
    coFeedRx(f);                                      // CANopen(Open) 页接收区同步
}

void MainWindow::onResponseReceived(const proto::RxResponse &r)
{
    if (r.id == proto::RSP_VERSION) {
        m_deviceFwVersion = r.text;
        logInfo(QStringLiteral("设备固件版本：%1").arg(r.text.isEmpty() ? QStringLiteral("(空)") : r.text));
    } else if (r.id == proto::RSP_BITRATE_LOCK) {
        logInfo(QStringLiteral("波特率锁定 = %1 Hz").arg(r.value));
        for (int i = 0; i < m_bitrateBox->count(); i++) {
            if ((quint32)m_bitrateBox->itemData(i).toUInt() == r.value) {
                QSignalBlocker b(m_bitrateBox);
                m_bitrateBox->setCurrentIndex(i);
                break;
            }
        }
    } else {
        logInfo(QStringLiteral("AA AC id=%1 value=%2").arg(r.id).arg(r.value));
    }
}

void MainWindow::onStatusLine(const QString &line)
{
    logInfo(QStringLiteral("状态: %1").arg(line));
}

QString MainWindow::logText() const
{
    return (m_logPanels.isEmpty() || !m_logPanels.first().log)
               ? QString() : m_logPanels.first().log->toPlainText();
}

// ---------------------------------------------------------------------------
//  AA AD 警告/报错帧 -> 日志区（日志区仅显示警告与报错）
// ---------------------------------------------------------------------------
void MainWindow::onLogMessage(const proto::LogMessage &m)
{
    QString t = m.text;
    if (t.isEmpty())
        t = QStringLiteral("AA AD（无正文）");
    if (!m.crcOk)
        t += QStringLiteral("  (CRC 校验失败)");
    // 无显式级别字段：正文含 WARN / 警告 视为警告，否则按报错
    if (t.contains(QStringLiteral("WARN"), Qt::CaseInsensitive) ||
        t.contains(QStringLiteral("警告")))
        logWarn(t);
    else
        logError(t);
}

QString MainWindow::scrollText() const
{
    if (!m_scrollTable) return QString();
    QString out;
    for (int r = 0; r < m_scrollTable->rowCount(); r++) {
        QStringList cells;
        for (int c = 0; c < SC_COL_COUNT; c++) {
            auto *it = m_scrollTable->item(r, c);
            cells << (it ? it->text() : QString());
        }
        out += cells.join(QLatin1Char(' ')) + QLatin1Char('\n');
    }
    return out;
}

// ---------------------------------------------------------------------------
//  持久化（QSettings）：保存/恢复全部可填项（不含窗口几何）
// ---------------------------------------------------------------------------
void MainWindow::saveSettings()
{
    QSettings st;
    st.setValue("table/rows", m_table->rowCount());
    for (int r = 0; r < m_table->rowCount(); r++) {
        const QString p = QStringLiteral("table/%1/").arg(r);
        auto *chk = m_table->item(r, COL_CHK);
        st.setValue(p + "check", chk && chk->checkState() == Qt::Checked);
        auto *idIt = m_table->item(r, COL_ID);
        st.setValue(p + "id", idIt ? idIt->text() : QString());
        auto *dIt = m_table->item(r, COL_DATA);
        st.setValue(p + "data", dIt ? dIt->text() : QString());
        auto cdata = [&](int col, int def) -> int {
            auto *w = qobject_cast<QComboBox *>(m_table->cellWidget(r, col));
            return w ? w->currentData().toInt() : def;
        };
        st.setValue(p + "idtype", cdata(COL_IDTYPE, 0));
        st.setValue(p + "len",    cdata(COL_LEN, 8));
        st.setValue(p + "ftype",  cdata(COL_FRAMETYPE, (int)FT_CLASSIC));
    }
    st.setValue("edit/id",          m_idEdit->text());
    st.setValue("edit/idtype",      m_idTypeBox->currentData().toInt());
    st.setValue("edit/len",         m_lenBox->currentData().toInt());
    st.setValue("edit/ftype",       m_frameTypeBox->currentData().toInt());
    st.setValue("edit/data",        m_dataEdit->text());
    st.setValue("can/bitrateIndex",    m_bitrateBox->currentIndex());
    st.setValue("can/dataBitrateIndex", m_dataBitrateBox->currentIndex());
    st.setValue("can/loop",         m_loopBox->isChecked());
    st.setValue("can/interval",     m_intervalSpin->value());
    st.setValue("ui/transport",     m_transportBox->currentIndex());
    st.setValue("ui/port",          m_portBox->currentData().toString());
    st.setValue("rx/scroll",        m_scrollMode);
    st.setValue("rx/timestamp",     m_tsBox->isChecked());
    // 工作模式（CAN/RS485）与协议页（CyberBeast/Simple/Open/通用CAN）
    st.setValue("ui/workMode", (int)(m_modeStack ? m_modeStack->currentIndex() : 0));
    st.setValue("ui/protocol", (int)(m_protoStack ? m_protoStack->currentIndex() : 3));
    // Simple 协议页数据
    if (m_simpleNodeEdit) st.setValue("simple/node", m_simpleNodeEdit->text());
    if (m_simpleGearEdit) st.setValue("simple/gear", m_simpleGearEdit->text());
    if (m_simpleCmdBox)   st.setValue("simple/cmd",  m_simpleCmdBox->currentData().toInt());
    st.setValue("simple/nparam", m_simpleParamEdits.size());
    for (int i = 0; i < m_simpleParamEdits.size(); ++i)
        st.setValue(QStringLiteral("simple/param%1").arg(i),
                    m_simpleParamEdits.at(i) ? m_simpleParamEdits.at(i)->text() : QString());
    st.setValue("simple/example", curSimpleExample());   // 实战示例（-1=普通命令）
    for (int i = 0; i < m_simpleUnitBoxes.size(); ++i)
        if (m_simpleUnitBoxes.at(i))
            st.setValue(QStringLiteral("simple/unit%1").arg(i),
                        m_simpleUnitBoxes.at(i)->currentIndex());
    // RxSdo 专属字段
    if (m_sdoOpcodeBox)   st.setValue("simple/sdoOpcode",   m_sdoOpcodeBox->currentData().toInt());
    if (m_sdoEndpointEdit) st.setValue("simple/sdoEndpoint", m_sdoEndpointEdit->text());
    if (m_sdoTypeBox)     st.setValue("simple/sdoType",     m_sdoTypeBox->currentData().toInt());
    if (m_sdoValueEdit)   st.setValue("simple/sdoValue",    m_sdoValueEdit->text());
    // Open（CANopen）协议页
    if (m_coNodeEdit) st.setValue("co/node", m_coNodeEdit->text());
    if (m_coGearEdit) st.setValue("co/gear", m_coGearEdit->text());
    {
        st.setValue("co/exN", m_coExValEdits.size());
        for (int k = 0; k < m_coExValEdits.size(); ++k) {
            st.setValue(QStringLiteral("co/exVal%1").arg(k),
                        m_coExValEdits.at(k) ? m_coExValEdits.at(k)->text() : QString());
            QComboBox *ub = (k < m_coExUnitBoxes.size()) ? m_coExUnitBoxes.at(k) : nullptr;
            st.setValue(QStringLiteral("co/exUnit%1").arg(k), ub ? ub->currentIndex() : -1);
        }
    }
    if (m_coFuncBox)  st.setValue("co/func", m_coFuncBox->currentData(Qt::UserRole).toInt());
    st.setValue("co/example", curCoExample());
    if (m_coNmtCmdBox)    st.setValue("co/nmt", m_coNmtCmdBox->currentData().toInt());
    if (m_coSdoOpBox)     st.setValue("co/sdoOp", m_coSdoOpBox->currentData().toInt());
    if (m_coSdoIdxBox)    st.setValue("co/sdoIdx", m_coSdoIdxBox->currentData().toUInt());
    if (m_coSdoValueEdit) st.setValue("co/sdoValue", m_coSdoValueEdit->text());
    // PDO 行（用户可配置的映射关系）：持久化
    {
        coSnapshotPdoRows();   // 先从控件快照，保存用户当前映射
        st.setValue("co/pdoRows", m_coPdoRows.size());
        for (int i = 0; i < m_coPdoRows.size(); ++i) {
            st.setValue(QStringLiteral("co/pdoIdx%1").arg(i), m_coPdoRows.at(i).index);
            st.setValue(QStringLiteral("co/pdoSub%1").arg(i), m_coPdoRows.at(i).sub);
            st.setValue(QStringLiteral("co/pdoVal%1").arg(i), m_coPdoRows.at(i).value);
        }
    }
    // PDO 映射修改示例：不持久化（关闭后恢复默认；会话内由 m_coMapCache 记忆）
    if (m_coHbStateBox)   st.setValue("co/hbState", m_coHbStateBox->currentData().toInt());
    // 命令列表：持久化
    st.setValue("co/listN", m_coListModel.size());
    for (int i = 0; i < m_coListModel.size(); ++i) {
        st.setValue(QStringLiteral("co/listLabel%1").arg(i), m_coListModel.at(i).label);
        st.setValue(QStringLiteral("co/listFunc%1").arg(i), m_coListModel.at(i).func);
        st.setValue(QStringLiteral("co/listEx%1").arg(i), m_coListModel.at(i).example);
        st.setValue(QStringLiteral("co/listPvN%1").arg(i), m_coListModel.at(i).pv.size());
        for (int k = 0; k < m_coListModel.at(i).pv.size(); ++k)
            st.setValue(QStringLiteral("co/listPv%1_%2").arg(i).arg(k), m_coListModel.at(i).pv.at(k));
        st.setValue(QStringLiteral("co/listPuN%1").arg(i), m_coListModel.at(i).pu.size());
        for (int k = 0; k < m_coListModel.at(i).pu.size(); ++k)
            st.setValue(QStringLiteral("co/listPu%1_%2").arg(i).arg(k), m_coListModel.at(i).pu.at(k));
        st.setValue(QStringLiteral("co/listFrameN%1").arg(i), m_coListModel.at(i).frames.size());
        for (int j = 0; j < m_coListModel.at(i).frames.size(); ++j)
            st.setValue(QStringLiteral("co/listF%1_%2").arg(i).arg(j),
                        QString::fromLatin1(m_coListModel.at(i).frames.at(j).toHex()));
    }
    // RS485 数据区
    if (m_rs485DataEdit)  st.setValue("rs485/data", m_rs485DataEdit->text());
    // 接收区筛选框文本（各页）
    for (const RxFilterBar &b : m_rxFilters)
        for (int c = 0; c < b.edits.size(); ++c)
            st.setValue(QStringLiteral("rxfilter/%1/%2").arg(b.name).arg(c),
                        b.edits.at(c)->text());
    st.sync();
}

void MainWindow::loadSettings()
{
    QSettings st;
    // 发送表
    const int rows = st.value("table/rows", kDefaultRows).toInt();
    m_table->setRowCount(0);
    ensureRows(rows < 1 ? kDefaultRows : rows);
    for (int r = 0; r < m_table->rowCount(); r++) {
        const QString p = QStringLiteral("table/%1/").arg(r);
        auto *chk = m_table->item(r, COL_CHK);
        if (chk) chk->setCheckState(st.value(p + "check", false).toBool() ? Qt::Checked : Qt::Unchecked);
        auto *idIt = m_table->item(r, COL_ID);
        if (idIt) idIt->setText(st.value(p + "id", QString()).toString());
        auto *dIt = m_table->item(r, COL_DATA);
        if (dIt) dIt->setText(st.value(p + "data", QString()).toString());
        auto setCombo = [&](int col, const char *key, int def) {
            auto *w = qobject_cast<QComboBox *>(m_table->cellWidget(r, col));
            if (!w) return;
            int idx = w->findData(st.value(p + key, def).toInt());
            if (idx >= 0) w->setCurrentIndex(idx);
        };
        setCombo(COL_IDTYPE,    "idtype", 0);
        setCombo(COL_LEN,       "len",    8);
        setCombo(COL_FRAMETYPE, "ftype",  (int)FT_CLASSIC);
    }
    // 单帧编辑区
    m_idEdit->setText(st.value("edit/id", QString()).toString());
    m_dataEdit->setText(st.value("edit/data", QString()).toString());
    auto setBox = [&](QComboBox *w, const char *key, int def) {
        int idx = w->findData(st.value(key, def).toInt());
        if (idx >= 0) w->setCurrentIndex(idx);
    };
    setBox(m_idTypeBox,    "edit/idtype", 0);
    setBox(m_lenBox,       "edit/len",    8);
    setBox(m_frameTypeBox, "edit/ftype",  (int)FT_CLASSIC);
    // CAN 参数
    {
        int idx = st.value("can/bitrateIndex", 9).toInt();
        if (idx >= 0 && idx < m_bitrateBox->count()) {
            QSignalBlocker b(m_bitrateBox);           // 连接前避免误下发
            m_bitrateBox->setCurrentIndex(idx);
        }
        // 数据域波特率独立持久化（默认 5M = index 1）
        int didx = st.value("can/dataBitrateIndex", 1).toInt();
        if (didx >= 0 && didx < m_dataBitrateBox->count()) {
            QSignalBlocker b(m_dataBitrateBox);
            m_dataBitrateBox->setCurrentIndex(didx);
        }
    }
    m_loopBox->setChecked(st.value("can/loop", false).toBool());
    m_intervalSpin->setValue(st.value("can/interval", 100).toInt());
    // 传输 / 端口
    {
        int ti = st.value("ui/transport", 0).toInt();
        if (ti >= 0 && ti < m_transportBox->count()) {
            QSignalBlocker b(m_transportBox);
            m_transportBox->setCurrentIndex(ti);
        }
        refreshPorts();
        const QString want = st.value("ui/port", QString()).toString();
        if (!want.isEmpty()) {
            int idx = m_portBox->findData(want);
            if (idx >= 0) m_portBox->setCurrentIndex(idx);
        }
        syncConnUi();          // 端口选择广播到所有页面的连接区
    }
    if (m_rs485DataEdit)
        m_rs485DataEdit->setText(st.value("rs485/data", QString()).toString());
    // 接收区选项
    setRxMode(st.value("rx/scroll", false).toBool());
    m_tsBox->setChecked(st.value("rx/timestamp", false).toBool());

    // 工作模式（CAN/RS485）
    {
        const int wm = st.value("ui/workMode", 0).toInt();
        if (m_modeStack)
            m_modeStack->setCurrentWidget(wm == 1 ? m_pageRs485 : m_pageCan);
        if (wm == 1 && m_actionModeRs485) m_actionModeRs485->setChecked(true);
        else if (m_actionModeCan)         m_actionModeCan->setChecked(true);
    }
    // 协议页（CyberBeast/Simple/Open/通用CAN）
    {
        const int pr = st.value("ui/protocol", 3).toInt();
        if (m_protoStack && pr >= 0 && pr < m_protoStack->count())
            m_protoStack->setCurrentIndex(pr);
        QAction *pa[4] = { m_actionProtoCyberBeast, m_actionProtoSimple,
                           m_actionProtoOpen, m_actionProtoGeneric };
        if (pr >= 0 && pr < 4 && pa[pr]) pa[pr]->setChecked(true);
    }
    // Simple 协议页数据
    {
        if (m_simpleNodeEdit) m_simpleNodeEdit->setText(st.value("simple/node", QStringLiteral("1")).toString());
        if (m_simpleGearEdit) m_simpleGearEdit->setText(st.value("simple/gear", QStringLiteral("1")).toString());
        if (m_simpleCmdBox) {
            int rowSel = -1;
            const int exIdx = st.value("simple/example", -1).toInt();   // 优先恢复实战示例
            if (exIdx >= 0) {
                for (int r = 0; r < m_simpleCmdBox->count(); ++r) {
                    const QVariant v = m_simpleCmdBox->itemData(r, Qt::UserRole + 1);
                    if (v.isValid() && v.toInt() == exIdx) { rowSel = r; break; }
                }
            }
            if (rowSel < 0) rowSel = m_simpleCmdBox->findData(st.value("simple/cmd", 0x0C).toInt());
            if (rowSel >= 0) m_simpleCmdBox->setCurrentIndex(rowSel);
            rebuildSimpleParams();                 // 确保按当前命令重建参数框
        }
        const int np = st.value("simple/nparam", 0).toInt();
        for (int i = 0; i < np && i < m_simpleParamEdits.size(); ++i)
            if (m_simpleParamEdits.at(i))
                m_simpleParamEdits.at(i)->setText(
                    st.value(QStringLiteral("simple/param%1").arg(i), QStringLiteral("0")).toString());
        for (int i = 0; i < m_simpleUnitBoxes.size(); ++i) {   // 参数单位
            QComboBox *ub = m_simpleUnitBoxes.at(i);
            if (!ub) continue;
            const int ui = st.value(QStringLiteral("simple/unit%1").arg(i), -1).toInt();
            if (ui >= 0 && ui < ub->count()) ub->setCurrentIndex(ui);
        }
        // RxSdo 专属字段（仅当当前命令为 0x04，控件才存在）
        if (m_sdoOpcodeBox) {
            const int oi = m_sdoOpcodeBox->findData(st.value("simple/sdoOpcode", 0).toInt());
            if (oi >= 0) m_sdoOpcodeBox->setCurrentIndex(oi);
        }
        if (m_sdoEndpointEdit)
            m_sdoEndpointEdit->setText(st.value("simple/sdoEndpoint", QStringLiteral("0")).toString());
        if (m_sdoTypeBox) {
            const int ti = m_sdoTypeBox->findData(st.value("simple/sdoType", simple::SDO_FLOAT).toInt());
            if (ti >= 0) m_sdoTypeBox->setCurrentIndex(ti);
        }
        if (m_sdoValueEdit)
            m_sdoValueEdit->setText(st.value("simple/sdoValue", QStringLiteral("0")).toString());
        updateSimplePreview();
    }
    // Open（CANopen）协议页
    {
        if (m_coNodeEdit) m_coNodeEdit->setText(st.value("co/node", QStringLiteral("1")).toString());
        if (m_coGearEdit) m_coGearEdit->setText(st.value("co/gear", QStringLiteral("1")).toString());
        if (m_coFuncBox) {
            int rowSel = -1;
            const int exIdx = st.value("co/example", -1).toInt();
            if (exIdx >= 0) {
                for (int r = 0; r < m_coFuncBox->count(); ++r) {
                    const QVariant v = m_coFuncBox->itemData(r, Qt::UserRole + 1);
                    if (v.isValid() && v.toInt() == exIdx) { rowSel = r; break; }
                }
            }
            if (rowSel < 0)
                rowSel = m_coFuncBox->findData(st.value("co/func", (int)canopen::F_NMT).toInt(), Qt::UserRole);
            if (rowSel >= 0) m_coFuncBox->setCurrentIndex(rowSel);
            const int lfn = m_coFuncBox->currentData(Qt::UserRole).toInt();
            // PDO 行（用户自定义映射）：在重建前恢复；保存过则为用户映射，否则用 EDS 默认
            if (canopen::pdoMappingObjectIndex((canopen::Function)lfn) >= 0) {
                const int n = st.value("co/pdoRows", -1).toInt();
                if (n >= 0) {
                    m_coPdoRows.clear();
                    for (int i = 0; i < n; ++i) {
                        CoPdoRow r;
                        r.index = st.value(QStringLiteral("co/pdoIdx%1").arg(i), 0x6071).toInt();
                        r.sub   = st.value(QStringLiteral("co/pdoSub%1").arg(i), 0).toInt();
                        r.value = st.value(QStringLiteral("co/pdoVal%1").arg(i), QStringLiteral("0")).toString();
                        m_coPdoRows.append(r);
                    }
                }
                m_coPdoFunc = lfn;   // 阻止用 EDS 默认覆盖已恢复的用户映射
            }
            rebuildCoParams();
        }
        if (m_coNmtCmdBox) { int i = m_coNmtCmdBox->findData(st.value("co/nmt", 0x01).toInt()); if (i >= 0) m_coNmtCmdBox->setCurrentIndex(i); }
        if (m_coSdoOpBox)  { int i = m_coSdoOpBox->findData(st.value("co/sdoOp", 0).toInt()); if (i >= 0) m_coSdoOpBox->setCurrentIndex(i); }
        if (m_coSdoIdxBox) {
            const uint want = st.value("co/sdoIdx", (uint)(0x6040u << 8)).toUInt();
            int idxRow = m_coSdoIdxBox->findData(want);
            if (idxRow >= 0) m_coSdoIdxBox->setCurrentIndex(idxRow);
        }
        if (m_coSdoValueEdit) m_coSdoValueEdit->setText(st.value("co/sdoValue", QStringLiteral("0")).toString());
        // PDO 映射不恢复：进入 PDO 功能时按手册/EDS 默认映射重建
        if (m_coHbStateBox)   { int i = m_coHbStateBox->findData(st.value("co/hbState", 0x05).toInt()); if (i >= 0) m_coHbStateBox->setCurrentIndex(i); }
        coLoadCoList();   // 恢复命令列表
        // PDO 映射修改示例：不恢复（每次打开用所选 PDO 的当前映射）
        // 实战示例用户输入（位置/速度/力矩）
        for (int k = 0; k < m_coExValEdits.size(); ++k)
            if (m_coExValEdits.at(k))
                m_coExValEdits.at(k)->setText(
                    st.value(QStringLiteral("co/exVal%1").arg(k), m_coExValEdits.at(k)->text()).toString());
        for (int k = 0; k < m_coExUnitBoxes.size(); ++k) {
            QComboBox *ub = m_coExUnitBoxes.at(k);
            if (!ub) continue;
            const int ui = st.value(QStringLiteral("co/exUnit%1").arg(k), -1).toInt();
            if (ui >= 0 && ui < ub->count()) ub->setCurrentIndex(ui);
        }
        updateCoPreview();
    }
    // 接收区筛选框文本（各页）
    for (const RxFilterBar &b : m_rxFilters)
        for (int c = 0; c < b.edits.size(); ++c) {
            const QString v = st.value(
                QStringLiteral("rxfilter/%1/%2").arg(b.name).arg(c), QString()).toString();
            if (!v.isEmpty()) { b.edits.at(c)->setText(v); applyRxFilter(b.table); }
        }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    saveSettings();
    QMainWindow::closeEvent(e);
}
