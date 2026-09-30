#include "JointController.h"

#include <QCoreApplication>
#include <QDir>

extern "C" {
#include <joint_sdk/joint_sdk.h>
}

JointController::JointController() = default;

JointController::~JointController() { close(); }

bool JointController::open(const QString &ifname, int position, const QString &esiPath, QString *err)
{
    close();

    // 运行期数据（dc_timing.conf / ESI）随 exe 放在 joint_sdk/ 下
    const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/joint_sdk");
    qputenv("JSDK_DC_TIMING_CONF", (base + QStringLiteral("/config/dc_timing.conf")).toLocal8Bit());

    jsdk_context_config_t cfg;
    jsdk_context_config_default(&cfg);
    const QByteArray ifba = ifname.toLocal8Bit();
    std::snprintf(cfg.ifname, sizeof(cfg.ifname), "%s", ifba.constData());

    m_ctx = jsdk_context_create(&cfg);
    if (!m_ctx) { if (err) *err = QStringLiteral("context_create 失败（网卡名/权限？需管理员 + Npcap）"); return false; }

    jsdk_joint_config_t jc;
    jsdk_joint_config_default(&jc);
    jc.position = (quint16)(position > 0 ? position : 1);
    QByteArray esiBa;
    if (!esiPath.isEmpty()) { esiBa = esiPath.toLocal8Bit(); jc.profile_name = esiBa.constData(); }
    else                    { jc.profile_name = nullptr; }   // 内置 CyberBeast profile

    if (jsdk_context_add_joint(m_ctx, &jc, &m_joint) != JSDK_OK) {
        if (err) *err = QString::fromLocal8Bit(jsdk_context_last_error(m_ctx));
        jsdk_context_destroy(m_ctx); m_ctx = nullptr; m_joint = nullptr;
        return false;
    }
    m_esiPath = esiPath;
    m_operational = false;
    return true;
}

bool JointController::configure(QString *err)
{
    if (!m_ctx) { if (err) *err = QStringLiteral("未打开网卡"); return false; }
    if (jsdk_context_configure(m_ctx) != JSDK_OK) {
        if (err) *err = QString::fromLocal8Bit(jsdk_context_last_error(m_ctx));
        return false;
    }
    return true;
}

bool JointController::activate(QString *err)
{
    if (!m_ctx) { if (err) *err = QStringLiteral("未配置总线"); return false; }
    if (jsdk_context_activate(m_ctx) != JSDK_OK) {
        if (err) *err = QString::fromLocal8Bit(jsdk_context_last_error(m_ctx));
        return false;
    }
    m_operational = true;
    return true;
}

void JointController::close()
{
    if (m_ctx) { jsdk_context_deactivate(m_ctx); jsdk_context_destroy(m_ctx); }
    m_ctx = nullptr; m_joint = nullptr; m_operational = false;
}

bool JointController::enable(int mode, QString *err)
{
    if (!m_joint) { if (err) *err = QStringLiteral("未配置关节"); return false; }
    jsdk_joint_request_enable(m_joint, (jsdk_mode_t)mode);
    return true;
}

void JointController::disable()      { if (m_joint) jsdk_joint_request_disable(m_joint); }
void JointController::faultReset()   { if (m_joint) jsdk_joint_request_fault_reset(m_joint); }
void JointController::setTargetPosition(qint32 counts) { if (m_joint) jsdk_joint_set_target_position(m_joint, counts); }

bool JointController::sdoRead(quint16 index, quint8 sub, QByteArray *out, QString *err)
{
    if (!m_joint) { if (err) *err = QStringLiteral("未配置关节"); return false; }
    char buf[256] = {0};
    int size = sizeof(buf);
    if (jsdk_joint_sdo_upload(m_joint, index, sub, buf, &size) != JSDK_OK) {
        if (err) *err = QStringLiteral("SDO 读 0x%1:%2 失败：%3")
                            .arg(index, 4, 16, QLatin1Char('0')).arg(sub)
                            .arg(QString::fromLocal8Bit(jsdk_context_last_error(m_ctx)));
        return false;
    }
    if (out) *out = QByteArray(buf, size);
    return true;
}

bool JointController::sdoWrite(quint16 index, quint8 sub, const QByteArray &data, QString *err)
{
    if (!m_joint) { if (err) *err = QStringLiteral("未配置关节"); return false; }
    if (jsdk_joint_sdo_download(m_joint, index, sub, data.constData(), data.size()) != JSDK_OK) {
        if (err) *err = QStringLiteral("SDO 写 0x%1:%2 失败：%3")
                            .arg(index, 4, 16, QLatin1Char('0')).arg(sub)
                            .arg(QString::fromLocal8Bit(jsdk_context_last_error(m_ctx)));
        return false;
    }
    return true;
}

bool JointController::cycleStep()
{
    if (!m_ctx) return false;
    if (jsdk_context_cycle_begin(m_ctx, 0) != JSDK_OK) return false;
    if (jsdk_context_cycle_end(m_ctx) != JSDK_OK) return false;
    return true;
}

qint64 JointController::dcToffNs() const { return m_ctx ? jsdk_context_dc_toff_ns(m_ctx) : 0; }

quint16 JointController::statusword() const
{
    jsdk_joint_feedback_t fb{};
    if (m_joint && jsdk_joint_get_feedback(m_joint, &fb) == JSDK_OK) return fb.statusword;
    return 0;
}

qint32 JointController::actualPosition() const
{
    jsdk_joint_feedback_t fb{};
    if (m_joint && jsdk_joint_get_feedback(m_joint, &fb) == JSDK_OK) return fb.actual_position;
    return 0;
}

int JointController::modeDisplay() const
{
    jsdk_joint_feedback_t fb{};
    if (m_joint && jsdk_joint_get_feedback(m_joint, &fb) == JSDK_OK) return fb.mode_display;
    return 0;
}

bool JointController::feedbackOnline() const
{
    jsdk_joint_feedback_t fb{};
    return m_joint && jsdk_joint_get_feedback(m_joint, &fb) == JSDK_OK && fb.online;
}

bool JointController::feedbackOperational() const
{
    jsdk_joint_feedback_t fb{};
    return m_joint && jsdk_joint_get_feedback(m_joint, &fb) == JSDK_OK && fb.operational;
}

int JointController::lastWkc() const
{
    jsdk_bus_state_t b{};
    if (m_ctx && jsdk_context_get_bus_state(const_cast<jsdk_context *>(m_ctx), &b) == JSDK_OK)
        return (int)b.working_counter;
    return -1;
}

int JointController::expectedWkc() const { return -1; }

QString JointController::lastError() const
{
    if (m_ctx) return QString::fromLocal8Bit(jsdk_context_last_error(m_ctx));
    return QString();
}
