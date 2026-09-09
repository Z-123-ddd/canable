/**
 * @file    slcan.h
 * @brief   SLCAN TX framing (CAN RX -> ASCII to host) for CANable2-N32
 */

#ifndef _SLCAN_H
#define _SLCAN_H

#include <stdint.h>
#include "n32h47x_48x.h"

// Maximum message length
#define SLCAN_MTU        138 + 1 + 16  // canfd 64 frame plus \r plus padding
#define SLCAN_STD_ID_LEN 3
#define SLCAN_EXT_ID_LEN 8

// Prototypes
// Format a received CAN frame as an ASCII slcan message (TX to host).
// Returns the message length (>0) or -1 on error.
int32_t slcan_parse_frame(uint8_t *buf, FDCAN_RxHeaderType *frame_header, uint8_t *frame_data);

#endif // _SLCAN_H
