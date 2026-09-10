#include "SerialLink.h"

SerialLink::SerialLink(QObject *parent)
    : Link(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &SerialLink::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &SerialLink::onError);
}

QList<QSerialPortInfo> SerialLink::availablePorts()
{
    return QSerialPortInfo::availablePorts();
}

bool SerialLink::open(const QString &portName)
{
    if (m_port.isOpen())
        m_port.close();

    resetParser();
    m_port.setPortName(portName);
    // USB CDC：波特率无实际约束，取一个常见值即可
    m_port.setBaudRate(QSerialPort::Baud115200);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port.open(QIODevice::ReadWrite)) {
        emit errorOccurred(QStringLiteral("打开 %1 失败: %2")
                               .arg(portName, m_port.errorString()));
        return false;
    }
    emit opened(portName);
    return true;
}

void SerialLink::close()
{
    if (m_port.isOpen()) {
        m_port.close();
        emit closed();
    }
}

void SerialLink::send(const QByteArray &bytes)
{
    if (!m_port.isOpen() || bytes.isEmpty())
        return;
    m_port.write(bytes);
    emit rawTx(bytes);
}

void SerialLink::flush()
{
    if (!m_port.isOpen())
        return;
    m_port.flush();
    m_port.waitForBytesWritten(1000);   // 阻塞至内部缓冲写出（最多 1s）
}

void SerialLink::onReadyRead()
{
    const QByteArray data = m_port.readAll();
    if (data.isEmpty())
        return;
    feedParser(data);       // 解析并派发（基类统一处理）
}

void SerialLink::onError(QSerialPort::SerialPortError e)
{
    if (e == QSerialPort::NoError)
        return;
    if (e == QSerialPort::ResourceError) {
        emit errorOccurred(QStringLiteral("串口错误: %1").arg(m_port.errorString()));
        close();
    }
}
