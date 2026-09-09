/**
 * @file    slcan.c
 * @brief   SLCAN framing: format CAN RX frames as ASCII for the USB host
 *
 * TX direction only. The host->MCU ASCII command parser was replaced by the
 * binary usb_proto protocol (see usb_proto.c); incoming CAN frames are still
 * reported to the host in slcan ASCII via slcan_parse_frame().
 *
 * Adapted for N32H473. The N32 FDCAN header types use slightly different
 * field names than STM32 (e.g., .ID vs .Identifier, .FDFormat values same).
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "can.h"
#include "error.h"
#include "slcan.h"
#include "system.h"
#include "usbd_cdc_if.h"


// Private method prototypes
static uint8_t __hal_dlc_code_to_std_dlc_code(uint32_t hal_dlc_code);


// Parse an incoming CAN frame into an outgoing slcan message
int32_t slcan_parse_frame(uint8_t *buf, FDCAN_RxHeaderType *frame_header, uint8_t *frame_data)
{
    // Clear buffer
    for (uint8_t j = 0; j < SLCAN_MTU; j++)
        buf[j] = '\0';

    // Start building the slcan message string at idx 0 in buf[]
    uint8_t msg_idx = 0;

    // Handle classic CAN frames
    if (frame_header->FDFormat == FDCAN_CLASSIC_CAN)
    {
        // Add character for frame type
        if (frame_header->RxFrameType == FDCAN_DATA_FRAME)
        {
            buf[msg_idx] = 't';
        }
        else if (frame_header->RxFrameType == FDCAN_REMOTE_FRAME)
        {
            buf[msg_idx] = 'r';
        }
    }
    // Handle FD CAN frames
    else
    {
        // FD doesn't support remote frames so this must be a data frame

        // Frame with BRS enabled
        if (frame_header->BitRateSwitch == FDCAN_BRS_ON)
        {
            buf[msg_idx] = 'b';
        }
        // Frame with BRS disabled
        else
        {
            buf[msg_idx] = 'd';
        }
    }

    // Assume standard identifier
    uint8_t id_len = SLCAN_STD_ID_LEN;
    uint32_t tmp = frame_header->ID;

    // Check if extended
    if (frame_header->IdType == FDCAN_EXTENDED_ID)
    {
        // Convert first char to upper case for extended frame
        buf[msg_idx] -= 32;
        id_len = SLCAN_EXT_ID_LEN;
    }
    msg_idx++;

    // Add identifier to buffer (hex nibbles)
    for (uint8_t j = id_len; j > 0; j--)
    {
        buf[j] = (tmp & 0xF);
        tmp = tmp >> 4;
        msg_idx++;
    }

    // Add DLC to buffer
    buf[msg_idx++] = __hal_dlc_code_to_std_dlc_code(frame_header->DataLength);
    int8_t bytes = hal_dlc_code_to_bytes(frame_header->DataLength);

    // Check bytes value
    if (bytes < 0) return -1;
    if (bytes > 64) return -1;

    // Add data bytes (hex encoded)
    for (uint8_t j = 0; j < bytes; j++)
    {
        buf[msg_idx++] = (frame_data[j] >> 4);
        buf[msg_idx++] = (frame_data[j] & 0x0F);
    }

    // Convert to ASCII (2nd character to end)
    for (uint8_t j = 1; j < msg_idx; j++)
    {
        if (buf[j] < 0xA) {
            buf[j] += 0x30;
        } else {
            buf[j] += 0x37;
        }
    }

    // Add carriage return
    buf[msg_idx++] = '\r';

    return msg_idx;
}




// Convert N32 HAL DLC code to raw nibble value (0-15)
static uint8_t __hal_dlc_code_to_std_dlc_code(uint32_t hal_dlc_code)
{
    return hal_dlc_code >> 16;
}
