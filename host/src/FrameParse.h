// ============================================================================
//  FrameParse — 接收帧“解析区”内容生成
//
//  按守护兽协议手册，把一帧 CAN 帧解析为可读文本：
//    · 帧 ID 号
//    · 命令 / 功能（MsgType / cmd_id / COB-ID 功能 / Modbus 功能码）
//    · 数据包中对应命令字节的含义（按手册规则还原为整数 / 浮点数 / 枚举）
//
//  各协议独立入口；MainWindow 的接收区下方 1/3「解析区」调用对应函数。
// ============================================================================
#ifndef FRAMEPARSE_H
#define FRAMEPARSE_H

#include <QString>
#include "Protocol.h"

namespace frameParse {

// 依据各协议手册解析一帧，返回一行可读文本。解析不了时返回带原始 hex 的兜底文本。
QString cyberBeast(const proto::RxFrame &f);   // CyberBeast v2.4（29-bit 扩展 ID）
QString simple(const proto::RxFrame &f);       // CAN Simple（11-bit，node<<5|cmd）
QString canopen(const proto::RxFrame &f);      // CANopen（COB-ID 功能段）
QString rs485(const proto::RxFrame &f);        // RS485 / Modbus RTU
QString generic(const proto::RxFrame &f);      // 通用：仅 ID/类型/长度/数据

} // namespace frameParse

#endif // FRAMEPARSE_H
