// ============================================================================
//  CommandClipboardDialog — 命令剪切栏窗口（列表式，可选择导出）
//    把一份 CAN 报文记录显示为**列表框**，支持：
//      · 单击选中一行、Shift+点击选中间范围、鼠标拖动选择多行（Ctrl 可加选）
//      · 「复制」把当前选中的记录复制到系统剪贴板（文本：帧ID|数据|标准/扩展）
//      · 「导出为文件」把当前选中的记录按所选格式导出（CSV/TXT/ASC/TRC/candump）
//    文本与各导出格式均由静态函数生成，便于无头自测直接校验。
//    列表框导出、接收区导出均复用本窗口（全局统一）。
// ============================================================================
#ifndef COMMANDCLIPBOARDDIALOG_H
#define COMMANDCLIPBOARDDIALOG_H

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <QVector>
#include <QDateTime>

class QComboBox;
class QLabel;
class QTableWidget;

// 一条待导出/复制的 CAN 报文记录
struct ClipRecord {
    quint32 id = 0;
    bool extended = false;   // 扩展帧(29bit)
    bool fd = false;         // CAN FD
    bool brs = false;        // FD 位速率切换
    QByteArray data;         // 0..64 字节
    // 该帧在主机侧的接收时刻（本地时间，毫秒）。0 = 未知（例如自测造的假数据、
    // 或来源没有时间信息），此时导出会留空而不是编造一个时间。
    qint64 tsMs = 0;
    // 记录时间戳的渲染（HH:mm:ss.zzz）；tsMs==0 时返回空串
    static QString stampText(qint64 ms);
};

class CommandClipboardDialog : public QDialog
{
    Q_OBJECT
public:
    enum Format { Txt, Csv, Asc, Trc, Candump };

    explicit CommandClipboardDialog(const QVector<ClipRecord> &records,
                                    QWidget *parent = nullptr,
                                    const QString &title = QStringLiteral("命令剪切栏"));

    // 当前**选中**的记录（按行序；无选中返回空）
    QVector<ClipRecord> selectedRecords() const;

    // 剪切文本："帧ID|数据|标准/扩展"，每行一条（供显示/复制/自测）
    static QString pipeText(const QVector<ClipRecord> &records);
    // 生成指定格式的导出文本（供导出文件/自测）
    // stamp = 本次导出动作的时间（写入文件头注释）；无效值则不加该行
    static QString exportText(const QVector<ClipRecord> &records, Format fmt,
                              const QDateTime &stamp = QDateTime());
    // 指定格式的默认文件扩展名（不含点）
    static QString defaultExt(Format fmt);
    // 指定格式的 QFileDialog 过滤器
    static QString fileFilter(Format fmt);

private slots:
    void selectAll();
    void selectNone();
    void copyToClipboard();
    void exportToFile();

private:
    QVector<ClipRecord> m_records;
    QComboBox *m_fmtBox = nullptr;
    QLabel *m_hint = nullptr;
    QTableWidget *m_table = nullptr;   // 列表框：0=序号 1=帧ID 2=数据 3=标准/扩展（行选择）
};

#endif // COMMANDCLIPBOARDDIALOG_H

