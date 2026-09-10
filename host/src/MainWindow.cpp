#include "MainWindow.h"
#include "SerialLink.h"
#include "UsbLink.h"
#include "FirmwareDialog.h"
#include "dfu_device.h"

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
    {"5 Mbit/s",    5000000},
};

// 帧类型编码（用于下拉 data）
enum FrameType { FT_CLASSIC = 0, FT_FD = 1, FT_FD_BRS = 2 };

// 发送表列：行号 | 选择 | 帧ID | 标准/扩展 | 发送字节长度 | 帧类型 | 数据
enum Col { COL_NO = 0, COL_CHK, COL_ID, COL_IDTYPE, COL_LEN, COL_FRAMETYPE, COL_DATA, COL_COUNT };

// 接收表列：帧ID | 类型 | 长度 | 数据 | 次数
enum RxCol { RX_ID = 0, RX_TYPE, RX_LEN, RX_DATA, RX_COUNT, RX_COL_COUNT };

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
    setWindowTitle(QStringLiteral("CANable2-N32 上位机"));

    auto *central = new QWidget(this);
    auto *root = new QHBoxLayout(central);       // 左右布局

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
    }

    // ======================= 左列：发送 =======================
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);

    // ---------------- 连接行 ----------------
    auto *serialBox = new QGroupBox(QStringLiteral("连接"));
    auto *sh = new QHBoxLayout(serialBox);
    sh->addWidget(new QLabel(QStringLiteral("传输:")));
    m_transportBox = new QComboBox;
    m_transportBox->setObjectName("transportBox");
    m_transportBox->addItem(QStringLiteral("串口(CDC)"), 0);
    m_transportBox->addItem(QStringLiteral("USB(libusb)"), 1);
    sh->addWidget(m_transportBox);
    sh->addWidget(new QLabel(QStringLiteral("端口:")));
    m_portBox = new QComboBox;
    m_portBox->setObjectName("portBox");
    m_portBox->setMinimumWidth(200);
    sh->addWidget(m_portBox);
    m_refreshBtn = new QPushButton(QStringLiteral("刷新"));
    sh->addWidget(m_refreshBtn);
    m_openBtn = new QPushButton(QStringLiteral("打开"));
    sh->addWidget(m_openBtn);
    m_updateFwBtn = new QPushButton(QStringLiteral("更新固件"));
    m_updateFwBtn->setObjectName("updateFwBtn");
    sh->addWidget(m_updateFwBtn);
    sh->addStretch();
    lv->addWidget(serialBox);

    // ---------------- CAN 参数行 ----------------
    auto *paramBox = new QGroupBox(QStringLiteral("CAN 参数"));
    auto *ph = new QHBoxLayout(paramBox);
    ph->addWidget(new QLabel(QStringLiteral("波特率:")));
    m_bitrateBox = new QComboBox;
    for (const auto &b : kBauds)
        m_bitrateBox->addItem(QString::fromUtf8(b.name), b.hz);
    m_bitrateBox->setCurrentIndex(9);             // 默认 1 Mbit/s
    ph->addWidget(m_bitrateBox);
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
    m_dataEdit = new QLineEdit;                     // 默认空
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
    resetTable();                 // 默认 20 行空表格
    llv->addWidget(m_table);

    auto *lb = new QHBoxLayout;
    m_resetBtn = new QPushButton(QStringLiteral("重置列表"));
    m_resetBtn->setObjectName("resetBtn");
    m_exportBtn = new QPushButton(QStringLiteral("导出CSV"));
    m_exportBtn->setObjectName("exportBtn");
    auto *hint = new QLabel(QStringLiteral("行内直接填写；勾选√决定发送行"));
    hint->setStyleSheet("color:#666;");
    lb->addWidget(m_resetBtn);
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

    // ---------------- 日志 ----------------
    auto *logBox = new QGroupBox(QStringLiteral("日志"));
    auto *lg = new QVBoxLayout(logBox);
    auto *lgb = new QHBoxLayout;
    lgb->addStretch();
    m_clearLogBtn = new QPushButton(QStringLiteral("清空日志"));
    m_clearLogBtn->setObjectName("clearLogBtn");
    lgb->addWidget(m_clearLogBtn);
    lg->addLayout(lgb);
    m_log = new QPlainTextEdit;
    m_log->setObjectName("log");
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    lg->addWidget(m_log);
    lv->addWidget(logBox, 1);

    root->addWidget(left, 3);

    // ======================= 右列：接收 =======================
    auto *rxBox = new QGroupBox(QStringLiteral("接收区 (相同帧ID就地更新，不滚动)"));
    auto *rv = new QVBoxLayout(rxBox);

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
    rv->addWidget(m_rxTable, 1);

    auto *rb = new QHBoxLayout;
    m_clearRxBtn = new QPushButton(QStringLiteral("清空接收"));
    m_clearRxBtn->setObjectName("clearRxBtn");
    rb->addWidget(m_clearRxBtn);
    rb->addStretch();
    rv->addLayout(rb);

    root->addWidget(rxBox, 2);

    m_statusLbl = new QLabel(QStringLiteral("未连接"));
    statusBar()->addWidget(m_statusLbl);

    setCentralWidget(central);
    resize(1520, 880);

    // ---------------- 信号连接 ----------------
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(m_openBtn,    &QPushButton::clicked, this, &MainWindow::togglePort);
    connect(m_transportBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                const bool usb = (m_transportBox->currentData().toInt() == 1);
                m_portBox->setEnabled(true);        // 两种传输下都可选择设备
                m_refreshBtn->setEnabled(true);
                // 每次切换串口<->USB 自动刷新设备列表
                refreshPorts();
                logInfo(usb ? QStringLiteral("传输方式：USB(libusb)")
                            : QStringLiteral("传输方式：串口(CDC)"));
            });
    connect(m_updateFwBtn,&QPushButton::clicked, this, &MainWindow::onUpdateFirmware);
    connect(m_bitrateBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBitrateChanged);
    connect(m_intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onIntervalChanged);
    connect(m_resetBtn, &QPushButton::clicked, this, &MainWindow::onResetList);
    connect(m_exportBtn, &QPushButton::clicked, this, &MainWindow::onExportCsv);
    connect(m_clearRxBtn, &QPushButton::clicked, this, &MainWindow::onClearRx);
    connect(m_clearLogBtn, &QPushButton::clicked, this, &MainWindow::onClearLog);
    connect(m_sendBtn,  &QPushButton::clicked, this, &MainWindow::onSend);
    connect(m_stopBtn,  &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_statusBtn,&QPushButton::clicked, this, &MainWindow::onQueryStatus);

    refreshPorts();
    setPortUiEnabled(false);
    logInfo(QStringLiteral("就绪。勾选列表行数决定单帧/列表发送；仅数据变化时才下发数据帧。"));
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
void MainWindow::refreshPorts()
{
    m_portBox->clear();
    if (m_transportBox->currentData().toInt() == 1) {
        // USB(libusb)：列出全部可用 libusb 设备
        const auto devs = UsbLink::scanDevices();
        for (const UsbDeviceInfo &d : devs)
            m_portBox->addItem(d.label(), d.id());
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
            m_portBox->addItem(label, p.portName());
        }
        if (ports.isEmpty())
            logInfo(QStringLiteral("未发现可用串口。"));
        else
            logInfo(QStringLiteral("发现 %1 个串口。").arg(ports.size()));
    }
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
    m_statusLbl->setText(QStringLiteral("已连接(%1): %2").arg(m_link->kind(), name));

    // 连接后自动下发当前 CAN 波特率 + 发送模式（打包为一条消息）
    queueCommand(proto::CMD_SET_BITRATE,
                 (quint32)m_bitrateBox->currentData().toUInt(),
                 QStringLiteral("设置波特率 %1").arg(m_bitrateBox->currentText()));
    queueCommand(proto::CMD_SET_MODE, proto::MODE_SINGLE,
                 QStringLiteral("发送模式 单帧"));
    m_lastMode = (int)proto::MODE_SINGLE;
    flushTx(QStringLiteral("连接初始化"));
    logInfo(QStringLiteral("已连接(%1) %2").arg(m_link->kind(), name));
}

void MainWindow::onPortClosed()
{
    setPortUiEnabled(false);
    m_openBtn->setText(QStringLiteral("打开"));
    m_statusLbl->setText(QStringLiteral("未连接"));
    logInfo(QStringLiteral("已断开。"));
}

void MainWindow::onSerialError(const QString &msg)
{
    logInfo(QStringLiteral("[错误] %1").arg(msg));
}

void MainWindow::setPortUiEnabled(bool open)
{
    m_bitrateBox->setEnabled(open);
    m_intervalSpin->setEnabled(open);
    m_sendBtn->setEnabled(open);
    m_stopBtn->setEnabled(open);
    m_statusBtn->setEnabled(open);
    m_resetBtn->setEnabled(true);   // 列表可离线编辑
    // 连接期间锁定传输/设备选择，避免误切
    m_transportBox->setEnabled(!open);
    m_portBox->setEnabled(!open);
    m_refreshBtn->setEnabled(!open);
}

// ---------------------------------------------------------------------------
void MainWindow::onBitrateChanged(int)
{
    if (!m_link->isOpen())
        return;
    sendCommand(proto::CMD_SET_BITRATE,
                (quint32)m_bitrateBox->currentData().toUInt(),
                QStringLiteral("设置波特率 %1").arg(m_bitrateBox->currentText()));
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

void MainWindow::onExportCsv()
{
    const QString csv = buildExportCsv();
    int rows = csv.count(QStringLiteral("\r\n")) - 1;   // 去掉表头
    if (rows <= 0) {
        logInfo(QStringLiteral("没有已勾选且已填写 ID 的记录可导出。"));
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
        logInfo(QStringLiteral("导出失败：无法写入 %1").arg(path));
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
    m_rxTable->setRowCount(0);
    m_rxRowById.clear();
}

void MainWindow::onClearLog()
{
    m_log->clear();
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

void MainWindow::onSend()
{
    if (!m_link->isOpen()) {
        logInfo(QStringLiteral("串口未打开。"));
        return;
    }
    doSend();
}

QString MainWindow::doSend()
{
    QString info;
    Payload p = collectPayload(&info);

    if (p.empty()) {
        logInfo(QStringLiteral("无可发送数据（编辑区为空且未勾选任何行）。"));
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

    if (changed) {
        for (const QByteArray &f : p.frames) {
            queueBytes(f, p.isList ? QStringLiteral("列表帧") : QStringLiteral("单帧"));
        }
        m_lastInit = true;
        m_lastIsList = p.isList;
        m_lastFrames = p.frames;
        logInfo(QStringLiteral("%1；数据已变化，已打包下发。").arg(info));
    } else {
        logInfo(QStringLiteral("%1；数据未变，仅下发发送命令。").arg(info));
    }

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
    logTx(bytes, note);
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
    logInfo(QStringLiteral("%1：单次写入 %2 字节")
                .arg(note).arg(m_txBuf.size()));
    m_txBuf.clear();
}

void MainWindow::sendCommand(proto::Cmd cmd, quint32 value, const QString &note)
{
    queueCommand(cmd, value, note);
    flushTx(note);
}

void MainWindow::logTx(const QByteArray &bytes, const QString &note)
{
    m_log->appendPlainText(QStringLiteral("[%1] TX  %2  %3")
        .arg(QTime::currentTime().toString("HH:mm:ss.zzz"),
             QString::fromLatin1(bytes.toHex(' ').toUpper()), note));
}

void MainWindow::logRx(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("[%1] RX  %2")
        .arg(QTime::currentTime().toString("HH:mm:ss.zzz"), text));
}

void MainWindow::logInfo(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("[%1] --  %2")
        .arg(QTime::currentTime().toString("HH:mm:ss.zzz"), text));
}

void MainWindow::onFrameReceived(const proto::RxFrame &f)
{
    // 日志记录
    logRx(QStringLiteral("CAN id=0x%1 %2%3%4 dlc=%5 data=[%6]")
        .arg(f.id, f.ext ? 8 : 3, 16, QLatin1Char('0'))
        .arg(f.ext ? "EXT " : "")
        .arg(f.fd ? "FD " : "")
        .arg(f.brs ? "BRS " : "")
        .arg(f.dlc)
        .arg(QString::fromLatin1(f.data.toHex(' ').toUpper())));
    // 接收区就地更新
    feedRx(f);
}

void MainWindow::onResponseReceived(const proto::RxResponse &r)
{
    if (r.id == proto::RSP_BITRATE_LOCK) {
        logRx(QStringLiteral("AA AC 波特率锁定 = %1 Hz").arg(r.value));
        for (int i = 0; i < m_bitrateBox->count(); i++) {
            if ((quint32)m_bitrateBox->itemData(i).toUInt() == r.value) {
                QSignalBlocker b(m_bitrateBox);
                m_bitrateBox->setCurrentIndex(i);
                break;
            }
        }
    } else {
        logRx(QStringLiteral("AA AC 响应 id=%1 value=%2").arg(r.id).arg(r.value));
    }
}

void MainWindow::onStatusLine(const QString &line)
{
    logRx(QStringLiteral("状态: %1").arg(line));
}

QString MainWindow::logText() const
{
    return m_log->toPlainText();
}
