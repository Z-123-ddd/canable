/**
 * @file    can_timer.h
 * @brief   Periodic CAN send timer (GTIM1) for CANable2-N32
 *
 * A hardware timer that keeps running and interrupts at a configurable
 * interval (minimum 1ms, default 1ms). The ISR checks an enable switch;
 * only when enabled (and the TX ring holds data from the host) does it
 * trigger one CAN frame transmission per tick.
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
void can_timer_set_enabled(uint8_t enabled);  // master switch (default: disabled)
uint8_t can_timer_is_enabled(void);
void can_timer_set_period_ms(uint32_t period_ms);  // clamp to >= 1ms

#endif /* _CAN_TIMER_H */
