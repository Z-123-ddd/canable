#include "CommandEditDialog.h"
#include "Protocol.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

CommandEditDialog::CommandEditDialog(const QVector<QByteArray> &wireFrames, QWidget *parent)
    : QDialog(parent), m_wire(wireFrames)
{
    setWindowTitle(QStringLiteral("编辑命令字节"));
    resize(520, 300);

    auto *v = new QVBoxLayout(this);
    v->addWidget(new QLabel(QStringLiteral(
        "双击记录编辑其字节内容：帧ID(hex) 与 数据(hex) 可直接修改。")));

    m_table = new QTableWidget(0, 3);
    m_table->setObjectName("editBytesTable");
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("帧ID(hex)"), QStringLiteral("数据(hex)")});
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);

    for (const QByteArray &f : m_wire) {
        proto::RxFrame rx;
        proto::decodeDataFrame(f, &rx);
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        auto *idx = new QTableWidgetItem(QString::number(r + 1));
        idx->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(r, 0, idx);
        m_table->setItem(r, 1, new QTableWidgetItem(
            QStringLiteral("0x%1").arg(rx.id, rx.ext ? 8 : 3, 16, QLatin1Char('0')).toUpper()));
        m_table->setItem(r, 2, new QTableWidgetItem(
            QString::fromLatin1(rx.data.toHex(' ').toUpper())));
    }
    v->addWidget(m_table, 1);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    bb->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
}

QVector<QByteArray> CommandEditDialog::frames() const
{
    QVector<QByteArray> out;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QString idTxt = m_table->item(r, 1) ? m_table->item(r, 1)->text().trimmed() : QString();
        QString dataTxt = m_table->item(r, 2) ? m_table->item(r, 2)->text().trimmed() : QString();
        bool ok = false;
        const quint32 id = idTxt.toUInt(&ok, 16);
        if (!ok) continue;
        QByteArray data = QByteArray::fromHex(
            dataTxt.remove(QLatin1Char(' ')).remove(QLatin1Char(':')).toLatin1());
        if (data.size() > 64) data.truncate(64);
        // 保留原帧格式位（ext/fd/brs/rtr），未变的按原值，找不到则按标准经典帧
        bool ext = false, fd = false, brs = false, rtr = false;
        if (r < m_wire.size()) {
            proto::RxFrame rx;
            if (proto::decodeDataFrame(m_wire.at(r), &rx)) { ext = rx.ext; fd = rx.fd; brs = rx.brs; rtr = rx.rtr; }
        }
        out.append(proto::buildDataFrame(id, ext, fd, brs, rtr,
                                         proto::bytesToDlc((uint8_t)data.size()), data));
    }
    return out;
}
