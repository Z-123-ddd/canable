/**
 * @file    usb_conf.h
 * @brief   USB Device configuration for CANable2-N32 (CDC ACM)
 *
 * Endpoint layout (default build, CDC_TX_USE_DBUF = 0):
 *   EP0: Control (bidirectional)
 *   EP1: Bulk OUT + IN   — CDC data, 64 bytes each way (0x01 / 0x81)
 *   EP2: Interrupt IN    — CDC notification (8 bytes)
 *
 * Optional build (CDC_TX_USE_DBUF = 1), see the notes further down:
 *   EP1: Bulk OUT only   — CDC data, host -> device
 *   EP3: Bulk IN         — CDC data, device -> host, hardware double buffered
 */

#ifndef __USB_CONF_H
#define __USB_CONF_H

/* Number of endpoints (excluding EP0) */
#define EP_NUM                      (4)

/* Uplink endpoint / double buffering.
 *   0 = EP1 IN (0x81), single buffered - host compatible, DEFAULT
 *   1 = EP3 IN (0x83), hardware double buffered - EXPERIMENTAL, see usb_conf.h
 *       notes above; a host that still reads 0x81 will not drain the uplink.
 * Requires a matching rebuild of the host tool (UsbLink.cpp EP_IN). */
#define CDC_TX_USE_DBUF             (0)

/* Buffer table base in USB packet memory (512 bytes total) */
#define BTABLE_ADDRESS              (0x00)

/* EP0 addresses (control: 64 bytes each direction) */
#define ENDP0_RXADDR                (0x40)
#define ENDP0_TXADDR                (0x80)

/* CDC data uplink (device -> host) endpoint.
 *
 * Single buffered (default): rides EP1 IN, so the wire layout stays exactly the
 * classic 0x01/0x81 pair and existing host software keeps working.  EP1 is
 * bidirectional again, which is only a problem for the experimental
 * double-buffered uplink: on this USB core double buffering consumes BOTH
 * halves of the endpoint buffer-table entry (ADDR_TX/COUNT_TX become buffer 0,
 * ADDR_RX/COUNT_RX become buffer 1), so a double-buffered endpoint cannot carry
 * both directions and needs its own number - EP3 IN, selected by
 * CDC_TX_USE_DBUF in usb_conf.h.
 *
 * The double-buffered path is disabled by default: on N32H473 the DTOG_RX
 * "give buffer 1 back" handshake is undocumented and the pipeline desyncs
 * after two or three packets (txq says 2 packets queued, STAT_TX reads NAK).
 * Dropping the handshake is worse (stalls after one packet).  See
 * CDC_TX_USE_DBUF for how to switch it back on. */
#define ENDP1_RXADDR                (0xC0)
#define ENDP1_TXADDR                (0x110)

/* EP2 address  — CDC Interrupt IN (8 bytes) */
#define ENDP2_TXADDR                (0x150)

/* Second PMA packet buffer, only used by the double-buffered EP3 uplink
 * (0x110 + 64 bytes = 0x150 is EP2, so this sits just past it). */
#define ENDP3_TXBUF0_ADDR           (0x110)
#define ENDP3_TXBUF1_ADDR           (0x160)

/* Interrupt mask: Correct Transfer | Reset | Wakeup */
#define IMR_MSK (CTRL_CTRSM | CTRL_WKUPM | CTRL_RSTM)

/* CDC class-specific request codes (not provided by N32 USB library) */
#define SET_LINE_CODING         0x20
#define GET_LINE_CODING         0x21
#define SET_CONTROL_LINE_STATE  0x22
#define SET_COMM_FEATURE        0x02

/* Endpoint callbacks live in usb_endp.c.  EP1 IN and EP3 IN/OUT all land on the
 * uplink service routine; in the default build EP3 carries no traffic so those
 * entries never fire, and in the double-buffered build EP1 IN is unused.  Both
 * directions of EP3 are wired because usbfsd_int.c routes CTR_RX to the OUT
 * table and CTR_TX to the IN table, and it is undocumented which one the
 * hardware raises for buffer 1 of a double-buffered IN endpoint. */
#define EP4_IN_Callback     USB_ProcessNop
#define EP5_IN_Callback     USB_ProcessNop
#define EP6_IN_Callback     USB_ProcessNop
#define EP7_IN_Callback     USB_ProcessNop

#define EP2_OUT_Callback    USB_ProcessNop
#define EP4_OUT_Callback    USB_ProcessNop
#define EP5_OUT_Callback    USB_ProcessNop
#define EP6_OUT_Callback    USB_ProcessNop
#define EP7_OUT_Callback    USB_ProcessNop

#endif /* __USB_CONF_H */
