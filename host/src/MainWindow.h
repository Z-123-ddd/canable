// ============================================================================
//  MainWindow — CANable2-N32 上位机主界面
//    控件约定：
//      · 波特率选择     -> 下拉框
//      · 循环发送       -> 复选框（决定 TX_START 触发 value）
//      · 列表/单帧模式  -> 两个显式按钮：「单条发送」取单帧编辑区 /「列表发送」取勾选行
//      · 布局：左=发送（编辑/列表/操作/日志），右=接收（按帧 id 就地更新）
// ============================================================================
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QVector>
#include <QHash>
#include <QThread>
#include <functional>
#include "Protocol.h"
#include "EsiXml.h"
#include "CommandClipboardDialog.h"

class SerialLink;
class EtherCatLink;
class JointController;
class UsbLink;
class Link;
class QComboBox;
class QPushButton;
class QCheckBox;
class QSpinBox;
class QLineEdit;
class QTableWidget;
class QPlainTextEdit;
class QLabel;
class QToolButton;
class QListWidget;
class QCloseEvent;
class QTimer;
class QStackedWidget;
class QGroupBox;
class QAction;
class QMenu;

// 「连接区」「日志区」在多个页面各嵌一份，但状态共享（都指向同一 USB 链路/同一份日志）
struct ConnPanel {
    QComboBox   *transport = nullptr;
    QComboBox   *port      = nullptr;
    QPushButton *refresh   = nullptr;
    QPushButton *open      = nullptr;
    QPushButton *updateFw  = nullptr;
};
struct LogPanel {
    QPushButton    *clear = nullptr;
    QPlainTextEdit *log   = nullptr;
};

// 接收区「按字段筛选」：一行输入框（每列一个），匹配当前可见表的各行。
struct RxFilterBar {
    QString              name;    // 持久化键名（如 can.rx）
    QWidget             *row = nullptr;   // 筛选行控件（随表显示/隐藏）
    QVector<QLineEdit *> edits;   // 每列一个输入框
    QTableWidget        *table = nullptr; // 目标表
};

// CANopen 命令列表一条：标签 + 待发送的帧序列
struct CoListEntry {
    QString           label;   // 显示标签
    QVector<QByteArray> frames; // 该条目的线上帧（数据帧字节）
    // 用于点击回填编辑器的源描述
    int              func = -1;  // 功能编号（F_COUNT = 实战示例）
    int              example = -1; // 实战示例索引（-1=非示例）
    QVector<double>  pv;         // 参数值（按功能语义）
    QVector<int>     pu;         // 参数单位（示例）
};

// CANopen PDO 一行：EDS 对象（索引/子索引）+ 用户数值
struct CoPdoRow {    int     index = 0x6071;   // 对象索引
    int     sub   = 0;        // 子索引
    QString value = QStringLiteral("0");
};

// 在独立线程执行一次阻塞的 EtherCAT 操作（状态切换 / 伺服使能准备），
// 避免 SOEM 的同步调用阻塞 GUI 线程导致界面卡死。结果经 jobDone 信号回主线程。
class EcJob : public QThread
{
    Q_OBJECT
public:
    explicit EcJob(QObject *parent = nullptr) : QThread(parent) {}
    void runJob(std::function<QPair<bool, QString>()> fn) { m_fn = std::move(fn); start(); }
signals:
    void jobDone(bool ok, const QString &msg);
protected:
    void run() override {
        bool ok = false; QString msg;
        if (m_fn) { const QPair<bool, QString> r = m_fn(); ok = r.first; msg = r.second; }
        emit jobDone(ok, msg);
    }
private:
    std::function<QPair<bool, QString>()> m_fn;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 单条发送（取单帧编辑区）；返回本次动作摘要；供 UI 与无头自测复用
    QString doSendSingle();
    // 列表发送（取发送列表中勾选的行）；返回本次动作摘要；供 UI 与无头自测复用
    QString doSendList();
    // 当前日志文本（供无头自测读取）
    QString logText() const;
    // 串口写入次数 / 最近一次写入的字节（供无头自测校验“打包一次发送”）
    int txFlushCount() const { return m_txFlushCount; }
    QByteArray lastTxFlush() const { return m_lastFlush; }

    // 接收区：把一帧写入接收表（按 id 就地更新，不滚动）；供 UI 与无头自测调用
    void feedRx(const proto::RxFrame &f);
    void feedRxCounted(const proto::RxFrame &f, quint32 hits);
    // 供无头自测：模拟收到一帧（走完整 onFrameReceived 流程：接收表+解析区）
    void simulateRx(const proto::RxFrame &f);
    void stopRxFlushForTest();              // 仅无头压测用：停掉批量刷新定时器
    int  rxRowCount() const;
    QString rxCellText(int row, int col) const;
    // 接收区：滚动视图文本（供无头自测读取）
    QString scrollText() const;
    // CANopen 页接收区（供无头自测：滚动模式下的取值与模式切换）
    void coSetRxModeForTest(bool scroll) { coSetRxMode(scroll); }
    void coFeedRxForTest(const proto::RxFrame &f) { coFeedRx(f); }
    QString coScrollCellText(int row, int col) const;
    // 持久化（供无头自测复用；正常由 closeEvent 自动调用）
    void coSyncTransmitFromMapping();   // 执行映射修改后：同步到 PDO 传输区（会话记忆；供自测调用）
    void saveSettings();
    void loadSettings();

    // 导出已勾选记录为 CSV 文本（不含写文件）；供 UI 与无头自测校验
    QString buildExportCsv() const;
    QVector<ClipRecord> rxTableRecords(QTableWidget *t) const;   // 接收表 → 记录（供导出/自测）
    // 固件版本比对：设备上报文本 vs 内置最新固件版本(APP 内置 FW_VERSION)，返回状态描述
    static QString firmwareStatusNote(const QString &devText);
    // 单条发送（CANopen）取用的帧：始终取参数框中当前配置的命令，与命令列表无关
    QVector<QByteArray> coSingleCommandFrames(QString *what = nullptr) const;
    // 统一的发送缓存（模式未变不发 SET_MODE，数据未变不发数据帧，总是发 TX_START）；
    // 供各页面与自测调用。
    void sendCachedFrames(bool listMode, const QVector<QByteArray> &frames,
                          quint32 trig, int intervalMs = -1);
    void flushTxNow();   // 立即写出队列（供自测）

private slots:
    void refreshPorts();
    void togglePort();
    void onPortOpened(const QString &name);
    void onPortClosed();
    void onSerialError(const QString &msg);

    void onFrameReceived(const proto::RxFrame &f);
    void onResponseReceived(const proto::RxResponse &r);
    void onStatusLine(const QString &line);
    void onLogMessage(const proto::LogMessage &m);   // AA AD 警告/报错

    void onBitrateChanged(int idx);
    void onDataBitrateChanged(int idx);  // 数据域波特率下拉变化
    void onIntervalChanged(int ms);

    void onResetList();     // 重置为 20 行空表格（默认值）
    void onExportCsv();     // 导出已勾选记录为 CSV
    void onExportListRecords();   // 通用发送列表 → 弹出剪切栏窗口（复制/导出）
    void coExportListRecords();   // CANopen 命令列表 → 弹出剪切栏窗口（复制/导出）
    void onImportCsv();     // 导入其他 CAN 工具的记录文件到发送列表
    void onUpdateFirmware();// 进入 DFU 并打开固件更新对话框
    void onClearRx();       // 清空接收区
    void onClearLog();      // 清空日志
    void onSendSingle();    // 单条发送（取单帧编辑区）
    void onSendList();      // 列表发送（取勾选行）
    void onStop();
    void onQueryStatus();
    void onClearErrors();               // 清除固件内部错误寄存器（命令 12）
    // 帮助：关于（软件版本 + 设备固件版本）
    void onAbout();

    // ---- 接收暂停（冻结接收 + 清未完成帧）----
    void onTogglePause();               // 暂停/恢复接收（下发命令 11 + 冻结本机接收）
    // ---- CyberBeast 协议页（按类别下拉，仿 simple/open）----
    void onCbCatChanged(int idx);       // 类别切换 → 重建命令下拉
    void onCbCmdChanged(int idx);       // 命令切换 → 重建参数输入
    void onCbSend();
    void onCbStop();

    // Simple 协议页
    void onSimpleCmdChanged(int idx);   // 命令下拉变化 → 重建参数输入
    void onSimpleSend();                // 发送（数据变则打包，不变只发 TX_START）
    void onSimpleStop();                // 停止发送

    // Open（CANopen）协议页
    void onCoSend();                    // 下方「发送」：只发送命令列表（全部）；实战示例=序列
    void onCoSendSingle();              // 上方「单条发送」：只发参数框中当前配置的单条命令（与列表无关）
    void onCoStop();                    // 停止发送
    void onOpenObjectDictionary();      // 打开对象字典浏览器（EDS）
    // Open 页命令列表（可追加、按顺序发送、调换顺序）
    void coAddCurrentToList();          // 把当前编辑器命令追加到列表
    void coRebuildListWidget();         // 按模型重建 QListWidget（含 ↑/↓/×）
    void coMoveListRow(int row, int delta);
    void coRemoveListRow(int row);
    void coSnapshotCoList();            // 从控件同步模型（拖动后）
    void coRestoreFromEntry(int row);   // 点击列表项：回填编辑器（供再次修改）
    void coRestoreScrollPos(int pos);   // 布局重算后恢复命令列表滚动位置
    void coEditEntryBytes(int row);     // 双击列表项：手动编辑该命令的字节内容
    void coMapAddRow();                 // PDO 映射修改：追加一个目标对象
    void coMapRemoveRow(int i);         // PDO 映射修改：删除一个目标对象
    // 接收区筛选（各页接收区表头上方，每列一个输入框；随当前可见表切换）
    QWidget *makeRxFilterRow(const QString &name, QTableWidget *table, const QStringList &labels);
    void     applyRxFilterIdx(int idx);
    void     applyRxFilter(QTableWidget *table);   // 按表指针筛选
    QWidget *filterRowFor(QTableWidget *table);    // 取某表的筛选行（显示/隐藏用）
    void coLoadCoList();                // 从 QSettings 恢复列表
    void onImportEds();                 // 导入外部 EDS 文件
    void onResetEds();                  // 恢复内置 EDS
    void coAddPdoRow();                 // PDO 追加一行对象
    void coRemovePdoRow(int i);         // PDO 移除第 i 行
    void coSnapshotPdoRows();           // 从控件收集当前 PDO 行


protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;   // 空格：发送列表当前行

private:
    // ---- 发送表格 ----
    static const int kDefaultRows = 20;
    void initRow(int row);          // 初始化/重置一行（行号+复选框+下拉默认值）
    void ensureRows(int n);         // 保证至少有 n 行
    void resetTable();              // 重置为 kDefaultRows 行空表格

    // 从单帧编辑区构建一帧（表头/数据），返回空表示无效
    QByteArray buildSingleFrame(QString *err = nullptr) const;
    // 把一行表格内容构建成一帧；返回空表示该行无效
    QByteArray buildRowFrame(int row, QString *err = nullptr) const;

    // 收集"单条发送"有效载荷：仅取单帧编辑区
    struct Payload {
        bool isList = false;          // true: 列表发送; false: 单帧
        QVector<QByteArray> frames;   // 要下发的数据帧（列表时为勾选行）
        bool empty() const { return frames.isEmpty(); }
    };
    Payload buildPayloadSingle(QString *info = nullptr) const;
    // 收集"列表发送"有效载荷：仅取发送列表中勾选的行（不勾选则空）
    Payload buildPayloadList(QString *info = nullptr) const;
    // 下发一份有效载荷（滚动区回显 + 缓存发送 + flush），返回动作摘要
    QString sendPayload(const Payload &p, const QString &info);

    // 仅发送表格第 row 行（单帧，忽略勾选）；供空格键使用
    void sendRowFrame(int row);

    void sendCommand(proto::Cmd cmd, quint32 value, const QString &note);
    // 打包：入队到 m_txBuf（仅记录），flushTx 一次性写出
    void queueBytes(const QByteArray &bytes, const QString &note);
    void queueCommand(proto::Cmd cmd, quint32 value, const QString &note);
    void flushTx(const QString &note);
    // 统一的发送缓存：模式未变不发 SET_MODE，数据未变不发数据帧，总是发 TX_START。
    // 固件的“当前发送模式/缓存数据”全局唯一，故所有页面共用此缓存（跨页面一致）。
    void logWarn(const QString &text);          // 写入日志区（警告）
    void logError(const QString &text);         // 写入日志区（报错）
    void logInfo(const QString &text);          // 状态提示（写状态栏，不进日志）
    void appendScroll(const QString &dir, const proto::RxFrame &f);  // 滚动视图追加一行（含表头）
    void setRxMode(bool scroll);                // 切换接收区显示模式并更新提示/图标
    void setPortUiEnabled(bool open);
    // ---- 多页面共享的「连接区/日志区」----
    QGroupBox   *buildConnGroup();      // 工厂：建一份连接区并注册（第 0 份沿用既有成员名）
    QGroupBox   *buildLogGroup();       // 工厂：建一份日志区并注册（第 0 份沿用既有成员名）
    void         syncConnUi();          // 把第 0 份（规范）的传输/端口/按钮状态广播到所有副本
    void         onTransportChanged(int panelIdx);
    void         onPortChanged(int panelIdx);
    void setWorkMode(int mode);     // 模式菜单：切页面（CAN=0/RS485=1/EtherCAT=2）
    // ---- EtherCAT（统一走厂商 joint-sdk；仅网卡枚举用 SOEM 静态接口）----
    void onEcRefreshAdapters();     // 扫描主机网卡
    void onEcSdoRead();             // 经 joint-sdk 的 SDO 读
    void onEcSdoWrite();            // 经 joint-sdk 的 SDO 写
    // ---- EtherCAT 后台任务（阻塞操作放工作线程，避免卡死）----
    void startEcJob(std::function<QPair<bool, QString>()> fn, const QString &busyLog);
    void onEcJobDone(bool ok, const QString &msg);
    void setEcBusy(bool busy);
    // ---- EtherCAT 对象字典（ESI XML）----
    void onEcImportXml();
    void onEcObjSelected(int idx);
    // ---- EtherCAT 关节（joint-sdk）----
    void onJointOpenConfig();     // 打开网卡 + 配置(SAFE_OP)
    void onJointActivate();       // 激活(OP)
    void onJointEnable();
    void onJointDisable();
    void onJointFaultReset();
    void onJointRunToggle();      // 启动/停止周期
    void jointCycle();
    // 从站 PDO 分配为空时，按已导入 ESI 写入 0x1C12/0x1C13（含映射项，除非 Fixed）；
    // 返回是否发生了写入。仅在工作线程调用（内部为阻塞 SDO）。
    bool ecConfigurePdoFromEsi(int slave, QString *log);
    // ---- Simple 协议页 ----
    void rebuildSimpleParams();                        // 按当前命令重建参数输入框
    void updateSimplePreview();                        // 刷新预览（CAN ID / 数据 hex）
    QByteArray buildSimpleFrame(QString *err) const;   // 生成 Simple 主命令线上帧（含 CAN ID）
    QVector<QByteArray> buildSimpleFrames(QString *err) const; // 单命令=1 帧；实战示例=固定前缀+主命令
    int curSimpleExample() const;                      // 当前选中的实战示例索引（-1=普通命令）
    // ---- Open（CANopen）协议页 ----
    void rebuildCoParams();                             // 按当前功能重建参数输入
    void updateCoPreview();                             // 刷新预览（COB-ID / DATA / 序列）
    void coUpdateSdoOpAvail();                          // 按 EDS 访问类型(ro/rw/wo)启用/禁用 读/写 选项
    int  curCoExample() const;                          // 当前选中示例索引（-1=普通功能）
    QVector<QByteArray> buildCoFrames(QString *err) const; // 普通功能=1 帧；示例=固定序列
    void coFeedRx(const proto::RxFrame &f);             // 接收：就地汇总 + 滚动
    void coAppendScroll(const QString &dir, const proto::RxFrame &f);
    void coSetRxMode(bool scroll);
    void simpleFeedRx(const proto::RxFrame &f);        // 接收：就地汇总 + 滚动
    void simpleAppendScroll(const QString &dir, const proto::RxFrame &f);
    void simpleSetRxMode(bool scroll);

    void setProtocol(int which);    // 协议菜单：切协议调试页（0=CyberBeast 1=Simple 2=Open）
    // ---- 接收“解析区”（4 个协议页接收区下方 1/3）----
    void registerParseArea(const QString &key, QTableWidget *tbl);
    void appendParse(const QString &key, const proto::RxFrame &f);   // 按 key 协议解析，按 ID 就地更新
    // CANopen 实战示例输入项：取值 / 单位索引 / 写回（枚举下拉回退到文本框）
    double coExValue(int k) const;
    int    coExUnit(int k) const;
    void   coExSet(int k, double v);
    void clearParseAreas();                                          // 清空全部解析区（所有协议页）
    void capScrollRows(QTableWidget *t);                             // 滚动接收区：超出 kRxCacheBytes(≈10MB) 丢弃最旧
    void capIndexedRows(QTableWidget *t, QHash<quint32,int> &map);   // 就地接收区：超出 kRxCacheBytes(≈10MB) 丢弃最旧
    qint64 rxRowBytes(QTableWidget *t, int row) const;               // 一行占用的近似字节数
    void   rxAddRowBytes(QTableWidget *t, int row);                  // 记入一行字节（新增行）
    QHash<QTableWidget*, qint64> m_rxBytes;                          // 各接收表已缓存字节数（用于 ≈10MB 上限）
    void onExportRx();                                               // 导出接收区（当前显示）数据
    QPushButton *makePauseButton();          // 生成接收区“暂停/恢复”按钮并登记
    void applyPauseUi();                     // 按 m_rxPaused 刷新所有暂停按钮文本/接收状态
    // ---- CyberBeast 协议页 ----
    void rebuildCbCommands();                // 按类别重建命令下拉
    void rebuildCbParams();                  // 按命令重建参数输入框
    void updateCbPreview();                  // 刷新预览（CAN ID / 数据 hex）
    QByteArray buildCbFrame(QString *err) const;   // 生成 CyberBeast 线上帧
    void cbFeedRx(const proto::RxFrame &f);
    void cbAppendScroll(const QString &dir, const proto::RxFrame &f);
    void cbSetRxMode(bool scroll);


    SerialLink *m_serial = nullptr;
    UsbLink    *m_usb = nullptr;
    Link       *m_link = nullptr;    // 当前活动传输（串口或 USB）
    // ---- EtherCAT 页控件（统一走厂商 joint-sdk；网卡枚举用 SOEM 静态接口）----
    QComboBox   *m_ecAdapterBox = nullptr;   // 网卡(适配器)下拉
    QPushButton *m_ecRefreshBtn = nullptr;
    QLabel      *m_ecStatusLbl = nullptr;
    QLineEdit   *m_ecIndexEdit = nullptr;     // 对象字典：索引(hex)
    QLineEdit   *m_ecSubEdit = nullptr;       // 对象字典：子索引
    QLineEdit   *m_ecValEdit = nullptr;       // 对象字典：值(hex)
    QPushButton *m_ecSdoReadBtn = nullptr;
    QPushButton *m_ecSdoWriteBtn = nullptr;
    QLabel      *m_ecSdoResultLbl = nullptr;
    // ---- EtherCAT ESI XML 对象字典 ----
    QPushButton *m_ecImportXmlBtn = nullptr;      // 导入 XML(ESI)
    QLabel      *m_ecXmlLabel = nullptr;          // 已导入文件名 / 统计
    QComboBox   *m_ecObjBox = nullptr;            // 对象下拉（来自 XML）
    EsiXmlData   m_esi;                           // 已解析的 ESI 数据
    QString      m_esiPath;                       // 已导入 XML 路径
    // ---- EtherCAT 关节（joint-sdk）----
    JointController *m_joint = nullptr;           // 关节控制器（jsdk）
    QSpinBox    *m_jointPosSpin = nullptr;        // 从站拓扑序号
    QComboBox   *m_jointModeBox = nullptr;        // 使能模式
    QPushButton *m_jointOpenBtn = nullptr;
    QPushButton *m_jointActivateBtn = nullptr;
    QPushButton *m_jointEnableBtn = nullptr;
    QPushButton *m_jointDisableBtn = nullptr;
    QPushButton *m_jointFaultBtn = nullptr;
    QPushButton *m_jointRunBtn = nullptr;
    QLineEdit   *m_jointTargetEdit = nullptr;
    QLabel      *m_jointStatusLbl = nullptr;
    QTimer      *m_jointTimer = nullptr;
    bool         m_jointStartAfterJob = false;    // 后台激活成功后自动启动周期
    EcJob       *m_ecJob = nullptr;               // EtherCAT 阻塞操作的工作线程
    bool         m_ecBusy = false;                // 是否有 EtherCAT 后台任务在跑

    // 串口/USB 传输选择
    QComboBox *m_transportBox = nullptr;
    QComboBox *m_portBox = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_updateFwBtn = nullptr;   // 更新固件（进入 DFU + 烧录）
    QPushButton *m_clearErrBtn = nullptr;    // 清除错误（命令 12；各页连接区各一份）

    // CAN 参数
    QComboBox *m_bitrateBox = nullptr;     // 仲裁域波特率下拉（命令 1）
    QComboBox *m_dataBitrateBox = nullptr; // CAN FD 数据域波特率下拉（命令 8）
    QCheckBox *m_loopBox = nullptr;      // 循环发送复选框
    QSpinBox *m_intervalSpin = nullptr;  // 周期 ms

    // 单帧编辑区
    QLineEdit *m_idEdit = nullptr;       // ID(hex)，默认空
    QComboBox *m_idTypeBox = nullptr;    // 标准/扩展
    QComboBox *m_lenBox = nullptr;       // 发送字节长度
    QComboBox *m_frameTypeBox = nullptr; // 标准帧/FD帧/FD加速
    QLineEdit *m_dataEdit = nullptr;     // 数据(hex)，默认空

    // 列表（发送表格）
    QTableWidget *m_table = nullptr;
    QPushButton *m_resetBtn = nullptr;
    QPushButton *m_exportBtn = nullptr;
    QPushButton *m_exportClipBtn = nullptr;   // 导出列表记录（弹出剪切栏窗口）
    QPushButton *m_importBtn = nullptr;

    // 接收区（接收表格）
    QVector<RxFilterBar> m_rxFilters;   // 各接收区筛选行
    QTableWidget *m_rxTable = nullptr;
    QPushButton *m_clearRxBtn = nullptr;
    QHash<quint32, int> m_rxRowById;     // 帧 id -> 接收表行号
    QTableWidget *m_scrollTable = nullptr; // 滚动显示（带固定表头，数据行滚动追加）
    QToolButton *m_modeBtn = nullptr;    // 切换图标（就地更新 <-> 滚动显示）
    QLabel *m_modeLbl = nullptr;         // 上方文本提示：当前显示模式
    QCheckBox *m_tsBox = nullptr;        // 滚动行是否显示时间戳
    bool m_scrollMode = false;           // false=就地更新, true=滚动显示

    // 操作
    QPushButton *m_sendSingleBtn = nullptr;   // 单条发送（单帧编辑区下方）
    QPushButton *m_sendListBtn = nullptr;     // 列表发送（发送列表下方）
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_statusBtn = nullptr;
    QPushButton *m_clearLogBtn = nullptr;

    // 发送打包缓冲：一次发送/连接 的全部指令拼成一条消息写出
    QByteArray m_txBuf;
    int m_txFlushCount = 0;
    QByteArray m_lastFlush;

    // 上次发送状态（用于"未变化则不重发/不打包"）
    bool m_lastInit = false;
    bool m_lastIsList = false;
    QVector<QByteArray> m_lastFrames;
    int  m_lastMode = -1;        // 固件当前发送模式 (0 单帧 / 1 列表)；-1=未知
    int  m_lastInterval = -1;    // 上次下发的周期 ms

    QPlainTextEdit *m_log = nullptr;
    QLabel *m_statusLbl = nullptr;
    QLabel *m_errCntLbl = nullptr;   // 状态栏右侧 TEC/REC（AA AC id=3 主动上报）
    QVector<ConnPanel> m_connPanels;   // 各页面的连接区（[0] 为规范/通用CAN）
    QVector<LogPanel>  m_logPanels;    // 各页面的日志区（[0] 为规范/通用CAN）

    // 模式页面（顶部菜单「模式」切换）
    QStackedWidget *m_modeStack = nullptr;   // 0=CAN, 1=RS485, 2=EtherCAT
    QWidget *m_pageCan = nullptr;
    QWidget *m_pageRs485 = nullptr;
    QWidget *m_pageEtherCat = nullptr;
    // RS485 页：数据区 + 接收区（布局与通用CAN一致）
    QLineEdit    *m_rs485DataEdit = nullptr;   // 数据区：待发送数据(hex)
    QPushButton  *m_rs485SendBtn = nullptr;
    QPushButton  *m_rs485StopBtn = nullptr;
    QTableWidget *m_rs485RxTable = nullptr;
    QTableWidget *m_rs485ScrollTable = nullptr;
    QToolButton  *m_rs485ModeBtn = nullptr;
    QLabel       *m_rs485ModeLbl = nullptr;
    QCheckBox    *m_rs485TsBox = nullptr;
    QPushButton  *m_rs485ClearRxBtn = nullptr;
    bool          m_rs485ScrollMode = false;
    QAction *m_actionModeCan = nullptr;
    QAction *m_actionModeRs485 = nullptr;
    QAction *m_actionModeEtherCat = nullptr;

    // 协议调试页（CAN 页内）：协议菜单切换（CyberBeast / Simple / Open）
    QStackedWidget *m_protoStack = nullptr;
    QWidget *m_pageProtoCyberBeast = nullptr;
    QWidget *m_pageProtoSimple = nullptr;
    QWidget *m_pageProtoOpen = nullptr;
    QWidget *m_pageProtoGeneric = nullptr;    // 通用 CAN UI（现有那套）
    QAction *m_actionProtoCyberBeast = nullptr;
    QAction *m_actionProtoSimple = nullptr;
    QAction *m_actionProtoOpen = nullptr;
    QAction *m_actionProtoGeneric = nullptr;
    QString m_deviceFwVersion;               // 帮助：设备固件版本（命令 10 回包，预留）
    QMenu  *m_openMenu = nullptr;            // 当前由快捷键展开的菜单（切换时先收起）

    // ---- Simple 协议页控件 ----
    QComboBox   *m_simpleCmdBox = nullptr;      // 命令下拉
    QLineEdit   *m_simpleNodeEdit = nullptr;    // 节点 ID
    QLineEdit   *m_simpleGearEdit = nullptr;    // 减速比
    QWidget     *m_simpleParamArea = nullptr;   // 参数输入容器
    QVector<QLineEdit *> m_simpleParamEdits;    // 参数输入框（随命令重建）
    QVector<QLineEdit *> m_simpleExEdits;    // Simple 实战示例固定帧输入项
    QVector<QComboBox *> m_simpleUnitBoxes;     // 每个参数所选单位（与 m_simpleParamEdits 平行，可为空）
    // RxSdo(0x04) 专用控件：opcode(读/写) + Endpoint + 写类型 + Value（仅写模式生效）
    QComboBox   *m_sdoOpcodeBox = nullptr;
    QLineEdit   *m_sdoEndpointEdit = nullptr;
    QComboBox   *m_sdoTypeBox = nullptr;
    QLineEdit   *m_sdoValueEdit = nullptr;
    QLineEdit   *m_simplePreview = nullptr;     // 预览（CAN ID + 数据 hex）
    QPushButton *m_simpleSendBtn = nullptr;
    QPushButton *m_simpleStopBtn = nullptr;
    QTableWidget *m_simpleRxTable = nullptr;    // 接收：就地汇总
    QTableWidget *m_simpleScrollTable = nullptr;// 接收：滚动
    QHash<quint32,int> m_simpleRxRowById;
    QToolButton *m_simpleModeBtn = nullptr;
    QLabel      *m_simpleModeLbl = nullptr;
    QCheckBox   *m_simpleTsBox = nullptr;
    QPushButton *m_simpleClearRxBtn = nullptr;
    bool         m_simpleScrollMode = false;
    QByteArray   m_simpleLastFrame;             // 上次发送的帧（判变化）
    bool         m_simpleLastInit = false;

    // ---- Open（CANopen）协议页控件 ----
    QLineEdit   *m_coNodeEdit = nullptr;        // node_id
    QLineEdit   *m_coGearEdit = nullptr;        // 减速比（输出轴:电机轴）
    QComboBox   *m_coFuncBox = nullptr;         // 功能/COB-ID（含实战示例）
    QWidget     *m_coParamArea = nullptr;       // 动态参数容器
    QComboBox   *m_coNmtCmdBox = nullptr;       // NMT 状态命令字
    QComboBox   *m_coSdoOpBox = nullptr;        // SDO 操作（写/读）
    QComboBox   *m_coSdoIdxBox = nullptr;       // SDO 索引（EDS 下拉，含子索引/类型）
    QLineEdit   *m_coSdoValueEdit = nullptr;    // SDO 写值
    // PDO（RPDO/TPDO）多行：EDS 对象下拉 + 用户数值
    QVector<CoPdoRow>     m_coPdoRows;           // PDO 行模型
    QHash<int, QVector<CoPdoRow>> m_coPdoCache;   // 会话内各 PDO 的值记忆（重开不保留）
    QVector<QComboBox *>  m_coPdoIdxBoxes;       // 行对象下拉（与 m_coPdoRows 平行）
    QVector<QLineEdit *>  m_coPdoValEdits;        // 行数值输入
    QPushButton *m_coPdoAddBtn = nullptr;         // 添加对象
    // 实战示例「PDO 映射修改」：可配置映射关系
    QComboBox *m_coMapPdoBox = nullptr;           // 要修改的 PDO（RPDO1-4/TPDO1-4）
    QVector<CoPdoRow> m_coMapRows;                // 目标映射对象（index/sub）
    int m_coMapCurPdo = -1;                       // 当前选中的 PDO（用于会话缓存）
    QHash<int, QVector<CoPdoRow>> m_coMapCache;   // 本次会话内各 PDO 的映射记忆（关闭不保存）
    QVector<QComboBox *> m_coMapObjBoxes;         // 目标对象下拉
    QPushButton *m_coMapAddBtn = nullptr;         // 追加目标对象
    // 命令列表（所有功能都可追加）
    QGroupBox   *m_coListBox = nullptr;           // 命令列表分组（实战示例时隐藏）
    QListWidget *m_coList = nullptr;              // 待发送命令列表
    QPushButton *m_coAddBtn = nullptr;            // 追加到列表
    QPushButton *m_coListClearBtn = nullptr;      // 清空列表
    QPushButton *m_coExportBtn = nullptr;         // 导出列表记录（弹出剪切栏窗口）
    QVector<CoListEntry> m_coListModel;           // 命令列表模型
    int          m_coEditRow = -1;                // 当前正在编辑的列表行（-1=追加模式）
    // 实战示例的用户输入（位置/速度/力矩），与 examples()[ex].inputs 平行
    QVector<QLineEdit *> m_coExValEdits;
    QVector<QComboBox *> m_coExUnitBoxes;
    QPushButton *m_coImportEdsBtn = nullptr;      // 导入 EDS
    QPushButton *m_coResetEdsBtn = nullptr;       // 恢复内置 EDS
    QComboBox   *m_coHbStateBox = nullptr;      // 心跳状态字节
    QLineEdit   *m_coPreview = nullptr;         // 预览
    QPushButton *m_coSendBtn = nullptr;
    QPushButton *m_coSendSingleBtn = nullptr;   // 单条发送（参数框中当前配置的单条命令，与列表无关）
    QWidget     *m_coSingleRow = nullptr;       // 单条发送按钮所在的独立行（实战示例时隐藏）
    QPushButton *m_coStopBtn = nullptr;
    QPushButton *m_coOdBrowserBtn = nullptr;    // 对象字典按钮
    QTableWidget *m_coRxTable = nullptr;        // 接收：就地汇总
    QTableWidget *m_coScrollTable = nullptr;    // 接收：滚动
    QHash<quint32,int> m_coRxRowById;
    QToolButton *m_coModeBtn = nullptr;
    QLabel      *m_coModeLbl = nullptr;
    QCheckBox   *m_coTsBox = nullptr;
    QPushButton *m_coClearRxBtn = nullptr;
    bool         m_coScrollMode = false;
    QByteArray   m_coLastFrame;                 // 上次发送的帧（判变化）
    int          m_coPdoFunc = -1;              // 上次填充默认映射的 PDO 功能（-1=无）
    bool         m_coLastInit = false;

    // ---- 接收暂停 ----
    bool m_rxPaused = false;                 // true=接收已暂停（下发命令 11 + 冻结本机）
    QVector<QPushButton *> m_pauseBtns;      // 各接收区的暂停按钮（文本随状态刷新）
    QHash<QString, QTableWidget *> m_parseAreas;             // 协议 key -> 解析区表格
    QHash<QString, QHash<quint32, int>> m_parseRowById;      // 协议 key -> (帧ID -> 行号)，就地更新
    // 接收区缓存上限：≈10MB（按文本字节数计），超出丢弃最旧
    // ---- 接收批量刷新 ----
    // 固件在总线繁忙时可达 8000 帧/s。若每帧都跑「4 个接收区 + 4 个协议解析 +
    // 若干次 setText」，主线程必然卡死（实测直接冻结界面）。这里把帧先压进队列，
    // 由 33 Hz 定时器一次批处理，帧率与 UI 开销彻底解耦。
    static constexpr int    kRxFlushIntervalMs = 33;    // ≈30 Hz
    static constexpr int    kRxQueueCap        = 200000; // 约 20 万帧，防内存失控
    void flushRxQueue();                                 // 定时器回调：批处理队列
    QVector<proto::RxFrame> m_rxQueue;                   // 待处理帧
    QTimer *m_rxFlushTimer = nullptr;
    quint64 m_rxDroppedByCap = 0;                        // 队列满丢弃计数（可观测）

    static constexpr qint64 kRxCacheBytes = 10LL * 1024 * 1024;
    static constexpr int    kRxMaxRows    = 1000000;   // 兜底行数上限（防极端情况下无限增长）
    QLineEdit *m_rxPartialEdit = nullptr;    // (预留)
    // ---- CyberBeast 协议页控件 ----
    QLineEdit   *m_cbMasterEdit = nullptr;   // 主站(源)地址
    QLineEdit   *m_cbDestEdit = nullptr;     // 目标设备/位掩码（广播）
    QComboBox   *m_cbCatBox = nullptr;       // 类别下拉
    QComboBox   *m_cbCmdBox = nullptr;       // 命令下拉
    QWidget     *m_cbParamArea = nullptr;    // 参数输入容器
    QVector<QLineEdit *> m_cbParamEdits;     // 参数输入框（随命令重建）
    QLineEdit   *m_cbPreview = nullptr;      // 预览
    QPushButton *m_cbSendBtn = nullptr;
    QPushButton *m_cbStopBtn = nullptr;
    QTableWidget *m_cbRxTable = nullptr;
    QTableWidget *m_cbScrollTable = nullptr;
    QHash<quint32,int> m_cbRxRowById;
    QToolButton *m_cbModeBtn = nullptr;
    QLabel      *m_cbModeLbl = nullptr;
    QCheckBox   *m_cbTsBox = nullptr;
    QPushButton *m_cbClearRxBtn = nullptr;
    bool         m_cbScrollMode = false;
    QByteArray   m_cbLastFrame;
    bool         m_cbLastInit = false;

    // DFU 设备检测轮询
    QTimer *m_dfuPollTimer = nullptr;
    int m_dfuPollCount = 0;
    bool m_dfuDialogOpen = false;   // 固件更新对话框已弹出（防定时器重入）
};

#endif // MAINWINDOW_H
