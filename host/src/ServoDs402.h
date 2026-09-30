// ============================================================================
//  ServoDs402.h — CiA402 / DS402 伺服状态机（纯逻辑，无 Qt 依赖，便于无头自测）
//
//  控制字 0x6040 / 状态字 0x6041 的位定义与使能状态机：
//    Switch on disabled → (Shutdown 0x06) → Ready to switch on
//                        → (Switch on 0x07) → Switched on
//                        → (Enable operation 0x0F) → Operation enabled
//    故障（bit3）用 Fault reset 0x80 复位。
//  用于 EtherCAT(CoE) 周期过程数据中写控制字、读状态字确认使能。
// ============================================================================
#ifndef SERVODS402_H
#define SERVODS402_H

#include <QtGlobal>

namespace ds402 {

// 控制字 0x6040 位
enum Ctrl {
    CW_SWITCH_ON        = 0x0001,
    CW_ENABLE_VOLTAGE   = 0x0002,
    CW_QUICK_STOP       = 0x0004,   // 低有效
    CW_ENABLE_OPERATION = 0x0008,
    CW_FAULT_RESET      = 0x0080,
    CW_HALT             = 0x0100,
};

// 状态字 0x6041 位
enum SwBit {
    SW_READY_TO_SWITCH_ON = 0x0001,
    SW_SWITCHED_ON        = 0x0002,
    SW_OPERATION_ENABLED  = 0x0004,
    SW_FAULT              = 0x0008,
    SW_VOLTAGE_ENABLED    = 0x0010,
    SW_QUICK_STOP         = 0x0020,
    SW_SWITCH_ON_DISABLED = 0x0040,
    SW_WARNING            = 0x0080,
    SW_REMOTE             = 0x0200,
    SW_TARGET_REACHED     = 0x0400,
};

enum class State {
    NotReadyToSwitchOn,
    SwitchOnDisabled,
    ReadyToSwitchOn,
    SwitchedOn,
    OperationEnabled,
    QuickStopActive,
    FaultReactionActive,
    Fault,
    Unknown,
};

// 标准 CiA402 状态解码（按掩码判定）
inline State decode(quint16 sw)
{
    if ((sw & 0x004F) == 0x0000) return State::NotReadyToSwitchOn;
    if ((sw & 0x004F) == 0x0040) return State::SwitchOnDisabled;
    if ((sw & 0x006F) == 0x0021) return State::ReadyToSwitchOn;
    if ((sw & 0x006F) == 0x0023) return State::SwitchedOn;
    if ((sw & 0x006F) == 0x0027) return State::OperationEnabled;
    if ((sw & 0x006F) == 0x0007) return State::QuickStopActive;
    if ((sw & 0x004F) == 0x000F) return State::FaultReactionActive;
    if ((sw & 0x004F) == 0x0008) return State::Fault;
    return State::Unknown;
}

inline const char *stateName(State s)
{
    switch (s) {
    case State::NotReadyToSwitchOn:  return "NotReadyToSwitchOn";
    case State::SwitchOnDisabled:    return "SwitchOnDisabled";
    case State::ReadyToSwitchOn:     return "ReadyToSwitchOn";
    case State::SwitchedOn:          return "SwitchedOn";
    case State::OperationEnabled:    return "OperationEnabled";
    case State::QuickStopActive:     return "QuickStopActive";
    case State::FaultReactionActive: return "FaultReactionActive";
    case State::Fault:               return "Fault";
    default:                         return "Unknown";
    }
}

// 为使能，根据状态字给出下一个控制字（0x06→0x07→0x0F；故障用 0x80 复位后重来）
inline quint16 nextEnableControlword(quint16 sw)
{
    switch (decode(sw)) {
    case State::SwitchOnDisabled:    return CW_ENABLE_VOLTAGE | CW_QUICK_STOP;                 // 0x0006 Shutdown
    case State::ReadyToSwitchOn:     return CW_ENABLE_VOLTAGE | CW_QUICK_STOP | CW_SWITCH_ON;  // 0x0007 Switch on
    case State::SwitchedOn:          return 0x000F;                                            // Enable operation
    case State::OperationEnabled:    return 0x000F;                                            // 保持
    case State::Fault:
    case State::FaultReactionActive: return CW_FAULT_RESET;                                     // 0x0080 故障复位
    default:                         return 0x0006;   // NotReady/QuickStop/Unknown：先 Shutdown
    }
}

inline bool isEnabled(quint16 sw) { return decode(sw) == State::OperationEnabled; }
inline bool isFault(quint16 sw)   { return (sw & SW_FAULT) != 0; }

} // namespace ds402

#endif // SERVODS402_H
