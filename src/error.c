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


// Assert an error condition
void error_assert(error_t err)
{
    error_register |= (1 << err);
    error_timestamps[err] = system_get_ticks();
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
