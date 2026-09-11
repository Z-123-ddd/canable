/**
 * @file    main.c
 * @brief   CANable2 firmware main entry point (N32H473 port)
 *
 * USB-to-CAN bridge firmware (binary USB protocol).
 *   - Receives binary command/data frames via USB CDC
 *   - Parses and transmits CAN/CANFD frames
 *   - Receives CAN/CANFD frames and reports them as binary frames via USB
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

// Firmware identification (kept for future reporting commands)
char* fw_id = GIT_VERSION " " GIT_REMOTE "\r";


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

        // Report any newly-asserted errors to the host as AA AD warning frames.
        {
            int pending_err;
            while ((pending_err = error_take_pending()) >= 0)
                usb_proto_send_error((error_t)pending_err);
        }

        cdc_process();

        // Auto bitrate scan: cycle candidate rates while the bus shows error
        // activity but no frame is being received; on lock, report it to the
        // host as an AA AC response before the frames below.
        can_scan_poll();
        int32_t lock_hz = can_scan_take_lock();
        if (lock_hz >= 0)
            usb_proto_send_bitrate_lock((uint32_t)lock_hz);

        // Check for received CAN messages (software RX queue filled by FDCAN ISR)
        while (is_can_msg_pending(FDCAN_RX_FIFO0))
        {
            // Read CAN frame from software RX queue
            if (can_rx(&rx_msg_header, rx_msg_data) == SUCCESS)
            {
                // Encode frame into the binary RX report format (AA AB | ID | CTRL | DATA | CRC)
                int32_t msg_len = usb_proto_encode_rx(msg_buf, sizeof(msg_buf), &rx_msg_header, rx_msg_data);

                // Transmit via USB CDC
                if (msg_len > 0)
                {
                    cdc_transmit(msg_buf, msg_len);
                }
            }
            else
            {
                break;
            }
        }
    }
}
