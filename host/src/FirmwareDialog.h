// ============================================================================
//  FirmwareDialog — DFU 固件更新对话框
//    选 .bin → 后台线程调用 dfu::flashBin 烧录到 0x08000000 → 显示进度
// ============================================================================
#ifndef FIRMWAREDIALOG_H
#define FIRMWAREDIALOG_H

#include <QDialog>
#include <QThread>
#include "dfu_device.h"

class QLineEdit;
class QPushButton;
class QProgressBar;
class QPlainTextEdit;
class QLabel;

// 后台烧录线程
class FlashWorker : public QThread
{
    Q_OBJECT
public:
    FlashWorker(const QString &path, quint32 addr, QObject *parent = nullptr);
signals:
    void progress(int percent, const QString &stage);
    void finished(bool ok, const QString &err);
protected:
    void run() override;
private:
    QString m_path;
    quint32 m_addr;
};

class FirmwareDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FirmwareDialog(const dfu::Info &info, QWidget *parent = nullptr);

private slots:
    void browse();
    void startFlash();
    void onProgress(int percent, const QString &stage);
    void onFinished(bool ok, const QString &err);

private:
    void log(const QString &s);

    dfu::Info m_info;
    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_browseBtn = nullptr;
    QPushButton *m_flashBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QProgressBar *m_progress = nullptr;
    QPlainTextEdit *m_log = nullptr;
    FlashWorker *m_worker = nullptr;
};

#endif // FIRMWAREDIALOG_H
