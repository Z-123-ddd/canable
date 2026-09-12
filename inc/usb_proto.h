/**
 * @file    usb_proto.h
 * @brief   Binary USB protocol (host -> MCU): frame layout & CRC parameters
 *
 * Replaces the ASCII SLCAN receive path. Every host packet starts with a
 * two-byte header:
 *    AA AA  -> command frame (host -> MCU)
 *    AA AB  -> data frame (host -> MCU to transmit; also MCU -> host RX report)
 *    AA AC  -> response / event frame (MCU -> host, e.g. bitrate lock)
 *
 * Command frame payload (5 bytes):
 *    byte0: cmd id (1 = set nominal bitrate)
 *    byte1..4: 32-bit unsigned value, little-endian (value = bitrate in Hz)
 *
 * Data frame payload (max 72 bytes, per host spec):
 *    ID:       byte0..byte1 (standard, 11-bit) or byte0..byte3 (extended,
 *              29-bit) -- selected by the current CAN ID format config
 *    control:  byte4 -- CAN FD control-field style bits:
 *                bit7 FDF, bit6 BRS, bit5 RTR (classic remote req),
 *                bit4 reserved, bit3..0 DLC code (0..15)
 *    data:     bytes after control, length from the DLC code low nibble
 *              (0..64); classic frames limited to 8 bytes
 *    crc:      classic 2 bytes / FD 3 bytes (see CRC notes below)
 *
 * USB packet boundaries are ignored: the parser is a byte stream and a
 * frame may span several 64-byte USB packets.
 */

#ifndef _USB_PROTO_H
#define _USB_PROTO_H

#include <stdint.h>
#include "n32h47x_48x.h"
#include "error.h"


// ---- Frame headers ----
#define USB_PROTO_SOF1         0xAA
#define USB_PROTO_CMD_SOF2     0xAA   // command frame (host -> MCU)
#define USB_PROTO_DATA_SOF2    0xAB   // data frame (host -> MCU, and MCU -> host RX report)
#define USB_PROTO_RSP_SOF2     0xAC   // response / event frame (MCU -> host)
#define USB_PROTO_LOG_SOF2     0xAD   // warning/error frame (MCU -> host)


// ---- Command frames (AA AA) ----
#define USB_PROTO_CMD_LEN      5      // cmd id (1) + value (4)
#define USB_PROTO_CMD_ID_SET_BITRATE   1   // value = nominal bitrate in Hz

// TX control commands:
//   id 2: start sending the cached frame. value selects the trigger:
//           0 = single shot (send once; list mode replays the list once)
//           1 = continuous  (periodic resend on every timer tick)
//   id 3: stop a running send job.
//   id 4: query status. The MCU replies with one ASCII line built by
//         can_format_status() (bus/mode/job/timer/cur/pend/act/err).
//   id 5: enter DFU (system bootloader). The MCU switches the clock to
//         HSI and jumps into the factory bootloader (0x1FFF01A1); it does
//         NOT come back - the host can then download firmware over DFU.
#define USB_PROTO_CMD_ID_TX_START     2
#define USB_PROTO_CMD_ID_TX_STOP      3
#define USB_PROTO_CMD_ID_GET_STATUS  4
#define USB_PROTO_CMD_ID_ENTER_DFU   5
//   id 6: set the periodic send interval. value = interval in ms, allowed
//         range 1..1000; out-of-range values are refused (ERR_USB_BADFRAME).
#define USB_PROTO_CMD_ID_SET_INTERVAL 6
//   id 7: select the send mode. value = 0 single frame / 1 frame list;
//         other values are refused (ERR_USB_BADFRAME).
#define USB_PROTO_CMD_ID_SET_MODE     7
//   id 8: set the CAN FD data-phase bitrate. value = data bitrate in Hz
//         (2000000 / 5000000); other values are refused (ERR_USB_BADFRAME).
#define USB_PROTO_CMD_ID_SET_DATA_BITRATE 8

// Allowed range (ms) for the SET_INTERVAL command value.
#define USB_PROTO_TX_INTERVAL_MIN_MS  1U
#define USB_PROTO_TX_INTERVAL_MAX_MS  1000U

// SET_MODE command values.
#define USB_PROTO_MODE_SINGLE         0U
#define USB_PROTO_MODE_FRAMELIST      1U


// ---- Data frames (AA AB) ----
#define USB_PROTO_CTRL_FDF     0x80U  // CAN FD format
#define USB_PROTO_CTRL_BRS     0x40U  // bit-rate switch (FD only)
#define USB_PROTO_CTRL_RTR     0x20U  // remote frame (classic only)
#define USB_PROTO_CTRL_DLC_M   0x0FU  // DLC code (low nibble)

#define USB_PROTO_MAX_ID_BYTES 4U
#define USB_PROTO_MAX_DATA     64U
#define USB_PROTO_MAX_CRC      3U
#define USB_PROTO_MAX_PAYLOAD  (USB_PROTO_MAX_ID_BYTES + 1 + USB_PROTO_MAX_DATA + USB_PROTO_MAX_CRC)  // 72
#define USB_PROTO_MAX_FRAME    (2U + USB_PROTO_MAX_PAYLOAD)                                          // 74


// ---- CRC ----
// Transport checksum over ID + control + data bytes (in wire order).
// Bit widths follow the CAN / CAN FD definitions:
//   classic (FDF=0): CRC-15/CAN   (15 bits) -> 2 bytes, right aligned
//   FD data <= 16B : CRC-17/CAN-FD(17 bits) -> 3 bytes, right aligned
//   FD data >  16B : CRC-21/CAN-FD(21 bits) -> 3 bytes, right aligned
// All: MSB-first, init 0x00000000, xorout 0, no reflection, no bit
// stuffing (transport CRC, MUST match the host implementation).
#define USB_PROTO_CRC15_POLY   0x4599U     // x^15+x^14+x^10+x^8+x^7+x^4+x^3+1  (x^15 term omitted)
#define USB_PROTO_CRC17_POLY   0x1685BU    // CAN FD CRC-17 register constant (x^17 term omitted)
#define USB_PROTO_CRC21_POLY   0x102899U   // CAN FD CRC-21 register constant (x^21 term omitted)


// ---- API ----
// Feed one received byte (called from the CDC process loop)
void usb_proto_feed(uint8_t byte);

// Encode one RECEIVED CAN/FD frame (bus -> USB) into the SAME binary layout
// the host uses to send data:  AA AB | ID | CTRL | DATA | CRC.
//   ID    : 2 bytes (standard) or 4 bytes (extended), big-endian, taken from
//           the ACTUAL received frame type (not the host id-format config)
//   CTRL  : bit7 FDF, bit6 BRS(FD), bit5 RTR(classic), bits3..0 DLC code
//   CRC   : classic 2 bytes (CRC-15) / FD 3 bytes (CRC-17 or CRC-21)
// Returns the total frame length (>0), or -1 on error / buffer too small.
int32_t usb_proto_encode_rx(uint8_t *out, uint16_t max,
                            const FDCAN_RxHeaderType *hdr, const uint8_t *data);

// ---- Response / event frames (AA AC, MCU -> host) ----
//   id 1: bitrate lock. Payload = the nominal bitrate in Hz as a 4-byte
//         little-endian uint32. Sent once, when the auto-bitrate scan locks
//         onto a rate that actually receives frames (see can_scan_poll()).
#define USB_PROTO_RSP_ID_BITRATE_LOCK  1

// Emit the "bitrate locked" response: AA AC 01 | hz[4] LE.
void usb_proto_send_bitrate_lock(uint32_t hz);

// ---- Warning / error frames (AA AD, MCU -> host) ----
// Emit: AA AD | TEXT(ASCII error enum) | DA AA | CRC8.
// CRC-8 (poly 0x07, init 0) covers TEXT only; the AA AD and DA AA delimiters
// are excluded from the CRC.
void usb_proto_send_error(error_t err);

#endif /* _USB_PROTO_H */
