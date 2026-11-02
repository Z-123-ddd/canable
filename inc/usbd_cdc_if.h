/**
 * @file    usbd_cdc_if.h
 * @brief   USB CDC interface for CANable2-N32
 */

#ifndef __USBD_CDC_IF_H__
#define __USBD_CDC_IF_H__

#include <stdint.h>

// CDC data max packet size
#define CDC_DATA_FS_MAX_PACKET_SIZE   64

// RX buffering
#define NUM_RX_BUFS        8
#define RX_BUF_SIZE        CDC_DATA_FS_MAX_PACKET_SIZE

// ---- Uplink (device -> host) packet queue ----
//
// One cdc_transmit() call is chopped into 1..N USB packets and pushed as that
// many queue slots.  Packets belonging to different calls are NEVER merged, so
// every CAN/FD report starts on its own packet boundary: a frame that does not
// fit in 64 bytes (an extended-ID FD frame with a long payload) simply spans
// two packets, it is never packed together with the next frame.
#define USBTX_PKT_SIZE     CDC_DATA_FS_MAX_PACKET_SIZE

// Slots must be large enough that one frame per packet does not cost buffering:
// a classic 8-byte frame occupies a whole 64-byte slot, so 192 slots hold 191
// queued frames (the old byte-oriented 2 KB ring held ~136) and 95 max-size
// FD frames (the old ring held 27).  A shallow queue shows up as
// ERR_FULLBUF_USBTX plus ERR_CANRXFIFO_OVERFLOW as soon as the CAN bus has
// real traffic, because the main loop stops draining the CAN ring while the
// uplink is congested.
#define USBTX_PKT_NUM      192
#define USBTXQUEUE_LEN     (USBTX_PKT_NUM * USBTX_PKT_SIZE)

// Uplink endpoint + double buffering are selected by CDC_TX_USE_DBUF in
// usb_conf.h (it has to be visible to the descriptor and endpoint init too).

// Debug counters (dbg_* in usbd_cdc_if.c), readable over SWD.  0 in a release
// build: the counters cost 24 bytes of RAM and a few cycles per packet.
#define CDC_TX_DEBUG       0

// PMA buffer (0 or 1) the core transmits first after a bus reset, i.e. the
// phase of DTOG_TX.  Only meaningful with CDC_TX_USE_DBUF=1; flip to 1 if the
// double-buffered uplink stays silent.
#define CDC_TX_FIRST_BUF   0

// Transmit buffering: ring of ready-to-send USB packets
typedef struct usbtx_ring_ {
    uint8_t  data[USBTX_PKT_NUM][USBTX_PKT_SIZE];
    uint8_t  len[USBTX_PKT_NUM];
    uint32_t head;
    uint32_t tail;
} usbtx_ring_t;

// Receive buffering: circular buffer FIFO
typedef struct _usbrx_buf_ {
    uint8_t buf[NUM_RX_BUFS][RX_BUF_SIZE];
    uint32_t msglen[NUM_RX_BUFS];
    uint32_t head;
    uint32_t tail;
} usbrx_buf_t;

// USB initialization
void usb_init(void);

// CDC transmit function (all-or-nothing; asserts ERR_FULLBUF_USBTX on fail)
void cdc_transmit(uint8_t *buf, uint16_t len);

// Free bytes in the USB TX ring (one slot kept empty).
uint32_t cdc_tx_free(void);
// Discard up to nbytes of the oldest queued TX data to make room (e.g. so a
// GET_STATUS reply is not starved behind a backlog of CAN RX reports).
void cdc_tx_drop_oldest(uint32_t nbytes);

// CDC process function (call in main loop)
void cdc_process(void);

// Drop the software TX pipeline state (called on USB bus reset).
void cdc_tx_reset(void);

// Uplink packet finished on the wire: refill PMA and re-arm. Call from the
// endpoint 3 correct-transfer interrupt only.
void cdc_tx_service(void);

// Has a host completed USB enumeration (selected a configuration) yet?
//
// 0 means no host has opened a data pipe at all - the boot case, where the CAN
// bus can already be streaming while the device is still enumerating.
uint8_t usb_host_available(void);

// Is the uplink backed up, i.e. the host is not draining what we queued?
//
// Judged purely from queue state (every PMA buffer handed to the core AND
// packets still waiting to be loaded), never from a timeout.  A timer was tried
// first and it misfired: under a full 8k frames/s bus the host legitimately
// goes quiet for >20 ms now and then, which silently dropped ~6% of all frames
// while still reporting err=0.  Only consult this when a frame is about to be
// discarded, so normal traffic can never trip it.
uint8_t usb_tx_saturated(void);

// CDC endpoint OUT callback (called from USB ISR)
void CDC_EP1_OUT_Callback(void);

#endif
