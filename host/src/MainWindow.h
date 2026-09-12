// ============================================================================
//  MainWindow — CANable2-N32 上位机主界面
//    控件约定：
//      · 波特率选择     -> 下拉框
//      · 循环发送       -> 复选框（决定 TX_START 触发 value）
//      · 列表/单帧模式  -> 由列表勾选数量决定（≥2 列表 / 1 单帧 / 0 用编辑区）
//      · 布局：左=发送（编辑/列表/操作/日志），右=接收（按帧 id 就地更新）
// ============================================================================
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QVector>
#include <QHash>
#include "Protocol.h"

class SerialLink;
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
class QCloseEvent;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 组合并按规则发送（不含串口检查），返回本次动作摘要；供 UI 与无头自测复用
    QString doSend();
    // 当前日志文本（供无头自测读取）
    QString logText() const;
    // 串口写入次数 / 最近一次写入的字节（供无头自测校验“打包一次发送”）
    int txFlushCount() const { return m_txFlushCount; }
    QByteArray lastTxFlush() const { return m_lastFlush; }

    // 接收区：把一帧写入接收表（按 id 就地更新，不滚动）；供 UI 与无头自测调用
    void feedRx(const proto::RxFrame &f);
    int  rxRowCount() const;
    QString rxCellText(int row, int col) const;
    // 接收区：滚动视图文本（供无头自测读取）
    QString scrollText() const;
    // 持久化（供无头自测复用；正常由 closeEvent 自动调用）
    void saveSettings();
    void loadSettings();

    // 导出已勾选记录为 CSV 文本（不含写文件）；供 UI 与无头自测校验
    QString buildExportCsv() const;

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
    void onImportCsv();     // 导入其他 CAN 工具的记录文件到发送列表
    void onUpdateFirmware();// 进入 DFU 并打开固件更新对话框
    void onClearRx();       // 清空接收区
    void onClearLog();      // 清空日志
    void onSend();
    void onStop();
    void onQueryStatus();

protected:
    void closeEvent(QCloseEvent *e) override;

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

    // 收集当前"有效载荷"：勾选>1 用勾选行(列表)，=1 用该行(单帧)，0 用编辑区(单帧)
    struct Payload {
        bool isList = false;          // true: 列表发送; false: 单帧
        QVector<QByteArray> frames;   // 要下发的数据帧（列表时为勾选行）
        bool empty() const { return frames.isEmpty(); }
    };
    Payload collectPayload(QString *info = nullptr) const;

    void sendCommand(proto::Cmd cmd, quint32 value, const QString &note);
    // 打包：入队到 m_txBuf（仅记录），flushTx 一次性写出
    void queueBytes(const QByteArray &bytes, const QString &note);
    void queueCommand(proto::Cmd cmd, quint32 value, const QString &note);
    void flushTx(const QString &note);
    void logWarn(const QString &text);          // 写入日志区（警告）
    void logError(const QString &text);         // 写入日志区（报错）
    void logInfo(const QString &text);          // 状态提示（写状态栏，不进日志）
    void appendScroll(const QString &dir, const proto::RxFrame &f);  // 滚动视图追加一行（含表头）
    void setRxMode(bool scroll);                // 切换接收区显示模式并更新提示/图标
    void setPortUiEnabled(bool open);

    SerialLink *m_serial = nullptr;
    UsbLink    *m_usb = nullptr;
    Link       *m_link = nullptr;    // 当前活动传输（串口或 USB）

    // 串口/USB 传输选择
    QComboBox *m_transportBox = nullptr;
    QComboBox *m_portBox = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_updateFwBtn = nullptr;   // 更新固件（进入 DFU + 烧录）

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
    QPushButton *m_importBtn = nullptr;

    // 接收区（接收表格）
    QTableWidget *m_rxTable = nullptr;
    QPushButton *m_clearRxBtn = nullptr;
    QHash<quint32, int> m_rxRowById;     // 帧 id -> 接收表行号
    QTableWidget *m_scrollTable = nullptr; // 滚动显示（带固定表头，数据行滚动追加）
    QToolButton *m_modeBtn = nullptr;    // 切换图标（就地更新 <-> 滚动显示）
    QLabel *m_modeLbl = nullptr;         // 上方文本提示：当前显示模式
    QCheckBox *m_tsBox = nullptr;        // 滚动行是否显示时间戳
    bool m_scrollMode = false;           // false=就地更新, true=滚动显示

    // 操作
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_statusBtn = nullptr;
    QPushButton *m_clearLogBtn = nullptr;

    // 发送打包缓冲：一次 doSend/连接 的全部指令拼成一条消息写出
    QByteArray m_txBuf;
    int m_txFlushCount = 0;
    QByteArray m_lastFlush;

    // 上次发送状态（用于"未变化则不重发/不打包"）
    bool m_lastInit = false;
    bool m_lastIsList = false;
    QVector<QByteArray> m_lastFrames;
    int  m_lastMode = -1;        // 上次下发的发送模式 (0 单帧 / 1 列表)
    int  m_lastInterval = -1;    // 上次下发的周期 ms

    QPlainTextEdit *m_log = nullptr;
    QLabel *m_statusLbl = nullptr;

    // DFU 设备检测轮询
    QTimer *m_dfuPollTimer = nullptr;
    int m_dfuPollCount = 0;
};

#endif // MAINWINDOW_H
