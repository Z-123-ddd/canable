/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief CiA402 (DS402) constants shared by the joint SDK.
 */

#ifndef JSDK_CIA402_H
#define JSDK_CIA402_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CoE object dictionary entries used by the drive profile */
#define JSDK_CIA402_CONTROLWORD        0x6040u
#define JSDK_CIA402_STATUSWORD         0x6041u
#define JSDK_CIA402_MODE_OF_OPERATION  0x6060u
#define JSDK_CIA402_MODE_DISPLAY       0x6061u
#define JSDK_CIA402_TARGET_POSITION    0x607Au
#define JSDK_CIA402_ACTUAL_POSITION    0x6064u
#define JSDK_CIA402_TARGET_VELOCITY    0x60FFu
#define JSDK_CIA402_ACTUAL_VELOCITY    0x606Cu
#define JSDK_CIA402_TARGET_TORQUE      0x6071u
#define JSDK_CIA402_MAX_TORQUE         0x6072u  /* 0.1% rated */
#define JSDK_CIA402_ACTUAL_TORQUE      0x6077u
#define JSDK_CIA402_MAX_PROFILE_VELOCITY 0x607Fu /* counts/s */
#define JSDK_CIA402_PROFILE_VELOCITY   0x6081u  /* counts/s */
#define JSDK_CIA402_PROFILE_ACCEL      0x6083u
#define JSDK_CIA402_PROFILE_DECEL      0x6084u

/* CiA402 PDO assignment objects */
#define JSDK_CIA402_RXPDO_ASSIGN       0x1C12u
#define JSDK_CIA402_TXPDO_ASSIGN       0x1C13u

/* Controlword commands (IgH / TwinCAT: ENABLE = 0x000F) */
#define JSDK_CIA402_CW_SHUTDOWN        0x0006u
#define JSDK_CIA402_CW_SWITCH_ON       0x0007u
#define JSDK_CIA402_CW_DISABLE_OP      0x0007u
#define JSDK_CIA402_CW_ENABLE_OP       0x000Fu
#define JSDK_CIA402_CW_HOMING_START    0x001Fu /* OE + Homing operation start */
#define JSDK_CIA402_CW_DISABLE_VOLTAGE 0x0000u
#define JSDK_CIA402_CW_QUICK_STOP      0x0002u
#define JSDK_CIA402_CW_FAULT_RESET     0x0080u

#define JSDK_CIA402_SW_TARGET_REACHED  0x0400u
#define JSDK_CIA402_SW_HOMING_ATTAINED 0x1000u
#define JSDK_CIA402_SW_HOMING_ERROR    0x2000u

/* Statusword decode */
#define JSDK_CIA402_SW_FAULT           0x0008u
#define JSDK_CIA402_SW_MASK            0x006Fu
#define JSDK_CIA402_SW_SWITCH_ON_DISABLED 0x0040u
#define JSDK_CIA402_SW_READY_TO_SWITCH_ON 0x0021u
#define JSDK_CIA402_SW_SWITCHED_ON     0x0023u
#define JSDK_CIA402_SW_OPERATION_ENABLED 0x0027u

#ifdef __cplusplus
}
#endif

#endif
