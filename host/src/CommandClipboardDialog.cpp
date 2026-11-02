#include "CommandClipboardDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QItemSelectionModel>
#include <algorithm>

namespace {

// 无前缀的十六进制 ID：标准 3 位 / 扩展 8 位
QString idPadded(const ClipRecord &r)
{
    return QString::number(r.id, 16).toUpper()
        .rightJustified(r.extended ? 8 : 3, QLatin1Char('0'));
}

// 数据（大写、空格分隔）
QString dataSpaced(const ClipRecord &r)
{
    return QString::fromLatin1(r.data.toHex(' ').toUpper());
}

} // namespace

CommandClipboardDialog::CommandClipboardDialog(const QVector<ClipRecord> &records,
                                               QWidget *parent, const QString &title)
    : QDialog(parent), m_records(records)
{
    setWindowTitle(title);
    resize(600, 420);

    auto *v = new QVBoxLayout(this);

    m_hint = new QLabel(QStringLiteral(
        "单击选中，Shift+点击选中间范围，鼠标拖动可选择多行；Ctrl+C 复制选中行，或选格式「导出为文件」。"));
    m_hint->setStyleSheet("color:#666;");
    v->addWidget(m_hint);

    // 列表框：序号 + 帧ID + 数据 + 标准/扩展（按行选择，支持 Shift/拖动/Ctrl 多选）
    m_table = new QTableWidget(0, 5);
    m_table->setObjectName("clipTable");
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("时间"), QStringLiteral("帧ID"),
        QStringLiteral("数据"), QStringLiteral("标准/扩展")});
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    for (int c : {0, 1, 2, 4})
        m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);  // 支持 Shift/拖动/Ctrl
    m_table->setDragEnabled(false);
    m_table->setDragDropMode(QAbstractItemView::NoDragDrop);
    m_table->verticalHeader()->setVisible(false);
    // 选中底色改浅（与命令列表观感一致），避免默认高亮过深
    m_table->setStyleSheet(QStringLiteral(
        "QTableWidget{gridline-color:transparent;}"
        "QTableWidget::item:selected{background:#cce8ff;color:#000;}"));
    for (int i = 0; i < m_records.size(); ++i) {
        const ClipRecord &r = m_records.at(i);
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *no = new QTableWidgetItem(QString::number(i + 1));
        no->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, 0, no);
        auto *tsIt = new QTableWidgetItem(ClipRecord::stampText(r.tsMs));
        tsIt->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        if (r.tsMs > 0)
            tsIt->setToolTip(QDateTime::fromMSecsSinceEpoch(r.tsMs)
                                 .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz t")));
        m_table->setItem(row, 1, tsIt);
        auto *idIt = new QTableWidgetItem(QStringLiteral("0x") + idPadded(r));
        idIt->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, 2, idIt);
        auto *dataIt = new QTableWidgetItem(dataSpaced(r));
        dataIt->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, 3, dataIt);
        auto *extIt = new QTableWidgetItem(r.extended ? QStringLiteral("扩展") : QStringLiteral("标准"));
        extIt->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, 4, extIt);
    }
    m_table->selectAll();   // 默认全选
    v->addWidget(m_table, 1);

    auto *row = new QHBoxLayout;
    auto *allBtn  = new QPushButton(QStringLiteral("全选"));
    allBtn->setObjectName("clipAllBtn");
    auto *noneBtn = new QPushButton(QStringLiteral("全不选"));
    noneBtn->setObjectName("clipNoneBtn");
    row->addWidget(allBtn);
    row->addWidget(noneBtn);
    row->addSpacing(12);
    row->addWidget(new QLabel(QStringLiteral("导出格式:")));
    m_fmtBox = new QComboBox;
    m_fmtBox->setObjectName("clipFmtBox");
    m_fmtBox->addItem(QStringLiteral("TXT 文本 (*.txt)"), (int)Txt);
    m_fmtBox->addItem(QStringLiteral("CSV 表格 (*.csv)"), (int)Csv);
    m_fmtBox->addItem(QStringLiteral("Vector ASC (*.asc)"), (int)Asc);
    m_fmtBox->addItem(QStringLiteral("PEAK TRC (*.trc)"), (int)Trc);
    m_fmtBox->addItem(QStringLiteral("candump (*.log)"), (int)Candump);
    row->addWidget(m_fmtBox);
    auto *exportBtn = new QPushButton(QStringLiteral("导出为文件"));
    exportBtn->setObjectName("clipExportBtn");
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"));
    row->addWidget(exportBtn);
    row->addStretch();
    row->addWidget(closeBtn);
    v->addLayout(row);

    connect(allBtn, &QPushButton::clicked, this, &CommandClipboardDialog::selectAll);
    connect(noneBtn, &QPushButton::clicked, this, &CommandClipboardDialog::selectNone);
    connect(exportBtn, &QPushButton::clicked, this, &CommandClipboardDialog::exportToFile);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    // Ctrl+C：复制选中记录到剪贴板（无“复制”按钮，仅快捷键）
    auto *copySc = new QShortcut(QKeySequence::Copy, this);
    copySc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copySc, &QShortcut::activated, this, &CommandClipboardDialog::copyToClipboard);
}

QVector<ClipRecord> CommandClipboardDialog::selectedRecords() const
{
    QVector<ClipRecord> out;
    if (!m_table || !m_table->selectionModel()) return out;
    // 收集被选中的行号（去重、按行序）
    QVector<int> rows;
    const QModelIndexList idx = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &i : idx) rows.append(i.row());
    if (rows.isEmpty()) {
        // 某些情况下只选中单元格：回退用 selectedIndexes
        const QModelIndexList all = m_table->selectionModel()->selectedIndexes();
        for (const QModelIndex &i : all) rows.append(i.row());
    }
    std::sort(rows.begin(), rows.end());
    int last = -1;
    for (int r : rows) {
        if (r == last) continue;
        last = r;
        if (r >= 0 && r < m_records.size()) out.append(m_records.at(r));
    }
    return out;
}

void CommandClipboardDialog::selectAll()
{
    if (m_table) m_table->selectAll();
}

void CommandClipboardDialog::selectNone()
{
    if (m_table) m_table->clearSelection();
}

QString CommandClipboardDialog::pipeText(const QVector<ClipRecord> &records)
{
    QString out;
    for (const ClipRecord &r : records) {
        out += QStringLiteral("0x%1|%2|%3\n")
                   .arg(idPadded(r), dataSpaced(r),
                        r.extended ? QStringLiteral("扩展") : QStringLiteral("标准"));
    }
    return out;
}

/* 记录时间戳的显示文本。0 表示来源没有时间信息，返回空串而不是伪造时间。 */
QString ClipRecord::stampText(qint64 ms)
{
    if (ms <= 0) return QString();
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString CommandClipboardDialog::exportText(const QVector<ClipRecord> &records, Format fmt,
                                          const QDateTime &stamp)
{
    /* 导出时间戳：这是「本次导出动作的时间」，与每条记录自身的时间字段无关。
     * 一次性取好再传进来，保证同一次导出内所有格式时间一致；写在文件头部的注释，
     * 不新增列、不改动既有列顺序，因此不破坏下游解析。
     * stamp 无效时（内部自检等调用）不输出这一行。 */
    QString banner;
    if (stamp.isValid()) {
        banner = QStringLiteral("; 导出时间: %1\r\n")
                     .arg(stamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz t")));
    }
    QString out;
    switch (fmt) {
    case Txt:
        return banner + pipeText(records).replace(QLatin1Char('\n'), QStringLiteral("\r\n"));

    case Csv: {
        out += banner;            // 注释行：Excel 当普通文本，列结构不变
        out += QStringLiteral("序号,时间,帧ID,帧格式,帧类型,数据长度,数据\r\n");
        int seq = 0;
        for (const ClipRecord &r : records) {
            const QString idStr = QStringLiteral("0x") + idPadded(r);
            const QString fmtS  = r.extended ? QStringLiteral("扩展帧") : QStringLiteral("标准帧");
            const QString typeS = r.fd ? (r.brs ? QStringLiteral("CAN FD加速")
                                                : QStringLiteral("CAN FD"))
                                       : QStringLiteral("CAN");
            out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7\r\n")
                       .arg(++seq)
                       .arg(ClipRecord::stampText(r.tsMs))
                      .arg(idStr, fmtS, typeS)
                       .arg(QString::number(r.data.size()), dataSpaced(r));
        }
        return out;
    }

    case Asc: {
        out += banner;
        // 经典: <ts> <ch> <id>[x] <dir> d <dlc> <data...>
        // FD:   <ts> CANFD <ch> <dir> <id> <brs> <esi> <dlc> <len> <data...>
        int seq = 0;
        for (const ClipRecord &r : records) {
            // 时间列：ASC 规范要求「秒.微秒」相对时间，用序号换算保持合规；
            // 记录的真实接收时刻见文件头注释与命令行内嵌时间戳。
            ++seq;
            const QString ts = QStringLiteral("0.%1").arg(seq, 6, 10, QLatin1Char('0'));
            const QString idtok = idPadded(r) + (r.extended ? QStringLiteral("x") : QString());
            if (!r.fd) {
                out += QStringLiteral("%1 1  %2  Tx  d %3 %4\r\n")
                           .arg(ts, idtok)
                           .arg(r.data.size()).arg(dataSpaced(r));
            } else {
                out += QStringLiteral("%1 CANFD 1 Tx %2 %3 0 %4 %5 %6\r\n")
                           .arg(ts, idtok)
                           .arg(r.brs ? 1 : 0)
                           .arg(r.data.size()).arg(r.data.size()).arg(dataSpaced(r));
            }
        }
        return out;
    }

    case Trc: {
        out += banner;
        out += QStringLiteral(";$FILEVERSION=1.3\r\n");
        out += QStringLiteral(";$DELIMITER=\r\n");
        out += QStringLiteral(";$COLUMNS=N,O,T,I,d,L,D\r\n");
        int seq = 0;
        for (const ClipRecord &r : records) {
            const QString type = !r.fd ? QStringLiteral("Rx")
                               : r.brs ? QStringLiteral("FB")
                                       : QStringLiteral("FD");
            // 列：N) O T I d L D —— 前 6 列各占一个空白分隔的 token，最后 D 列保留整段数据
            // TRC 规范要求相对秒（秒.4 位小数），保持合规
            const QString tsec = QStringLiteral("0.0000");
            out += QStringLiteral("%1) %2 %3 %4 %5 %6 %7\r\n")
                       .arg(++seq).arg(tsec).arg(type, idPadded(r))
                       .arg(r.data.size()).arg(r.data.size()).arg(dataSpaced(r));
        }
        return out;
    }

    case Candump:
        out += banner;
        for (const ClipRecord &r : records) {
            // 行格式保持 "can0 id [len] data"（解析器不认带括号前缀）
            out += QStringLiteral("can0  %1  [%2]  %3\r\n")
                       .arg(idPadded(r)).arg(r.data.size()).arg(dataSpaced(r));
        }
        return out;
    }
    return out;
}

QString CommandClipboardDialog::defaultExt(Format fmt)
{
    switch (fmt) {
    case Txt:     return QStringLiteral("txt");
    case Csv:     return QStringLiteral("csv");
    case Asc:     return QStringLiteral("asc");
    case Trc:     return QStringLiteral("trc");
    case Candump: return QStringLiteral("log");
    }
    return QStringLiteral("txt");
}

QString CommandClipboardDialog::fileFilter(Format fmt)
{
    switch (fmt) {
    case Txt:     return QStringLiteral("文本文件 (*.txt)");
    case Csv:     return QStringLiteral("CSV 文件 (*.csv)");
    case Asc:     return QStringLiteral("Vector ASC (*.asc)");
    case Trc:     return QStringLiteral("PEAK TRC (*.trc)");
    case Candump: return QStringLiteral("candump 日志 (*.log)");
    }
    return QStringLiteral("所有文件 (*.*)");
}

void CommandClipboardDialog::copyToClipboard()
{
    const QVector<ClipRecord> sel = selectedRecords();
    if (sel.isEmpty()) { m_hint->setText(QStringLiteral("未选择任何记录。")); return; }
    QApplication::clipboard()->setText(pipeText(sel));
    m_hint->setText(QStringLiteral("已复制 %1 条记录到剪贴板。").arg(sel.size()));
}

void CommandClipboardDialog::exportToFile()
{
    const QVector<ClipRecord> sel = selectedRecords();
    if (sel.isEmpty()) { m_hint->setText(QStringLiteral("未选择任何记录。")); return; }

    const Format fmt = static_cast<Format>(m_fmtBox->currentData().toInt());
    const QString ext = defaultExt(fmt);
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出记录"),
        QStringLiteral("can_list.") + ext, fileFilter(fmt));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1Char('.') + ext, Qt::CaseInsensitive))
        path += QLatin1Char('.') + ext;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_hint->setText(QStringLiteral("导出失败：无法写入 %1").arg(path));
        return;
    }
    // 只取一次时间，保证整个文件内一致（不在循环里逐行取）
    const QDateTime stamp = QDateTime::currentDateTime();
    QByteArray body = exportText(sel, fmt, stamp).toUtf8();
    if (fmt == Csv)
        f.write("\xEF\xBB\xBF");   // UTF-8 BOM，便于 Excel 识别中文
    f.write(body);
    f.close();
    m_hint->setText(QStringLiteral("已导出 %1 条记录到 %2").arg(sel.size()).arg(path));
}
