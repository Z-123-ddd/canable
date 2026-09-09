/**
 * @file    can.h
 * @brief   CAN/FDCAN interface for CANable2-N32
 */

#ifndef _CAN_H
#define _CAN_H

#include <stdint.h>
#include "n32h47x_48x.h"


// Classic CAN / CANFD nominal bitrates
enum can_bitrate {
    CAN_BITRATE_10K = 0,
    CAN_BITRATE_20K,
    CAN_BITRATE_50K,
    CAN_BITRATE_100K,
    CAN_BITRATE_125K,
    CAN_BITRATE_250K,
    CAN_BITRATE_500K,
    CAN_BITRATE_750K,
    CAN_BITRATE_1000K,
    CAN_BITRATE_5000K,      // CAN FD max nominal rate (5 Mbit/s)
    CAN_BITRATE_83_3K,
    CAN_BITRATE_INVALID,
};


// Nominal bitrate values in Hz, as macros (compared by can_apply_bitrate_hz)
#define CAN_BITRATE_HZ_10K      10000U
#define CAN_BITRATE_HZ_20K      20000U
#define CAN_BITRATE_HZ_50K      50000U
#define CAN_BITRATE_HZ_100K     100000U
#define CAN_BITRATE_HZ_125K     125000U
#define CAN_BITRATE_HZ_250K     250000U
#define CAN_BITRATE_HZ_500K     500000U
#define CAN_BITRATE_HZ_750K     750000U
#define CAN_BITRATE_HZ_1000K    1000000U
#define CAN_BITRATE_HZ_5000K    5000000U
#define CAN_BITRATE_HZ_83_3K    83333U


// CANFD data bitrates
enum can_data_bitrate {
    CAN_DATA_BITRATE_2M = 2,
    CAN_DATA_BITRATE_5M = 5,
    CAN_DATA_BITRATE_INVALID,
};


// Identifier width used by the binary USB data-frame protocol
typedef enum can_id_format_ {
    CAN_ID_FORMAT_STANDARD = 0,   // 11-bit ID, carried in 2 bytes
    CAN_ID_FORMAT_EXTENDED,       // 29-bit ID, carried in 4 bytes
} can_id_format_t;


// Where USB data frames go (selected by a future command frame; the global
// defaults to TX-ring send mode and the frame-list mode stores into the cache)
typedef enum can_send_mode_ {
    CAN_SEND_MODE_TXRING = 0,     // normal send: frames -> TX ring FIFO (can_tx)
    CAN_SEND_MODE_FRAMELIST,      // frame-list mode: frames -> can_frame_list cache
} can_send_mode_t;


// Bus state
enum can_bus_state {
    OFF_BUS,
    ON_BUS
};


// CAN transmit buffering (ring buffer; when full the OLDEST frame is overwritten)
#define TXQUEUE_LEN     64    // Number of buffers allocated
#define TXQUEUE_DATALEN 64    // CAN DLC length of data buffers (64 for CANFD)


// Cirbuf structure for CAN TX frames
//   producer: can_tx()  (USB command path, main-loop context)
//   consumer: can_send_timer_tick() (periodic timer ISR)
typedef struct cantxbuf_ {
    uint8_t data[TXQUEUE_LEN][TXQUEUE_DATALEN];
    FDCAN_TxHeaderType header[TXQUEUE_LEN];
    uint16_t head;   // next slot to write (producer)
    uint16_t tail;   // next slot to send  (consumer)
} can_txbuf_t;


// CAN receive buffering (ring buffer; when full the OLDEST frame is overwritten)
#define RXQUEUE_LEN     64    // Number of RX buffers allocated
#define RXQUEUE_DATALEN 64    // CAN DLC length of data buffers (64 for CANFD)


// Cirbuf structure for CAN RX frames
//   producer: can_rx_isr() (FDCAN ISR, may overwrite oldest on overflow)
//   consumer: can_rx()     (main loop)
typedef struct canrxbuf_ {
    uint8_t data[RXQUEUE_LEN][RXQUEUE_DATALEN];
    FDCAN_RxHeaderType header[RXQUEUE_LEN];
    volatile uint16_t head;   // written by ISR only
    volatile uint16_t tail;   // written by main loop (can_rx) and by ISR on overflow overwrite
} can_rxbuf_t;


// Prototypes
void can_init(void);
void can_enable(void);
void can_disable(void);
void can_set_bitrate(enum can_bitrate bitrate);
void can_set_data_bitrate(enum can_data_bitrate bitrate);
void can_set_silent(uint8_t silent);
void can_set_autoretransmit(uint8_t autoretransmit);

// Binary-protocol configuration (defaults: standard ID + TX-ring mode)
void can_set_id_format(can_id_format_t fmt);
can_id_format_t can_get_id_format(void);
void can_set_send_mode(can_send_mode_t mode);
can_send_mode_t can_get_send_mode(void);

// Apply a nominal bitrate given in Hz. Returns 0 on success, -1 when the
// value is not supported. Restarts the bus when it is already open so the
// new rate takes effect immediately.
int32_t can_apply_bitrate_hz(uint32_t hz);

// Convert a raw DLC code (0..15, incl. CAN FD codes) to an FDCAN HAL DLC code
uint32_t can_dlc_code_to_hal_dlc(uint8_t dlc_code);

// Convert an FDCAN HAL DLC code to a payload byte count (0..64, -1 invalid)
int8_t hal_dlc_code_to_bytes(uint32_t hal_dlc_code);

uint32_t can_tx(FDCAN_TxHeaderType *tx_msg_header, uint8_t *tx_msg_data);
uint32_t can_rx(FDCAN_RxHeaderType *rx_msg_header, uint8_t *rx_msg_data);
void can_rx_isr(void);        // FDCAN1_INT0 ISR: drain HW FIFO into software RX queue
void can_send_timer_tick(void);  // periodic timer ISR: send one queued frame (if any)
uint8_t is_can_msg_pending(uint8_t fifo);

#endif // _CAN_H
