// ============================================================================
//  CommandEditDialog — 手动编辑一条命令记录的字节内容
//    把一条命令的若干线上帧（AA AB 数据帧）解码为「帧ID(hex) + 数据(hex)」，
//    允许手工修改后重新编码。用于 CANopen 命令列表双击记录编辑。
// ============================================================================
#ifndef COMMANDEDITDIALOG_H
#define COMMANDEDITDIALOG_H

#include <QByteArray>
#include <QDialog>
#include <QVector>

class QTableWidget;

class CommandEditDialog : public QDialog
{
    Q_OBJECT
public:
    // wireFrames：AA AB 数据帧（proto::buildDataFrame 产物）
    explicit CommandEditDialog(const QVector<QByteArray> &wireFrames, QWidget *parent = nullptr);

    // 编辑后的线上帧
    QVector<QByteArray> frames() const;

private:
    QVector<QByteArray> m_wire;      // 原始线上帧（保留帧格式位）
    QTableWidget *m_table = nullptr;
};

#endif // COMMANDEDITDIALOG_H
