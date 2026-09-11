/**
 * @file    error.c
 * @brief   Error tracking implementation
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "error.h"
#include "system.h"


// Error tracking storage
static uint32_t error_register = 0;
static uint32_t error_timestamps[ERR_MAX] = {0};

// Pending error reports. error_assert() may run in ISR context, so the AA AD
// frame is NOT sent here - we queue the error and the main loop drains it and
// transmits (avoids touching the USB TX ring from an interrupt).
#define ERR_REPORT_QUEUE 8
static volatile uint8_t  err_report_q[ERR_REPORT_QUEUE];
static volatile uint32_t err_report_head = 0;
static volatile uint32_t err_report_tail = 0;


// Assert an error condition (and queue it for host reporting)
void error_assert(error_t err)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    error_register |= (1u << err);
    error_timestamps[err] = system_get_ticks();

    // Queue a deferred host report (drop silently if the small queue is full).
    uint32_t next = (err_report_head + 1u) % ERR_REPORT_QUEUE;
    if (next != err_report_tail)
    {
        err_report_q[err_report_head] = (uint8_t)err;
        err_report_head = next;
    }

    __set_PRIMASK(primask);
}


// Pop one pending error report; returns -1 when the queue is empty.
int error_take_pending(void)
{
    if (err_report_tail == err_report_head)
        return -1;

    uint8_t e = err_report_q[err_report_tail];
    err_report_tail = (err_report_tail + 1u) % ERR_REPORT_QUEUE;
    return (int)e;
}


// Get timestamp of the most recent error
uint32_t error_last_timestamp(void)
{
    uint32_t latest = 0;
    for (uint8_t i = 0; i < ERR_MAX; i++)
    {
        if (error_timestamps[i] > latest)
            latest = error_timestamps[i];
    }
    return latest;
}


// Get the full error register
uint32_t error_reg(void)
{
    return error_register;
}
