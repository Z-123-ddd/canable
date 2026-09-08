/**
 * @file    interrupts.c
 * @brief   Interrupt Service Routines for CANable2-N32
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "interrupts.h"
#include "led.h"
#include "can.h"
#include "can_timer.h"

// N32 USB interrupt service routine (application-level, defined in usb_istr.c)
extern void USB_Istr(void);


// System exception handlers

void NMI_Handler(void)
{
    while (1);
}

void HardFault_Handler(void)
{
    while (1)
    {
        // Fault indicator
    }
}

void MemManage_Handler(void)
{
    while (1);
}

void BusFault_Handler(void)
{
    while (1);
}

void UsageFault_Handler(void)
{
    while (1);
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}


// USB Full-Speed Low Priority interrupt
void USB_FS_LP_IRQHandler(void)
{
    USB_Istr();
}


// USB Full-Speed Wakeup interrupt
void USB_FS_WKUP_IRQHandler(void)
{
    // Handle USB wakeup from suspend via EXTI Line 18
    if (EXTI_GetITStatus(EXTI_LINE18) != RESET)
    {
        EXTI_ClrITPendBit(EXTI_LINE18);
    }
}


// FDCAN1 interrupt line 0 (enabled by can_enable())
void FDCAN1_INT0_IRQHandler(void)
{
    // Rx FIFO 0 received a new message -> drain HW FIFO into software RX queue
    if (FDCAN_GetIntFlag(FDCAN1, FDCAN_FLAG_RX_FIFO0_NEW_MESSAGE) == SET)
    {
        FDCAN_ClearFlag(FDCAN1, FDCAN_FLAG_RX_FIFO0_NEW_MESSAGE);
        can_rx_isr();
    }

    // Bus-Off detected -> restart the CAN controller automatically
    if (FDCAN_GetIntFlag(FDCAN1, FDCAN_FLAG_BUS_OFF) == SET)
    {
        FDCAN_ClearFlag(FDCAN1, FDCAN_FLAG_BUS_OFF);
        FDCAN_Start(FDCAN1);
    }
}


// Periodic send timer (GTIM1, configured in can_timer_init()). Always runs and
// interrupts at the configured interval (default 1ms); the enable switch
// (default OFF) decides whether each tick triggers a CAN send.
void GTIM1_IRQHandler(void)
{
    TIM_ClrIntPendingBit(GTIM1, TIM_INT_UPDATE);

    if (!can_timer_is_enabled())
        return;

    // Send one queued frame from the TX ring (no-op if empty / bus off)
    can_send_timer_tick();
}
