/**
 * @file    usb_proto.c
 * @brief   Binary USB protocol (host -> MCU) byte-stream parser
 *
 * Replaces the SLCAN ASCII receive parser. A small state machine consumes
 * one byte at a time from the CDC receive path; frames may span USB packet
 * boundaries. On a malformed frame the parser drops it, records an error
 * and resynchronises on the next valid header.
 *
 * Command frame (AA AA ...):    5-byte payload handled immediately.
 * Data frame    (AA AB ...):    parsed into an FDCAN_TxHeaderType and sent
 *                               either to the TX ring FIFO (normal send) or
 *                               to the frame-list cache (list mode), selected
 *                               by the current CAN send-mode config.
 */

#include <string.h>
#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "usb_proto.h"
#include "can.h"
#include "can_timer.h"
#include "error.h"
#include "usbd_cdc_if.h"


// Parser states
typedef enum {
    PROTO_IDLE,         // waiting for the first header byte (0xAA)
    PROTO_SOF1,         // got 0xAA, waiting for the 2nd header byte
    PROTO_CMD,          // collecting a command payload
    PROTO_DATA_ID,      // collecting the CAN ID bytes
    PROTO_DATA_CTRL,    // waiting for the control byte
    PROTO_DATA_BODY,    // collecting data bytes
    PROTO_DATA_CRC,     // collecting CRC bytes
} proto_state_t;


// Module state
static proto_state_t state = PROTO_IDLE;
static uint8_t rxbuf[USB_PROTO_MAX_PAYLOAD];   // payload collected so far
static uint16_t rxlen;                          // bytes stored in rxbuf
static uint16_t need;                           // payload bytes still expected
static uint8_t id_bytes;                        // 2 or 4 (current ID config)
static uint8_t ctrl_byte;                       // data-frame control byte
static uint8_t data_bytes;                      // CAN payload length
static uint8_t crc_bytes;                       // 2 (classic) or 3 (FD)


// ---------------------------------------------------------------------------
// CRC engines (bit widths 15/17/21), MSB-first, init 0, xorout 0.
// poly is the register constant with the leading x^N term omitted.
// ---------------------------------------------------------------------------
static uint32_t crc_msb_width(const uint8_t *p, uint16_t len, uint8_t width, uint32_t poly)
{
    uint32_t crc = 0;
    const uint32_t top = 1UL << (width - 1);
    const uint8_t shift = (uint8_t)(width - 8);

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= (uint32_t)p[i] << shift;
        for (uint8_t b = 0; b < 8; b++)
        {
            if (crc & top)
                crc = (crc << 1) ^ poly;
            else
                crc <<= 1;
        }
    }
    if (width == 32)
        return crc;
    return crc & ((1UL << width) - 1);
}


// CRC over the wire bytes [ID | control | data], contiguous in rxbuf
static uint32_t crc_compute(uint16_t id_n, uint8_t ctrl)
{
    uint16_t n = (uint16_t)(id_n + 1 + data_bytes);

    // Select the CRC width by frame type / payload length (CAN FD rules)
    if (ctrl & USB_PROTO_CTRL_FDF)
        return (data_bytes <= 16U)
                   ? crc_msb_width(rxbuf, n, 17, USB_PROTO_CRC17_POLY)
                   : crc_msb_width(rxbuf, n, 21, USB_PROTO_CRC21_POLY);
    return crc_msb_width(rxbuf, n, 15, USB_PROTO_CRC15_POLY);
}


// ---------------------------------------------------------------------------
// Frame dispatch
// ---------------------------------------------------------------------------

// Handle a complete command frame: rxbuf[0..4]
static void proto_cmd_dispatch(void)
{
    uint8_t cmd = rxbuf[0];
    uint32_t value = (uint32_t)rxbuf[1]
                   | ((uint32_t)rxbuf[2] << 8)
                   | ((uint32_t)rxbuf[3] << 16)
                   | ((uint32_t)rxbuf[4] << 24);

    switch (cmd)
    {
        case USB_PROTO_CMD_ID_SET_BITRATE:
            if (can_apply_bitrate_hz(value) != 0)
                error_assert(ERR_USB_BADFRAME);   // unsupported rate
            break;

        case USB_PROTO_CMD_ID_TX_START:
        {
            can_tx_trigger_t trig = (value & 1u)
                                        ? CAN_TX_TRIGGER_CONTINUOUS
                                        : CAN_TX_TRIGGER_SINGLE;
            if (can_tx_start(trig) == 0)
            {
                // Continuous mode, and any list-mode job (even a single
                // pass), is driven by the periodic timer pump -> enable it.
                if (trig == CAN_TX_TRIGGER_CONTINUOUS ||
                    can_get_send_mode() == CAN_SEND_MODE_FRAMELIST)
                {
                    can_timer_enable();
                }
            }
            else
            {
                error_assert(ERR_USB_BADFRAME);   // nothing cached to send
            }
            break;
        }

        case USB_PROTO_CMD_ID_TX_STOP:
            can_tx_stop();        // also disables the periodic timer
            can_timer_disable();  // belt & braces
            break;

        case USB_PROTO_CMD_ID_GET_STATUS:
        {
            char st[64];
            uint16_t n = can_format_status(st, (uint16_t)sizeof(st));
            if (n > 0)
                cdc_transmit((uint8_t *)st, n);
            break;
        }

        default:
            error_assert(ERR_USB_BADFRAME);
            break;
    }
}


// Handle a complete data frame: rxbuf = [ID..][ctrl][data..][crc..]
static void proto_data_dispatch(void)
{
    const uint8_t *idp = &rxbuf[0];
    const uint8_t *datap = &rxbuf[id_bytes + 1];
    const uint8_t *crcp = datap + data_bytes;

    // 1. Verify CRC (covers ID + control + data)
    uint32_t expect = crc_compute(id_bytes, ctrl_byte);
    uint32_t got = 0;
    for (uint8_t i = 0; i < crc_bytes; i++)
        got = (got << 8) | crcp[i];
    if (got != expect)
    {
        error_assert(ERR_USB_CRC);
        return;
    }

    // 2. Build the FDCAN TX header
    FDCAN_TxHeaderType hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.TxFrameType = ((ctrl_byte & USB_PROTO_CTRL_RTR) && !(ctrl_byte & USB_PROTO_CTRL_FDF))
                          ? FDCAN_REMOTE_FRAME
                          : FDCAN_DATA_FRAME;
    hdr.FDFormat = (ctrl_byte & USB_PROTO_CTRL_FDF) ? FDCAN_FD_CAN : FDCAN_CLASSIC_CAN;
    hdr.IdType = (id_bytes == 4) ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID;
    hdr.BitRateSwitch = (ctrl_byte & USB_PROTO_CTRL_BRS) ? FDCAN_BRS_ON : FDCAN_BRS_OFF;
    hdr.ErrorState = FDCAN_ESI_ACTIVE;
    hdr.TxEventFifo = FDCAN_NO_TX_EVENTS;
    hdr.MsgMarker = 0;

    if (id_bytes == 4)
        hdr.ID = ((uint32_t)idp[0] << 24) | ((uint32_t)idp[1] << 16)
               | ((uint32_t)idp[2] << 8) | (uint32_t)idp[3];
    else
        hdr.ID = ((uint32_t)idp[0] << 8) | (uint32_t)idp[1];

    hdr.DataLength = can_dlc_code_to_hal_dlc(ctrl_byte & USB_PROTO_CTRL_DLC_M);

    // 3. Hand the frame to the TX engine (caches it in RAM; transmission is
    //    triggered by a later TX command via can_tx_start).
    can_tx_data(&hdr, datap);
}


// ---------------------------------------------------------------------------
// Byte-stream parser
// ---------------------------------------------------------------------------

void usb_proto_feed(uint8_t byte)
{
    switch (state)
    {
        case PROTO_IDLE:
            if (byte == USB_PROTO_SOF1)
                state = PROTO_SOF1;
            break;

        case PROTO_SOF1:
            if (byte == USB_PROTO_CMD_SOF2)
            {
                state = PROTO_CMD;
                rxlen = 0;
                need = USB_PROTO_CMD_LEN;
            }
            else if (byte == USB_PROTO_DATA_SOF2)
            {
                state = PROTO_DATA_ID;
                rxlen = 0;
                id_bytes = (can_get_id_format() == CAN_ID_FORMAT_EXTENDED) ? 4U : 2U;
                need = id_bytes;
            }
            else
            {
                // Bad header: drop it. If this byte could start a new frame
                // (0xAA) keep waiting for its 2nd byte, else go idle.
                state = (byte == USB_PROTO_SOF1) ? PROTO_SOF1 : PROTO_IDLE;
            }
            break;

        case PROTO_CMD:
            rxbuf[rxlen++] = byte;
            if (rxlen >= USB_PROTO_CMD_LEN)
            {
                proto_cmd_dispatch();
                state = PROTO_IDLE;
            }
            break;

        case PROTO_DATA_ID:
            rxbuf[rxlen++] = byte;
            if (rxlen >= id_bytes)
            {
                state = PROTO_DATA_CTRL;
                need = 1;
            }
            break;

        case PROTO_DATA_CTRL:
            ctrl_byte = byte;
            rxbuf[rxlen++] = byte;

            // Sanity check the DLC before committing to a length
            {
                uint8_t dlc = ctrl_byte & USB_PROTO_CTRL_DLC_M;
                int8_t dl = hal_dlc_code_to_bytes(can_dlc_code_to_hal_dlc(dlc));
                if (dl < 0 || dl > (int8_t)USB_PROTO_MAX_DATA)
                {
                    error_assert(ERR_USB_BADFRAME);
                    state = PROTO_IDLE;
                    break;
                }

                if (ctrl_byte & USB_PROTO_CTRL_FDF)
                {
                    crc_bytes = 3;
                }
                else
                {
                    if (dlc > 8U)
                    {
                        error_assert(ERR_USB_BADFRAME);  // classic with FD DLC
                        state = PROTO_IDLE;
                        break;
                    }
                    crc_bytes = 2;
                }

                // Remote frames carry no data bytes
                if ((ctrl_byte & USB_PROTO_CTRL_RTR) && !(ctrl_byte & USB_PROTO_CTRL_FDF))
                    data_bytes = 0;
                else
                    data_bytes = (uint8_t)dl;

                if (data_bytes > 0)
                {
                    state = PROTO_DATA_BODY;
                    need = data_bytes;
                }
                else
                {
                    state = PROTO_DATA_CRC;
                    need = crc_bytes;
                }
            }
            break;

        case PROTO_DATA_BODY:
            rxbuf[rxlen++] = byte;
            if (--need == 0)
            {
                state = PROTO_DATA_CRC;
                need = crc_bytes;
            }
            break;

        case PROTO_DATA_CRC:
            rxbuf[rxlen++] = byte;
            if (--need == 0)
            {
                proto_data_dispatch();
                state = PROTO_IDLE;
            }
            break;

        default:
            state = PROTO_IDLE;
            break;
    }
}


void usb_proto_reset(void)
{
    state = PROTO_IDLE;
    rxlen = 0;
    need = 0;
}
