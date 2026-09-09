/**
 * @file    can_timer.h
 * @brief   Periodic CAN send timer (GTIM1) for CANable2-N32
 *
 * A hardware timer that keeps running and interrupts at a configurable
 * interval (minimum 1ms, default 1ms). The ISR is gated by a software
 * enable flag (default OFF): only when enabled does each tick trigger the
 * CAN TX job pump (can_send_timer_tick), which reads the frame(s) cached in
 * RAM (single recent frame or the frame list) - so periodic transmission
 * does not depend on USB traffic once the job is started.
 *
 * The period / enable are intentionally exposed here so a future USB
 * command can change them at runtime (not wired to USB yet).
 */

#ifndef _CAN_TIMER_H
#define _CAN_TIMER_H

#include <stdint.h>

// Interval bounds
#define CAN_TIMER_MIN_PERIOD_MS     1U
#define CAN_TIMER_DEFAULT_PERIOD_MS 1U

// Prototypes
void can_timer_init(void);              // configure & start GTIM1 (always running)
void can_timer_set_enabled(uint8_t enabled);  // legacy master-switch setter
uint8_t can_timer_is_enabled(void);
void can_timer_set_period_ms(uint32_t period_ms);  // clamp to >= 1ms

// Reserved API for a future USB command frame (not wired up yet):
// software-gate the periodic TX. GTIM1 keeps counting and its update
// interrupt stays enabled; these only set/clear the flag checked in the
// ISR, so no tick is serviced while disabled.
void can_timer_enable(void);           // let ticks drive the CAN TX pump
void can_timer_disable(void);          // stop servicing ticks (idle)

#endif /* _CAN_TIMER_H */
