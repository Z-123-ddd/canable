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


// Where USB data frames go (selected by a command / API; default single
// frame). List mode is identical to single-frame mode except that the host
// sends a burst of data frames which are stored FIFO and replayed.
typedef enum can_send_mode_ {
    CAN_SEND_MODE_SINGLE = 0,   // data frame -> 'most recent frame' cache
    CAN_SEND_MODE_FRAMELIST,    // data frames -> pending list (burst)
} can_send_mode_t;


// How a TX job is triggered (the trigger APIs in can.c; a future USB command
// will call these to start sending). In SINGLE mode the source is the cached
// 'most recent frame'; in FRAMELIST mode it is the committed list.
//   SINGLE     -> send the source once and stop.
//   CONTINUOUS -> periodic: keep sending on every timer tick (re-send the
//                 single frame, or step through the list FIFO and wrap;
//                 independent of USB traffic once started).
typedef enum can_tx_trigger_ {
    CAN_TX_TRIGGER_SINGLE = 0,
    CAN_TX_TRIGGER_CONTINUOUS,
} can_tx_trigger_t;


// Bus state
enum can_bus_state {
    OFF_BUS,
    ON_BUS
};


// ---- TX model ----
// USB data frames are handled by can_tx_data(): in single mode the frame is
// cached in RAM as the 'most recent frame'; in list mode each frame of a host
// burst is appended to the pending list. Nothing is transmitted directly - a
// trigger (can_tx_start SINGLE/CONTINUOUS) decides when the cached source goes
// on the bus. A periodic job keeps re-sending it (single frame, or list steps)
// on every timer tick, independent of USB traffic.


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

// Binary-protocol configuration
void can_set_id_format(can_id_format_t fmt);          // default: standard
can_id_format_t can_get_id_format(void);
void can_set_send_mode(can_send_mode_t mode);         // default: single frame
can_send_mode_t can_get_send_mode(void);

// Apply a nominal bitrate given in Hz. Returns 0 on success, -1 when the
// value is not supported. Restarts the bus when it is already open so the
// new rate takes effect immediately.
int32_t can_apply_bitrate_hz(uint32_t hz);

// Convert a raw DLC code (0..15, incl. CAN FD codes) to an FDCAN HAL DLC code
uint32_t can_dlc_code_to_hal_dlc(uint8_t dlc_code);

// Convert an FDCAN HAL DLC code to a payload byte count (0..64, -1 invalid)
int8_t hal_dlc_code_to_bytes(uint32_t hal_dlc_code);

uint32_t can_rx(FDCAN_RxHeaderType *rx_msg_header, uint8_t *rx_msg_data);
void can_rx_isr(void);        // FDCAN1_INT0 ISR: drain HW FIFO into software RX queue
void can_send_timer_tick(void);  // periodic timer ISR (see can.c state machine)
uint8_t is_can_msg_pending(uint8_t fifo);

// ---- Host data / TX job control (single vs continuous send) ----
// Data-frame entry from the binary USB protocol: store the frame as the
// 'most recent frame' in RAM. Nothing is transmitted directly - the trigger
// (can_tx_start) decides when it goes on the bus.
void can_tx_data(const FDCAN_TxHeaderType *hdr, const uint8_t *data);

// Start a send job on the cached frame.
//   SINGLE     -> send it once and stop.
//   CONTINUOUS -> keep re-sending it on every timer tick.
// Returns 0 on success, -1 when there is no cached frame to send.
int32_t can_tx_start(can_tx_trigger_t trigger);
// Abort any running TX job (stops the periodic pump).
void can_tx_stop(void);
// True while a periodic TX job is running.
uint8_t can_tx_job_running(void);

#endif // _CAN_H
