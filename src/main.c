/**
 * @file    main.c
 * @brief   CANable2 firmware main entry point (N32H473 port)
 *
 * USB-to-CAN bridge firmware implementing slcan protocol.
 *   - Receives slcan ASCII commands via USB CDC
 *   - Parses and transmits CAN/CANFD frames
 *   - Receives CAN/CANFD frames and sends them as slcan ASCII via USB
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "system.h"
#include "can.h"
#include "can_timer.h"
#include "slcan.h"
#include "led.h"
#include "error.h"
#include "printf.h"
#include "usbd_cdc_if.h"

// Firmware identification (kept for future reporting commands)
char* fw_id = GIT_VERSION " " GIT_REMOTE "\r";


int main(void)
{
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
    uint8_t msg_buf[SLCAN_MTU];

    while (1)
    {
        led_process();
        cdc_process();

        // Check for received CAN messages (software RX queue filled by FDCAN ISR)
        while (is_can_msg_pending(FDCAN_RX_FIFO0))
        {
            // Read CAN frame from software RX queue
            if (can_rx(&rx_msg_header, rx_msg_data) == SUCCESS)
            {
                // Parse frame into SLCAN ASCII format
                int32_t msg_len = slcan_parse_frame(msg_buf, &rx_msg_header, rx_msg_data);

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
