// ============================================================================
//  ObjectDictionaryDialog — CANopen 对象字典浏览器（EDS）
//    列出 ds402.eds 的对象（索引/子索引/名称/访问/类型/默认值），可过滤，
//    选中后「读 / 写」发出 SDO 请求（交由 MainWindow 经当前链路下发）。
// ============================================================================
#ifndef OBJECTDICTIONARYDIALOG_H
#define OBJECTDICTIONARYDIALOG_H

#include <QDialog>
#include <QVector>

class QLineEdit;
class QComboBox;
class QTableWidget;
class QLabel;

class ObjectDictionaryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ObjectDictionaryDialog(QWidget *parent = nullptr);

signals:
    void sdoRead(quint16 index, quint8 sub);
    void sdoWrite(quint16 index, quint8 sub, double value, int sdoType);

private slots:
    void onFilterChanged(const QString &text);
    void onSelectionChanged();
    void onReadClicked();
    void onWriteClicked();

private:
    void reload();

    QLineEdit    *m_filter = nullptr;
    QTableWidget *m_table = nullptr;
    QLineEdit    *m_value = nullptr;
    QComboBox    *m_typeBox = nullptr;
    QLabel       *m_hint = nullptr;
    QPushButton  *m_readBtn = nullptr;
    QPushButton  *m_writeBtn = nullptr;

    struct Row { quint16 index; int sub; };
    QVector<Row> m_rows;   // 当前显示行 → 对象
};

#endif // OBJECTDICTIONARYDIALOG_H
