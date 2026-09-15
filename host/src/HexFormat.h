// ============================================================================
//  HexFormat — 十六进制输入框自动格式化
//    · 仅保留 0-9 / A-F（其余字符丢弃）
//    · 字母统一转为大写
//    · 每 2 位十六进制（= 1 字节）自动插入一个空格分隔
//  用法：
//    · 普通输入框用 HexLineEdit
//    · 表格数据列用 HexDelegate（编辑器为 HexLineEdit）
// ============================================================================
#ifndef HEXFORMAT_H
#define HEXFORMAT_H

#include <QLineEdit>
#include <QStyledItemDelegate>
#include <QString>
#include <QChar>

// 规范化十六进制文本：仅保留 0-9A-Fa-f，转大写，每 2 位插入一个空格。
inline QString formatHexBytes(const QString &in)
{
    QString hex;
    hex.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const ushort u = in.at(i).unicode();
        if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F'))
            hex.append(QChar(u).toUpper());
    }
    QString out;
    out.reserve(hex.size() + hex.size() / 2);
    for (int i = 0; i < hex.size(); ++i) {
        if (i > 0 && (i % 2) == 0) out.append(QLatin1Char(' '));
        out.append(hex.at(i));
    }
    return out;
}

// 十六进制输入框：输入时自动大写 + 每字节空格。
class HexLineEdit : public QLineEdit
{
public:
    explicit HexLineEdit(QWidget *parent = nullptr) : QLineEdit(parent)
    {
        connect(this, &QLineEdit::textEdited, this, [this](const QString &t) {
            const QString f = formatHexBytes(t);
            if (f != t) { setText(f); setCursorPosition(f.size()); }
        });
    }
};

// 供表格单元格使用：编辑时弹出 HexLineEdit。
class HexDelegate : public QStyledItemDelegate
{
public:
    explicit HexDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &) const override
    {
        return new HexLineEdit(parent);
    }
};

#endif // HEXFORMAT_H
