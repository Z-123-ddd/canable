// ============================================================================
//  SerialLink — 串口传输（USB CDC 虚拟串口, QSerialPort）
// ============================================================================
#ifndef SERIALLINK_H
#define SERIALLINK_H

#include <QSerialPort>
#include <QSerialPortInfo>
#include <QList>
#include "Link.h"

class SerialLink : public Link
{
    Q_OBJECT
public:
    explicit SerialLink(QObject *parent = nullptr);

    static QList<QSerialPortInfo> availablePorts();

    QString kind() const override { return QStringLiteral("串口"); }
    bool open(const QString &portName = QString()) override;
    void close() override;
    bool isOpen() const override { return m_port.isOpen(); }
    QString name() const override { return m_port.portName(); }
    void send(const QByteArray &bytes) override;

private slots:
    void onReadyRead();
    void onError(QSerialPort::SerialPortError e);

private:
    QSerialPort m_port;
};

#endif // SERIALLINK_H
