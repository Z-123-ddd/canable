// ============================================================================
//  Link — 传输抽象：串口(QSerialPort) 与 USB(libusb) 两种实现
//  两者共用同一套信号与 proto::Parser，MainWindow 只面向 Link 编程。
// ============================================================================
#ifndef LINK_H
#define LINK_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include "Protocol.h"

class Link : public QObject
{
    Q_OBJECT
public:
    explicit Link(QObject *parent = nullptr) : QObject(parent) {}
    ~Link() override = default;

    // 传输类型名（用于日志/UI）
    virtual QString kind() const = 0;
    // 打开（串口用端口名；USB 忽略参数）
    virtual bool open(const QString &arg = QString()) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // 当前连接标识（串口名或 USB VID:PID）
    virtual QString name() const = 0;
    // 发送原始协议字节
    virtual void send(const QByteArray &bytes) = 0;
    // 等待发送缓冲排空（默认无操作；串口实现会阻塞至写完）
    virtual void flush() {}

signals:
    void opened(const QString &name);
    void closed();
    void errorOccurred(const QString &msg);
    void frameReceived(const proto::RxFrame &f);
    void responseReceived(const proto::RxResponse &r);
    void statusLineReceived(const QString &line);
    void rawTx(const QByteArray &bytes);
    void rawRx(const QByteArray &bytes);

protected:
    // 把收到的字节喂给解析器并派发消息（子类在收到数据后调用）
    void feedParser(const QByteArray &data)
    {
        emit rawRx(data);
        m_parser.feed(data);
        const auto msgs = m_parser.takeMessages();
        for (const proto::Message &m : msgs) {
            switch (m.type) {
            case proto::Message::CanFrame:   emit frameReceived(m.frame); break;
            case proto::Message::Response:   emit responseReceived(m.response); break;
            case proto::Message::StatusLine: emit statusLineReceived(m.text); break;
            default: break;
            }
        }
    }
    void resetParser() { m_parser.reset(); }

private:
    proto::Parser m_parser;
};

#endif // LINK_H
