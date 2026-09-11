// ============================================================================
//  TraceImporter — 导入其他 CAN 工具导出的记录文件，解析成 CAN/CAN FD 帧
//
//  自动识别并解析以下格式（字段约定对齐 python-can 的对应 Reader 实现）：
//    · Vector ASC      (.asc, CANalyzer/CANoe)   —— 经典帧 / CANFD 行 / 远程帧
//    · PEAK TRC        (.trc, PCAN-View)         —— v1.0 / v1.1 / v1.3 / v2.x
//    · candump         (socketcan)               —— 可读格式 与 -L 日志格式
//    · 通用 CSV        (.csv)                    —— 单列十六进制 或 每字节一列
//                                                    （兼容 python-can 的 Base64 data 列）
//
//  解析结果是一串标准帧描述，供上位机填进“发送列表”后重放。
// ============================================================================
#ifndef TRACEIMPORTER_H
#define TRACEIMPORTER_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace trace {

// 一帧标准化的 CAN/CAN FD 帧
struct Frame {
    quint32 id = 0;
    bool extended = false;   // 扩展帧(29bit)
    bool rtr = false;        // 远程帧
    bool fd = false;         // CAN FD
    bool brs = false;        // FD 位速率切换
    QByteArray data;         // 0..64 字节
    int len() const { return data.size(); }
};

struct Result {
    bool ok = false;
    QString format;            // 识别到的格式名（"Vector ASC" 等）
    QVector<Frame> frames;
    int skipped = 0;           // 被跳过的行（注释 / 错误帧 / 远程帧 / 无法解析）
    QString error;             // ok=false 时的原因
};

// 自动识别并解析文件
Result load(const QString &path);

} // namespace trace

#endif // TRACEIMPORTER_H
