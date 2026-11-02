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
 * Data frame payload (max 69 bytes):
 *    AA AB | CTRL(1) | ID(2 or 4, BE) | DATA(0..64)
 *    CTRL bits:
 *      bit7 FDF, bit6 BRS, bit5 RTR, bit4 IDE (1=extended 4B ID / 0=standard 2B),
 *      bit3..0 DLC code (0..15)
 *
 * No application CRC on data frames: USB bulk transfers already carry a
 * per-packet CRC16 with automatic retry, so the link is lossless and the
 * checksum only wasted 2 (classic) / 3 (FD) bytes on every CAN report.
 * The rare AA AC / AA AD diagnostic frames keep their CRC-8.
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
//   id 9: switch the debugger work mode. value = 0 CAN (default) / 1 RS485;
//         other values are refused (ERR_USB_BADFRAME). Switching to RS485 stops
//         the CAN state machine / interrupts / timer and brings RS485 up in the
//         receive state; switching back to CAN releases RS485 and restarts FDCAN.
#define USB_PROTO_CMD_ID_SET_WORK_MODE 9
//   id 10: query the firmware version. The MCU replies with an ASCII string
//          (GIT_VERSION): AA AC 02 | LEN(1) | TEXT(LEN).
#define USB_PROTO_CMD_ID_GET_VERSION  10
//   id 11: pause/resume reception. value = 1 pause / 0 resume; other
//          values are refused (ERR_USB_BADFRAME). While paused the CAN
//          transmit job/timer and the FDCAN reception interrupts stop, and
//          the software RX queue is flushed so no partially/fully received
//          frame survives into the resumed session.
#define USB_PROTO_CMD_ID_SET_PAUSE    11

//   id 12: clear the firmware's sticky error register and drop queued AA AD
//          reports. No value. Useful to reset err= after a transient fault.
#define USB_PROTO_CMD_ID_CLEAR_ERRORS 12

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
#define USB_PROTO_CTRL_IDE     0x10U  // 1 = extended ID (4B) / 0 = standard (2B)
#define USB_PROTO_CTRL_DLC_M   0x0FU  // DLC code (low nibble)

#define USB_PROTO_MAX_ID_BYTES 4U
#define USB_PROTO_MAX_DATA     64U
#define USB_PROTO_MAX_CRC      0U   // AA AB data frames carry no application CRC
#define USB_PROTO_MAX_PAYLOAD  (USB_PROTO_MAX_ID_BYTES + 1 + USB_PROTO_MAX_DATA)   // 69
#define USB_PROTO_MAX_FRAME    (2U + USB_PROTO_MAX_PAYLOAD)                        // 71


// ---- CRC ----
// Transport checksum over CTRL + ID + data bytes (in wire order).
// Bit widths follow the CAN / CAN FD definitions:
//   classic (FDF=0): CRC-15/CAN   (15 bits) -> 2 bytes, right aligned
//   FD data <= 16B : CRC-17/CAN-FD(17 bits) -> 3 bytes, right aligned
//   FD data >  16B : CRC-21/CAN-FD(21 bits) -> 3 bytes, right aligned
// All: MSB-first, init 0x00000000, xorout 0, no reflection, no bit
// stuffing (transport CRC, MUST match the host implementation).
// (The CAN CRC15/17/21 polynomials and the AA AB frame CRC were removed; only
//  the CRC-8 used by AA AC / AA AD diagnostic frames remains.)


// ---- API ----
// Feed one received byte (called from the CDC process loop)
void usb_proto_feed(uint8_t byte);

// Encode one RECEIVED CAN/FD frame (bus -> USB):
//   AA AB | CTRL | ID(2/4) | DATA | CRC
//   CTRL.IDE selects ID width; CRC covers CTRL+ID+DATA.
// Returns the total frame length (>0), or -1 on error / buffer too small.
int32_t usb_proto_encode_rx(uint8_t *out, uint16_t max,
                            const FDCAN_RxHeaderType *hdr, const uint8_t *data);

// ---- Response / event frames (AA AC, MCU -> host) ----
//   id 1: bitrate lock. Payload = the nominal bitrate in Hz as a 4-byte
//         little-endian uint32. Sent once, when the auto-bitrate scan locks
//         onto a rate that actually receives frames (see can_scan_poll()).
#define USB_PROTO_RSP_ID_BITRATE_LOCK  1
//   id 2: firmware version. Payload = 1-byte length + ASCII (GIT_VERSION).
#define USB_PROTO_RSP_ID_VERSION       2
//   id 3: cumulative error-frame counts (unsolicited).
//         Payload = TEC_FRAMES(4 LE) | REC_FRAMES(4 LE) | CRC8(1).
//         CRC-8 (poly 0x07, init 0) covers the 8 counter bytes.
//         MCU accumulates hw TEC/REC increases ÷8 into uint32 (saturates).
#define USB_PROTO_RSP_ID_ERR_CNT       3

// Emit the "bitrate locked" response: AA AC 01 | hz[4] LE.
void usb_proto_send_bitrate_lock(uint32_t hz);

// Emit the firmware-version response: AA AC 02 | LEN | TEXT(ASCII).
void usb_proto_send_version(const char *ver);

// Emit cumulative error-frame counts: AA AC 03 | TEC_u32 LE | REC_u32 LE | CRC8.
void usb_proto_send_err_cnt(uint32_t tec_frames, uint32_t rec_frames);

// ---- Warning / error frames (AA AD, MCU -> host) ----
// Emit: AA AD | TEXT(ASCII error enum) | DA AA | CRC8.
// CRC-8 (poly 0x07, init 0) covers TEXT only; the AA AD and DA AA delimiters
// are excluded from the CRC.
void usb_proto_send_error(error_t err);

#endif /* _USB_PROTO_H */
