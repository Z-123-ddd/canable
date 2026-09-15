/**
 * @file    error.h
 * @brief   Error handling for CANable2-N32
 */

#ifndef INC_ERROR_H_
#define INC_ERROR_H_

#include <stdint.h>

// Error flags, value is bit position in the error register
typedef enum _error_t
{
    ERR_PERIPHINIT = 0,
    ERR_USBTX_BUSY,
    ERR_CAN_TXFAIL,
    ERR_CANRXFIFO_OVERFLOW,
    ERR_FULLBUF_CANTX,
    ERR_FULLBUF_USBRX,
    ERR_FULLBUF_USBTX,
    ERR_USB_BADFRAME,     // malformed binary USB frame / unsupported command
    ERR_USB_CRC,          // binary USB frame CRC mismatch
    ERR_FRAMELIST_FULL,   // legacy: list overflow now uses ERR_CAN_TXFAIL + drop
    ERR_CAN_ACK,          // CAN TX: no ACK (no other node on the bus)
    ERR_CAN_BUSOFF,       // severe protocol error caused Bus-Off (non-ACK)

    ERR_MAX
} error_t;


// Prototypes
void error_assert(error_t err);
uint32_t error_last_timestamp(void);
uint32_t error_reg(void);

// Pop one pending error type marked by error_assert() (for the main loop to
// send an AA AD warning frame). Same type coalesces while pending. Returns -1
// when there is none.
int error_take_pending(void);

#endif /* INC_ERROR_H_ */
