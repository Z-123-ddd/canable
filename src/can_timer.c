/**
 * @file    can_timer.c
 * @brief   Periodic CAN send timer (GTIM1) implementation
 *
 * GTIM1 sits on APB1. Clock tree (system.c): SYSCLK 240MHz, HCLK /1 = 240MHz,
 * PCLK1 = HCLK /4 = 60MHz. APB1 timer clock = PCLK1 * 2 = 120MHz (APB1
 * prescaler != 1). So the GTIM1 counter clock is fixed at 120MHz.
 *
 * Period = (PSC+1) * (ARR+1) / 120MHz. To keep the interrupt rate exact we
 * pick PSC/ARR so (PSC+1)*(ARR+1) == period_ms * 120000 with both factors
 * within their 16-bit register range (<=65536).
 *
 * The timer is started at boot and ALWAYS interrupts; the ISR checks an
 * enable switch (default OFF) before triggering a CAN send, so by default
 * the periodic transmission is disabled.
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "can_timer.h"

// GTIM1 counter clock (Hz): PCLK1 60MHz x 2 (APB1 prescaler /4)
#define CAN_TIMER_CLK_HZ   120000000UL

// Enable switch (default OFF: do not use the timed transmission)
static uint8_t can_timer_enabled = 0;

// Current interval (default 1ms, minimum 1ms)
static uint32_t can_timer_period_ms = CAN_TIMER_DEFAULT_PERIOD_MS;


// Recompute GTIM1 PSC/ARR for the current period and reload it.
// (PSC+1)*(ARR+1) = period_ms * 120000 (120MHz ticks per ms).
static void can_timer_reload(void)
{
    uint64_t n = (uint64_t)can_timer_period_ms * (CAN_TIMER_CLK_HZ / 1000UL);

    // Choose PSC+1 so that ARR+1 fits in 16 bits: psc1 = ceil(n / 65536)
    uint32_t psc1 = (uint32_t)((n + 65535UL) / 65536UL);
    if (psc1 < 1UL)
        psc1 = 1UL;
    if (psc1 > 65536UL)
        psc1 = 65536UL;

    uint32_t arr1 = (uint32_t)(n / psc1);
    uint32_t period = (arr1 > 0UL) ? (arr1 - 1UL) : 0UL;
    uint32_t prescaler = psc1 - 1UL;

    TIM_TimeBaseInitType tb;
    TIM_InitTimBaseStruct(&tb);
    tb.Prescaler    = prescaler;
    tb.Period       = period;
    tb.CounterMode  = TIM_CNT_MODE_UP;
    tb.ClkDiv       = TIM_CLK_DIV1;
    TIM_InitTimeBase(GTIM1, &tb);   // also generates an update event to reload
}


// Configure and start GTIM1. Called once at boot; the timer keeps running
// and interrupts every can_timer_period_ms (default 1ms).
void can_timer_init(void)
{
    // Enable GTIM1 clock (APB1)
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_GTIM1, ENABLE);

    // Load current period (default 1ms)
    can_timer_reload();

    // Enable update interrupt
    TIM_ConfigInt(GTIM1, TIM_INT_UPDATE, ENABLE);
    TIM_ClrIntPendingBit(GTIM1, TIM_INT_UPDATE);

    // NVIC: GTIM1_IRQn, preemption 4 (below SysTick 0, above USB LP 6 / FDCAN 7)
    NVIC_InitType nvic;
    nvic.NVIC_IRQChannel = GTIM1_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 4;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    // Start counting (always running)
    TIM_Enable(GTIM1, ENABLE);
}


// Master switch for the timed transmission (default OFF)
void can_timer_set_enabled(uint8_t enabled)
{
    can_timer_enabled = enabled ? 1 : 0;
}


uint8_t can_timer_is_enabled(void)
{
    return can_timer_enabled;
}


// Change the send interval (minimum 1ms). Disable the update interrupt while
// reloading so no stale interrupt fires mid-reconfiguration.
void can_timer_set_period_ms(uint32_t period_ms)
{
    if (period_ms < CAN_TIMER_MIN_PERIOD_MS)
        period_ms = CAN_TIMER_MIN_PERIOD_MS;

    TIM_ConfigInt(GTIM1, TIM_INT_UPDATE, DISABLE);

    can_timer_period_ms = period_ms;
    can_timer_reload();
    TIM_ClrIntPendingBit(GTIM1, TIM_INT_UPDATE);

    TIM_ConfigInt(GTIM1, TIM_INT_UPDATE, ENABLE);
}
