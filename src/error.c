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

// Sticky pending-report bits (one per error_t). Unlike a small FIFO, a bit
 // never silently drops: the LED can light only if a report bit is also set,
// and the main loop will eventually emit an AA AD for each pending type.
// Re-asserts of the same type while still pending coalesce (no flood).
static volatile uint32_t err_report_pending = 0;


// Assert an error condition (and mark it for deferred host reporting)
void error_assert(error_t err)
{
    if ((uint32_t)err >= (uint32_t)ERR_MAX)
        return;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    error_register |= (1u << err);
    error_timestamps[err] = system_get_ticks();
    err_report_pending |= (1u << err);

    __set_PRIMASK(primask);
}


// Pop one pending error report; returns -1 when none remain.
int error_take_pending(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (err_report_pending == 0)
    {
        __set_PRIMASK(primask);
        return -1;
    }

    for (uint8_t i = 0; i < ERR_MAX; i++)
    {
        uint32_t bit = (1u << i);
        if (err_report_pending & bit)
        {
            err_report_pending &= ~bit;
            __set_PRIMASK(primask);
            return (int)i;
        }
    }

    __set_PRIMASK(primask);
    return -1;
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
