// ============================================================================
//  JointController — 用厂商 joint-sdk（SOEM v2 之上的 CiA402 机器人关节 SDK）
//  驱动 EtherCAT 关节：configure(→SAFE_OP) → activate(→OP) → 使能 → 周期收发。
//
//  为什么需要它：通用 SOEM 路径缺少本关节 ESC 固件要求的
//    · PRE-OP 写 PDO 分配(0x1C12/0x1C13) + PO2SO 配置(0x1C32/1C33 SyncType 等)
//    · DC SYNC0(2ms/250us) + AssignActivate(0x0980)
//    · blockLRW=1（IgH 式 LWR+LRD）
//    · SAFE_OP 预热 + ~1ms 节拍
//  这些恰是 joint-sdk 的强项，故直接复用。
// ============================================================================
#ifndef JOINTCONTROLLER_H
#define JOINTCONTROLLER_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>

struct jsdk_context;
struct jsdk_joint;

class JointController
{
public:
    JointController();
    ~JointController();

    // 打开网卡并配置关节（position=从站拓扑序号；esiPath 非空则用 ESI，否则用内置 CyberBeast profile）
    bool open(const QString &ifname, int position, const QString &esiPath, QString *err);
    bool configure(QString *err);   // 扫描/校验/映射，进入 SAFE_OP
    bool activate(QString *err);    // 进入 OPERATIONAL

    void close();                   // deactivate + destroy
    bool isOpen() const { return m_ctx != nullptr; }
    bool isOperational() const { return m_operational; }

    // CiA402 命令
    bool enable(int mode, QString *err);   // mode: 8=CSP 9=CSV 10=CST
    void disable();
    void faultReset();
    void setTargetPosition(qint32 counts);

    // 对象字典 SDO（经 joint-sdk，阻塞；需已 configure）
    bool sdoRead(quint16 index, quint8 sub, QByteArray *out, QString *err);
    bool sdoWrite(quint16 index, quint8 sub, const QByteArray &data, QString *err);

    // 周期：一次 cycle_begin/end；返回 false 表示出错（如 WKC 不足）
    bool cycleStep();
    qint64 dcToffNs() const;

    // 反馈
    quint16 statusword() const;
    qint32  actualPosition() const;
    int     modeDisplay() const;
    bool    feedbackOnline() const;
    bool    feedbackOperational() const;
    int     expectedWkc() const;
    int     lastWkc() const;

    QString lastError() const;

private:
    jsdk_context *m_ctx = nullptr;
    jsdk_joint   *m_joint = nullptr;
    QString       m_esiPath;
    bool          m_operational = false;
};

#endif // JOINTCONTROLLER_H
