/**
 * @file    workmode.c
 * @brief   Debugger work-mode switch (CAN <-> RS485) implementation
 *
 * See workmode.h. Switching is driven by USB command 9 (SET_WORK_MODE).
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "workmode.h"
#include "can.h"
#include "can_timer.h"
#include "rs485.h"

/* RS485 line settings used when entering RS485 mode. */
#define WORKMODE_RS485_BAUD   115200u

/* Default = CAN (matches main() boot sequence, which enables FDCAN at boot). */
static volatile uint8_t g_work_mode = WORK_MODE_CAN;


uint8_t workmode_get(void)
{
    return g_work_mode;
}


void workmode_set(uint8_t mode)
{
    if (mode == g_work_mode)
        return;

    if (mode == WORK_MODE_RS485)
    {
        /* --- stop all CAN activities --- */
        can_tx_stop();          /* abort any running TX job            */
        can_timer_disable();    /* stop the periodic TX pump (GTIM1)   */
        can_disable();          /* FDCAN stop/deinit + interrupts off  */

        /* --- bring up RS485, default = receive (DE low) --- */
        rs485_init(WORKMODE_RS485_BAUD);

        g_work_mode = WORK_MODE_RS485;
    }
    else /* WORK_MODE_CAN */
    {
        /* --- release the RS485 (USART1 + DMA) --- */
        rs485_stop();

        /* --- (re)start the CAN controller + interrupts --- */
        can_enable();

        g_work_mode = WORK_MODE_CAN;
    }
}
