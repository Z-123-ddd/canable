#include "ObjectDictionaryDialog.h"
#include "CanOpenProto.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QAbstractItemView>
#include <QScreen>
#include <QGuiApplication>

namespace {
int sdoTypeFromOdType(const QString &dt)
{
    if (dt == QLatin1String("INT8"))   return canopen::SDO_INT8;
    if (dt == QLatin1String("INT16"))  return canopen::SDO_INT16;
    if (dt == QLatin1String("INT32"))  return canopen::SDO_INT32;
    if (dt == QLatin1String("UINT8"))  return canopen::SDO_UINT8;
    if (dt == QLatin1String("UINT16")) return canopen::SDO_UINT16;
    if (dt == QLatin1String("UINT32")) return canopen::SDO_UINT32;
    if (dt == QLatin1String("REAL32")) return canopen::SDO_FLOAT32;
    return canopen::SDO_UINT32;
}
} // namespace

ObjectDictionaryDialog::ObjectDictionaryDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("CANopen 对象字典（ds402.eds）"));
    resize(820, 600);
    if (QScreen *sc = screen() ? screen() : QGuiApplication::primaryScreen()) {
        const QRect a = sc->availableGeometry();
        const int w = qMin(width(), a.width() * 90 / 100);
        const int h = qMin(height(), a.height() * 90 / 100);
        resize(w, h);
        move(a.center() - QPoint(w / 2, h / 2));
    }

    auto *root = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("过滤（索引/名称）:")));
    m_filter = new QLineEdit;
    m_filter->setObjectName(QStringLiteral("odFilter"));
    m_filter->setPlaceholderText(QStringLiteral("如 6040 或 Controlword"));
    top->addWidget(m_filter, 1);
    root->addLayout(top);

    m_table = new QTableWidget(0, 6);
    m_table->setObjectName(QStringLiteral("odTable"));
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("索引"), QStringLiteral("子索引"), QStringLiteral("名称"),
        QStringLiteral("访问"), QStringLiteral("类型"), QStringLiteral("默认值")});
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int c : {0, 1, 3, 4, 5})
        m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    root->addWidget(m_table, 1);

    auto *bot = new QHBoxLayout;
    bot->addWidget(new QLabel(QStringLiteral("值:")));
    m_value = new QLineEdit(QStringLiteral("0"));
    m_value->setObjectName(QStringLiteral("odValue"));
    bot->addWidget(m_value);
    bot->addWidget(new QLabel(QStringLiteral("类型:")));
    m_typeBox = new QComboBox;
    m_typeBox->setObjectName(QStringLiteral("odType"));
    for (const QString &n : canopen::sdoTypeNames()) m_typeBox->addItem(n);
    bot->addWidget(m_typeBox);
    auto *readBtn = new QPushButton(QStringLiteral("读 (SDO)"));
    readBtn->setObjectName(QStringLiteral("odReadBtn"));
    auto *writeBtn = new QPushButton(QStringLiteral("写 (SDO)"));
    writeBtn->setObjectName(QStringLiteral("odWriteBtn"));
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"));
    bot->addWidget(readBtn);
    bot->addWidget(writeBtn);
    bot->addStretch();
    bot->addWidget(closeBtn);
    root->addLayout(bot);

    m_hint = new QLabel(QStringLiteral("选中一行后「读/写」会通过当前链路发送 SDO 请求（0x600+node）。"));
    m_hint->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(m_hint);

    connect(m_filter, &QLineEdit::textChanged, this, &ObjectDictionaryDialog::onFilterChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &ObjectDictionaryDialog::onSelectionChanged);
    connect(readBtn,  &QPushButton::clicked, this, &ObjectDictionaryDialog::onReadClicked);
    connect(writeBtn, &QPushButton::clicked, this, &ObjectDictionaryDialog::onWriteClicked);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    reload();
}

void ObjectDictionaryDialog::reload()
{
    const QString flt = m_filter ? m_filter->text().trimmed().toLower() : QString();
    const QVector<canopen::OdEntry> od = canopen::objectDictionary();

    m_table->setRowCount(0);
    m_rows.clear();
    for (const canopen::OdEntry &e : od) {
        const QString idx = QStringLiteral("0x%1").arg(e.index, 4, 16, QLatin1Char('0')).toUpper();
        const QString sub = (e.sub < 0) ? QString() : QStringLiteral("0x%1").arg(e.sub, 2, 16, QLatin1Char('0')).toUpper();
        if (!flt.isEmpty()) {
            const QString hay = (idx + QLatin1Char(' ') + sub + QLatin1Char(' ') + e.name).toLower();
            if (!hay.contains(flt)) continue;
        }
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        auto set = [&](int c, const QString &t) {
            auto *it = new QTableWidgetItem(t);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_table->setItem(r, c, it);
        };
        set(0, idx);
        set(1, sub);
        set(2, e.name);
        set(3, e.access);
        set(4, e.dataType);
        set(5, e.dflt);
        m_rows.append({ e.index, e.sub });
    }
}

void ObjectDictionaryDialog::onFilterChanged(const QString &)
{
    reload();
}

void ObjectDictionaryDialog::onSelectionChanged()
{
    const int r = m_table->currentRow();
    if (r < 0 || r >= m_rows.size()) return;
    // 用该条目的默认值/类型预填
    auto *dfltItem = m_table->item(r, 5);
    auto *typeItem = m_table->item(r, 4);
    if (dfltItem) m_value->setText(dfltItem->text());
    if (typeItem) {
        const int t = sdoTypeFromOdType(typeItem->text());
        if (t >= 0 && t < m_typeBox->count()) m_typeBox->setCurrentIndex(t);
    }
}

void ObjectDictionaryDialog::onReadClicked()
{
    const int r = m_table->currentRow();
    if (r < 0 || r >= m_rows.size()) { m_hint->setText(QStringLiteral("请先选中一行。")); return; }
    emit sdoRead(m_rows.at(r).index, (quint8)(m_rows.at(r).sub < 0 ? 0 : m_rows.at(r).sub));
}

void ObjectDictionaryDialog::onWriteClicked()
{
    const int r = m_table->currentRow();
    if (r < 0 || r >= m_rows.size()) { m_hint->setText(QStringLiteral("请先选中一行。")); return; }
    emit sdoWrite(m_rows.at(r).index, (quint8)(m_rows.at(r).sub < 0 ? 0 : m_rows.at(r).sub),
                  m_value->text().toDouble(), m_typeBox->currentIndex());
}
