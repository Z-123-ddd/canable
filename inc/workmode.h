/**
 * @file    workmode.h
 * @brief   Debugger work-mode switch (CAN vs RS485) for CANable2-N32
 *
 * The device is a debugger/adapter that can work in one of two mutually
 * exclusive modes:
 *   - WORK_MODE_CAN   : the default CAN/CAN-FD bridge (FDCAN1 + USB protocol)
 *   - WORK_MODE_RS485 : RS485 half-duplex adapter (USART1 PA9/PA10 + DE PC6)
 *
 * Switching to RS485 stops the CAN side (TX job, periodic timer pump, FDCAN
 * controller + interrupts) and brings the RS485 up in the receive state
 * (DE = low). Switching back to CAN releases the RS485 (USART1/DMA) and
 * restarts FDCAN. The USB command channel stays alive in both modes so the
 * host can always switch back (USB command 9).
 */

#ifndef _WORKMODE_H
#define _WORKMODE_H

#include <stdint.h>

#define WORK_MODE_CAN     0u   /* default: CAN/CAN-FD bridge */
#define WORK_MODE_RS485   1u   /* RS485 half-duplex adapter  */

/* Current work mode. Defaults to WORK_MODE_CAN at boot. */
uint8_t workmode_get(void);

/**
 * @brief  Switch the work mode. No-op if @p mode equals the current mode.
 * @param  mode  WORK_MODE_CAN or WORK_MODE_RS485
 */
void workmode_set(uint8_t mode);

#endif /* _WORKMODE_H */
