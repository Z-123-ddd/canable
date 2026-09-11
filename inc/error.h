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
    ERR_FRAMELIST_FULL,   // list-mode burst longer than CAN_FRAME_LIST_MAX_FRAMES

    ERR_MAX
} error_t;


// Prototypes
void error_assert(error_t err);
uint32_t error_last_timestamp(void);
uint32_t error_reg(void);

// Pop one pending error report queued by error_assert() (for the main loop to
// send an AA AD warning frame). Returns -1 when there is none.
int error_take_pending(void);

#endif /* INC_ERROR_H_ */
