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
#include "workmode.h"
#include "dfu.h"
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


// Encode a received CAN/FD frame (bus -> USB) into the same binary layout the
// host uses to send data: AA AB | ID | CTRL | DATA | CRC. The ID field width
// follows the ACTUAL received frame type (standard 2B / extended 4B) so the
// report is independent of the host's send-side id-format config.
int32_t usb_proto_encode_rx(uint8_t *out, uint16_t max,
                            const FDCAN_RxHeaderType *hdr, const uint8_t *data)
{
    if (out == NULL || hdr == NULL)
        return -1;

    uint8_t fd     = (hdr->FDFormat == FDCAN_FD_CAN) ? 1u : 0u;
    uint8_t ext    = (hdr->IdType == FDCAN_EXTENDED_ID) ? 1u : 0u;
    uint8_t remote = (!fd && hdr->RxFrameType == FDCAN_REMOTE_FRAME) ? 1u : 0u;
    uint8_t id_bytes = ext ? 4u : 2u;

    int8_t nbytes = hal_dlc_code_to_bytes(hdr->DataLength);
    if (nbytes < 0)
        return -1;
    if (remote)
        nbytes = 0;   // classic remote frames carry no data bytes

    // Raw DLC nibble (0..15): the HAL DLC code keeps it in the high nibble.
    uint8_t dlc = (uint8_t)((hdr->DataLength >> 16) & USB_PROTO_CTRL_DLC_M);

    uint8_t ctrl = 0;
    if (fd)                                             ctrl |= USB_PROTO_CTRL_FDF;
    if (fd && hdr->BitRateSwitch == FDCAN_BRS_ON)       ctrl |= USB_PROTO_CTRL_BRS;
    if (remote)                                         ctrl |= USB_PROTO_CTRL_RTR;
    ctrl |= dlc;

    uint8_t crc_bytes = fd ? 3u : 2u;
    uint16_t total = (uint16_t)(2u + id_bytes + 1u + (uint16_t)nbytes + crc_bytes);
    if (total > max)
        return -1;

    uint16_t i = 0;
    out[i++] = USB_PROTO_SOF1;       // 0xAA
    out[i++] = USB_PROTO_DATA_SOF2;  // 0xAB

    // ID, big-endian
    if (ext) {
        out[i++] = (uint8_t)(hdr->ID >> 24);
        out[i++] = (uint8_t)(hdr->ID >> 16);
    }
    out[i++] = (uint8_t)(hdr->ID >> 8);
    out[i++] = (uint8_t)(hdr->ID);

    out[i++] = ctrl;

    for (int8_t j = 0; j < nbytes; j++)
        out[i++] = data[j];

    // Transport CRC over ID + CTRL + DATA (skip the 2-byte header)
    uint16_t cov = (uint16_t)(id_bytes + 1u + (uint16_t)nbytes);
    uint32_t crc;
    if (fd)
        crc = (nbytes <= 16)
                  ? crc_msb_width(&out[2], cov, 17, USB_PROTO_CRC17_POLY)
                  : crc_msb_width(&out[2], cov, 21, USB_PROTO_CRC21_POLY);
    else
        crc = crc_msb_width(&out[2], cov, 15, USB_PROTO_CRC15_POLY);

    if (fd) {
        out[i++] = (uint8_t)(crc >> 16);
        out[i++] = (uint8_t)(crc >> 8);
        out[i++] = (uint8_t)(crc);
    } else {
        out[i++] = (uint8_t)(crc >> 8);
        out[i++] = (uint8_t)(crc);
    }
    return (int32_t)i;
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

        case USB_PROTO_CMD_ID_SET_DATA_BITRATE:
            if (can_apply_data_bitrate_hz(value) != 0)
                error_assert(ERR_USB_BADFRAME);   // unsupported data rate
            break;

        case USB_PROTO_CMD_ID_SET_WORK_MODE:
            // value = 0 CAN / 1 RS485. Other values are refused.
            if (value == WORK_MODE_CAN || value == WORK_MODE_RS485)
                workmode_set((uint8_t)value);
            else
                error_assert(ERR_USB_BADFRAME);
            break;

        case USB_PROTO_CMD_ID_SET_INTERVAL:
            // value = desired periodic send interval in ms (1..1000). Reject
            // out-of-range values like an unsupported bitrate; the timer
            // reload takes effect on the next tick and does not disturb any
            // queued frame or running job.
            if (value < USB_PROTO_TX_INTERVAL_MIN_MS ||
                value > USB_PROTO_TX_INTERVAL_MAX_MS)
                error_assert(ERR_USB_BADFRAME);
            else
                can_timer_set_period_ms(value);
            break;

        case USB_PROTO_CMD_ID_SET_MODE:
            // value = 0 single frame / 1 frame list. Anything else is refused.
            if (value == USB_PROTO_MODE_SINGLE)
                can_set_send_mode(CAN_SEND_MODE_SINGLE);
            else if (value == USB_PROTO_MODE_FRAMELIST)
                can_set_send_mode(CAN_SEND_MODE_FRAMELIST);
            else
                error_assert(ERR_USB_BADFRAME);
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

        case USB_PROTO_CMD_ID_ENTER_DFU:
            // Stop any TX job first, then jump to the bootloader.
            can_tx_stop();
            can_timer_disable();
            dfu_enter_bootloader();   // never returns on success
            break;

        case USB_PROTO_CMD_ID_GET_VERSION:
            // Reply with the ASCII firmware version string.
            usb_proto_send_version(GIT_VERSION);
            break;

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


// Emit the "bitrate locked" response frame (MCU -> host): AA AC 01 | hz[4] LE.
// Called (indirectly, via the main loop) when the auto-bitrate scan locks.
void usb_proto_send_bitrate_lock(uint32_t hz)
{
    uint8_t b[7];
    b[0] = USB_PROTO_SOF1;
    b[1] = USB_PROTO_RSP_SOF2;
    b[2] = USB_PROTO_RSP_ID_BITRATE_LOCK;
    b[3] = (uint8_t)(hz);
    b[4] = (uint8_t)(hz >> 8);
    b[5] = (uint8_t)(hz >> 16);
    b[6] = (uint8_t)(hz >> 24);
    cdc_transmit(b, (uint16_t)sizeof(b));
}


// Emit the firmware-version response (MCU -> host): AA AC 02 | LEN | TEXT(ASCII).
void usb_proto_send_version(const char *ver)
{
    if (ver == 0) ver = "unknown";
    uint16_t n = (uint16_t)strlen(ver);
    if (n > 60) n = 60;                 // bound to the frame buffer
    uint8_t frame[4 + 60];
    frame[0] = USB_PROTO_SOF1;          // 0xAA
    frame[1] = USB_PROTO_RSP_SOF2;      // 0xAC
    frame[2] = USB_PROTO_RSP_ID_VERSION;// 0x02
    frame[3] = (uint8_t)n;              // length
    for (uint16_t i = 0; i < n; i++)
        frame[4 + i] = (uint8_t)ver[i];
    cdc_transmit(frame, (uint16_t)(4 + n));
}


// ---- Warning/error (AA AD) frames -----------------------------------------
// ASCII names, index-aligned with error_t (see inc/error.h).
static const char *const k_err_names[ERR_MAX] =
{
    "ERR_PERIPHINIT",
    "ERR_USBTX_BUSY",
    "ERR_CAN_TXFAIL",
    "ERR_CANRXFIFO_OVERFLOW",
    "ERR_FULLBUF_CANTX",
    "ERR_FULLBUF_USBRX",
    "ERR_FULLBUF_USBTX",
    "ERR_USB_BADFRAME",
    "ERR_USB_CRC",
    "ERR_FRAMELIST_FULL",
    "ERR_CAN_ACK",
    "ERR_CAN_BUSOFF",
};

// CRC-8, poly 0x07, init 0, MSB-first (matches the host implementation).
static uint8_t crc8_byte(const uint8_t *p, uint16_t len)
{
    uint8_t c = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        c ^= p[i];
        for (uint8_t b = 0; b < 8; b++)
            c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
    }
    return c;
}

// Emit an AA AD warning/error frame: AA AD | TEXT(ASCII) | DA AA | CRC8.
// The CRC covers TEXT only (the AA AD and DA AA delimiters are excluded).
void usb_proto_send_error(error_t err)
{
    if ((uint32_t)err >= (uint32_t)ERR_MAX)
        return;

    const char *name = k_err_names[err];
    uint8_t text[64];
    uint16_t tlen = 0;
    while (name[tlen] != 0 && tlen < (uint16_t)sizeof(text))
    {
        text[tlen] = (uint8_t)name[tlen];
        tlen++;
    }

    uint8_t frame[2 + sizeof(text) + 2 + 1];
    uint16_t k = 0;
    frame[k++] = USB_PROTO_SOF1;              // 0xAA
    frame[k++] = USB_PROTO_LOG_SOF2;          // 0xAD
    for (uint16_t i = 0; i < tlen; i++)
        frame[k++] = text[i];
    frame[k++] = 0xDA;                        // trailer
    frame[k++] = 0xAA;
    frame[k++] = crc8_byte(text, tlen);       // CRC over TEXT only
    cdc_transmit(frame, k);
}
