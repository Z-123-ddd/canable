/**
 * @file    main.c
 * @brief   CANable2 firmware main entry point (N32H473 port)
 *
 * USB-to-CAN bridge firmware (binary USB protocol).
 *   - Receives binary command/data frames via USB CDC
 *   - Parses and transmits CAN/CANFD frames
 *   - Receives CAN/CANFD frames and reports them as binary frames via USB
 *
 * Scheduling note: never drain the CAN RX soft-queue in an unbounded loop.
 * A peer that floods heartbeats before the host opens the port used to keep
 * while(is_can_msg_pending) busy forever, starving cdc_process() so GET_STATUS
 * / open never ran — the module looked "frozen" after the host finally connected.
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "system.h"
#include "can.h"
#include "can_timer.h"
#include "usb_proto.h"
#include "led.h"
#include "error.h"
#include "usbd_cdc_if.h"
#include "dfu.h"
#include "rs485.h"
#include "workmode.h"

// Bound work per main-loop turn so USB command handling stays responsive.
#define MAIN_ERR_BURST   2
#define MAIN_RX_BURST    8


int main(void)
{
    // If a DFU jump was requested (host command 5 set the cookie then reset
    // the chip), jump into the bootloader NOW - before any peripheral/clock
    // init - so its USB DFU enumerates from a clean reset state.
    dfu_bootloader_if_pending();

    // Initialize peripherals
    system_init();
    can_init();
    can_timer_init();
    led_init();
    usb_init();
    rs485_init(115200);   // RS485 (USART1 PA9/PA10 + DE PC6), DMA TX/RX

    // Power-on blink sequence
    led_blue_blink(2);

    // Start FDCAN1 and enable its RX/bus-off interrupts by default
    // (auto-open on boot; no need to send 'O' first. 'C' still closes it.)
    can_enable();

    // Storage for CAN RX message
    FDCAN_RxHeaderType rx_msg_header;
    uint8_t rx_msg_data[64] = {0};
    uint8_t msg_buf[USB_PROTO_MAX_FRAME];

    while (1)
    {
        led_process();

        // Report a few pending errors (AA AD); do not drain the whole queue
        // in one turn — that used to refill USB TX while no host was reading.
        {
            int pending_err;
            int nerr = 0;
            while (nerr < MAIN_ERR_BURST &&
                   (pending_err = error_take_pending()) >= 0)
            {
                usb_proto_send_error((error_t)pending_err);
                nerr++;
            }
        }

        // USB first every turn: drain IN backlog + parse host commands
        // (GET_STATUS / open / bitrate) even when the CAN bus is flooding.
        cdc_process();

        // CAN-only tasks run only in CAN mode; in RS485 mode the FDCAN
        // controller/interrupts/timer are stopped and the RS485 sits in its
        // default (receive) state.
        if (workmode_get() != WORK_MODE_CAN)
            continue;

        // Auto bitrate scan: cycle candidate rates while the bus shows error
        // activity but no frame is being received; on lock, report it to the
        // host as an AA AC response before the frames below.
        can_scan_poll();
        int32_t lock_hz = can_scan_take_lock();
        if (lock_hz >= 0)
            usb_proto_send_bitrate_lock((uint32_t)lock_hz);

        // Unsolicited TEC/REC (AA AC id=3) when either counter is non-zero
        {
            uint8_t tec = 0, rec = 0;
            if (can_err_cnt_poll(&tec, &rec))
                usb_proto_send_err_cnt(tec, rec);
        }

        // Push at most MAIN_RX_BURST frames to USB per turn. If the USB TX
        // ring is nearly full (host not reading yet, or still catching up),
        // stop and leave frames in the soft RX ring — ISR overwrite keeps
        // the newest traffic until the host can accept more.
        for (int i = 0; i < MAIN_RX_BURST; i++)
        {
            if (!is_can_msg_pending(FDCAN_RX_FIFO0))
                break;
            if (cdc_tx_free() < (uint32_t)USB_PROTO_MAX_FRAME)
                break;

            if (can_rx(&rx_msg_header, rx_msg_data) != SUCCESS)
                break;

            int32_t msg_len = usb_proto_encode_rx(msg_buf, sizeof(msg_buf),
                                                  &rx_msg_header, rx_msg_data);
            if (msg_len > 0)
                cdc_transmit(msg_buf, (uint16_t)msg_len);
        }
    }
}
