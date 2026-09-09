/**
 * @file    usb_proto.h
 * @brief   Binary USB protocol (host -> MCU): frame layout & CRC parameters
 *
 * Replaces the ASCII SLCAN receive path. Every host packet starts with a
 * two-byte header:
 *    AA AA  -> command frame
 *    AA AB  -> data frame (one CAN / CAN FD frame to transmit)
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


// ---- Frame headers ----
#define USB_PROTO_SOF1         0xAA
#define USB_PROTO_CMD_SOF2     0xAA   // command frame
#define USB_PROTO_DATA_SOF2    0xAB   // data frame


// ---- Command frames (AA AA) ----
#define USB_PROTO_CMD_LEN      5      // cmd id (1) + value (4)
#define USB_PROTO_CMD_ID_SET_BITRATE   1   // value = nominal bitrate in Hz


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
// Drop any partially received frame and resynchronise on the next header
void usb_proto_reset(void);

#endif /* _USB_PROTO_H */
