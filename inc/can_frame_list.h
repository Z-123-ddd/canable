/**
 * @file    can_frame_list.h
 * @brief   Frame list cache for the host "frame list mode" (CANable2-N32)
 *
 * Ordered storage for up to CAN_FRAME_LIST_MAX_FRAMES TX frames, each
 * described by a full FDCAN_TxHeaderType plus up to
 * CAN_FRAME_LIST_MAX_DLC data bytes. Supports classic CAN and CAN FD.
 *
 * This buffer is DISTINCT from the TX ring buffer used for one-shot sends
 * (can_tx() / txqueue in can.c, which stays a FIFO). The frame list is
 * meant to serve a host-side frame-list mode, i.e. periodic looped
 * transmission of a list of frames.
 *
 * NOTE: only the storage + management API lives here for now. The USB
 * command/protocol wiring and the periodic send loop are added later.
 */

#ifndef _CAN_FRAME_LIST_H
#define _CAN_FRAME_LIST_H

#include <stdint.h>
#include "n32h47x_48x.h"


// Frame list limits
#define CAN_FRAME_LIST_MAX_FRAMES   20U   // Max number of frames in the list
#define CAN_FRAME_LIST_MAX_DLC      64U   // Max data bytes per frame (CAN FD)


// One entry of the frame list: full TX description + payload
typedef struct {
    FDCAN_TxHeaderType header;              // ID, frame type (classic/FD), BRS, DLC, ...
    uint8_t data[CAN_FRAME_LIST_MAX_DLC];   // payload (valid bytes = DLC of header)
} can_frame_list_frame_t;


// Prototypes
void  can_frame_list_init(void);                       // reset the list (empty)
int32_t can_frame_list_add(const FDCAN_TxHeaderType *header,
                           const uint8_t *data);       // append at end; -1 if full/invalid
void  can_frame_list_clear(void);                      // remove all frames
uint16_t can_frame_list_count(void);                   // number of valid frames
const can_frame_list_frame_t *can_frame_list_get(uint16_t idx);  // NULL if out of range

#endif /* _CAN_FRAME_LIST_H */
