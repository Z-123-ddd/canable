#include "VendorDfuFlasher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace vendorDfu {

static QString rbDir()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(kRbDir);
}

static QString helperExe()
{
    return QDir(rbDir()).filePath(QStringLiteral("nsi_dfu_flash.exe"));
}

bool available()
{
    // helper 及它依赖的两个 DLL 都在才可用
    const QDir d(rbDir());
    return QFileInfo::exists(d.filePath(QStringLiteral("nsi_dfu_flash.exe")))
        && QFileInfo::exists(d.filePath(QStringLiteral("NsMcuCmdDll.dll")))
        && QFileInfo::exists(d.filePath(QStringLiteral("USB2UARTSPIIICDLL.dll")));
}

bool flash(const QString &binPath, quint32 addr, QString *err,
           const std::function<void(int, const QString &)> &progress)
{
    auto report = [&](int pct, const QString &stage) { if (progress) progress(pct, stage); };

    if (!QFileInfo::exists(binPath)) {
        if (err) *err = QStringLiteral("固件文件不存在: %1").arg(binPath);
        return false;
    }
    if (!available()) {
        if (err) *err = QStringLiteral("缺少厂商 DFU 运行时（%1）。").arg(helperExe());
        return false;
    }

    report(5, QStringLiteral("启动厂商 DFU 烧录"));

    QProcess proc;
    proc.setWorkingDirectory(rbDir());
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(helperExe(), { rbDir(), binPath,
                              QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0')) });
    if (!proc.waitForStarted(5000)) {
        if (err) *err = QStringLiteral("无法启动烧录进程: %1").arg(proc.errorString());
        return false;
    }

    QByteArray out;
    if (!proc.waitForFinished(90000)) {
        proc.kill();
        proc.waitForFinished(3000);
        if (err) *err = QStringLiteral("烧录超时（90s），已中止。");
        return false;
    }
    out = proc.readAll();

    // 逐行回调进度/阶段
    const QString text = QString::fromLocal8Bit(out);
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                                         Qt::SkipEmptyParts);
    int pct = 5;
    for (const QString &ln : lines) {
        if (ln.startsWith(QStringLiteral("erase")))       pct = 20;
        else if (ln.startsWith(QStringLiteral("download"))) pct = 80;
        else if (ln.startsWith(QStringLiteral("app_go")))   pct = 95;
        report(pct, ln.trimmed());
    }

    const bool ok = (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0)
                    && text.contains(QStringLiteral("RESULT: OK"));
    if (!ok) {
        if (err) *err = QStringLiteral("DFU 烧录失败:\n%1").arg(text.trimmed());
        return false;
    }
    report(100, QStringLiteral("完成"));
    return true;
}

} // namespace vendorDfu
