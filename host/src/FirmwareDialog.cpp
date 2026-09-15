#include "FirmwareDialog.h"
#include "VendorDfuFlasher.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QTime>
#include <QScreen>
#include <QGuiApplication>

// ============================================================================
//  FlashWorker
// ============================================================================
FlashWorker::FlashWorker(const QString &path, quint32 addr, QObject *parent)
    : QThread(parent), m_path(path), m_addr(addr)
{
}

void FlashWorker::run()
{
    QString err;
    bool ok;
    if (vendorDfu::available()) {
        ok = vendorDfu::flash(m_path, m_addr, &err, [this](int pct, const QString &stage) {
            emit progress(pct, stage);
        });
    } else {
        // 兜底：自实现 libusb DfuSe 路径
        ok = dfu::flashBin(m_path, m_addr, &err, [this](int pct, const QString &stage) {
            emit progress(pct, stage);
        });
    }
    emit finished(ok, err);
}

// ============================================================================
//  FirmwareDialog
// ============================================================================
FirmwareDialog::FirmwareDialog(const dfu::Info &info, QWidget *parent)
    : QDialog(parent), m_info(info)
{
    setWindowTitle(QStringLiteral("固件更新 (DFU)"));
    resize(560, 420);
    if (QScreen *sc = screen() ? screen() : QGuiApplication::primaryScreen()) {
        const QRect a = sc->availableGeometry();
        const int w = qMin(width(), a.width() * 90 / 100);
        const int h = qMin(height(), a.height() * 90 / 100);
        resize(w, h);
        move(a.center() - QPoint(w / 2, h / 2));
    }

    auto *root = new QVBoxLayout(this);

    // 设备信息
    auto *devBox = new QGridLayout;
    devBox->addWidget(new QLabel(QStringLiteral("DFU 设备:")), 0, 0);
    QString devText = info.found
        ? QStringLiteral("VID:PID = %1:%2  总线 %3 地址 %4  接口 %5  %6")
              .arg(info.vid, 4, 16, QLatin1Char('0')).arg(info.pid, 4, 16, QLatin1Char('0'))
              .arg(info.bus).arg(info.address).arg(info.interface).arg(info.product)
        : QStringLiteral("(未检测到 DFU 设备)");
    devBox->addWidget(new QLabel(devText), 0, 1);
    devBox->addWidget(new QLabel(QStringLiteral("烧录地址:")), 1, 0);
    devBox->addWidget(new QLabel(QStringLiteral("0x08000000")), 1, 1);
    root->addLayout(devBox);

    // 固件文件选择
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(new QLabel(QStringLiteral("固件(.bin):")));
    m_pathEdit = new QLineEdit;
    m_pathEdit->setPlaceholderText(QStringLiteral("选择 .bin 固件文件"));
    fileRow->addWidget(m_pathEdit, 1);
    m_browseBtn = new QPushButton(QStringLiteral("浏览..."));
    fileRow->addWidget(m_browseBtn);
    root->addLayout(fileRow);

    // 进度
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    root->addWidget(m_progress);

    // 操作按钮
    auto *btnRow = new QHBoxLayout;
    m_flashBtn = new QPushButton(QStringLiteral("开始烧录"));
    m_flashBtn->setEnabled(info.found);
    m_closeBtn = new QPushButton(QStringLiteral("关闭"));
    btnRow->addStretch();
    btnRow->addWidget(m_flashBtn);
    btnRow->addWidget(m_closeBtn);
    root->addLayout(btnRow);

    // 日志
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    root->addWidget(m_log, 1);

    connect(m_browseBtn, &QPushButton::clicked, this, &FirmwareDialog::browse);
    connect(m_flashBtn, &QPushButton::clicked, this, &FirmwareDialog::startFlash);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::close);

    log(QStringLiteral("DFU 固件更新。烧录地址 0x08000000。"));
    if (!info.found)
        log(QStringLiteral("提示：未检测到 DFU 设备，请确认设备已进入 DFU 模式。"));
}

void FirmwareDialog::browse()
{
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择固件"),
        QString(), QStringLiteral("固件文件 (*.bin);;所有文件 (*)"));
    if (!path.isEmpty())
        m_pathEdit->setText(path);
}

void FirmwareDialog::startFlash()
{
    const QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("固件更新"), QStringLiteral("请先选择 .bin 固件文件。"));
        return;
    }
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("固件更新"), QStringLiteral("文件不存在：%1").arg(path));
        return;
    }

    m_flashBtn->setEnabled(false);
    m_browseBtn->setEnabled(false);
    m_progress->setValue(0);
    log(QStringLiteral("开始烧录：%1").arg(path));

    m_worker = new FlashWorker(path, 0x08000000, this);
    connect(m_worker, &FlashWorker::progress, this, &FirmwareDialog::onProgress);
    connect(m_worker, &FlashWorker::finished, this, &FirmwareDialog::onFinished);
    m_worker->start();
}

void FirmwareDialog::onProgress(int percent, const QString &stage)
{
    m_progress->setValue(percent);
    log(QStringLiteral("[%1%] %2").arg(percent).arg(stage));
}

void FirmwareDialog::onFinished(bool ok, const QString &err)
{
    m_progress->setValue(ok ? 100 : m_progress->value());
    if (ok)
        log(QStringLiteral("烧录成功，设备将重启运行新固件。"));
    else
        log(QStringLiteral("烧录失败：%1").arg(err));

    QMessageBox::information(this, QStringLiteral("固件更新"),
        ok ? QStringLiteral("烧录成功，设备已重启。")
           : QStringLiteral("烧录失败：%1").arg(err));

    m_flashBtn->setEnabled(true);
    m_browseBtn->setEnabled(true);
    if (m_worker) { m_worker->wait(2000); m_worker->deleteLater(); m_worker = nullptr; }
}

void FirmwareDialog::log(const QString &s)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
        .arg(QTime::currentTime().toString("HH:mm:ss"), s));
}
