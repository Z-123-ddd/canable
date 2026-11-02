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

// Per-type pending report counts. Each new report increments; the main
// loop pops one AA AD per count (saturates at 65535).
static volatile uint16_t err_report_count[ERR_MAX];


static void error_stamp_and_queue(error_t err, uint8_t queue_report)
{
    error_register |= (1u << err);
    error_timestamps[err] = system_get_ticks();
    if (queue_report && err_report_count[err] < 0xFFFFu)
        err_report_count[err]++;
}


// Assert an error condition (always queues a host report)
void error_assert(error_t err)
{
    if ((uint32_t)err >= (uint32_t)ERR_MAX)
        return;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    error_stamp_and_queue(err, 1u);
    __set_PRIMASK(primask);
}


// Sticky raise: AA AD only on 0->1 transition (keeps bit set while condition lasts)
void error_raise(error_t err)
{
    if ((uint32_t)err >= (uint32_t)ERR_MAX)
        return;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t bit = (1u << err);
    uint8_t rising = (error_register & bit) ? 0u : 1u;
    error_stamp_and_queue(err, rising);

    __set_PRIMASK(primask);
}


// Clear a sticky error bit (does not emit a host report)
void error_clear(error_t err)
{
    if ((uint32_t)err >= (uint32_t)ERR_MAX)
        return;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    error_register &= ~(1u << err);
    __set_PRIMASK(primask);
}


// Clear the whole error register and discard any queued AA AD reports.
void error_clear_all(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    error_register = 0;
    for (uint8_t i = 0; i < ERR_MAX; i++)
        err_report_count[i] = 0;
    __set_PRIMASK(primask);
}


// Pop one pending error report; returns -1 when none remain.
int error_take_pending(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    for (uint8_t i = 0; i < ERR_MAX; i++)
    {
        if (err_report_count[i] > 0u)
        {
            err_report_count[i]--;
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
