/**
 * @file    can_frame_list.h
 * @brief   Frame-list cache (double buffer) for the "list mode" TX path
 *
 * List mode receives a burst of USB data frames from the host and replays
 * them (single FIFO pass, or cycled by the periodic timer), exactly like the
 * single-frame path except that the source is a list.
 *
 * Two buffers are kept:
 *   - pending : where incoming USB data frames accumulate (host writes)
 *   - active  : the committed list that the TX pump (timer ISR) replays
 *
 * A TX-start command "commits" the pending burst: if it is non-empty it
 * replaces the active list (copy under interrupts disabled), otherwise the
 * previous active list is kept (data unchanged -> command only).
 */

#ifndef _CAN_FRAME_LIST_H
#define _CAN_FRAME_LIST_H

#include <stdint.h>
#include "n32h47x_48x.h"

// List limits (host guarantees no more than 20 frames per burst)
#define CAN_FRAME_LIST_MAX_FRAMES   20U
#define CAN_FRAME_LIST_MAX_DLC      64U

// One list entry: full TX description + payload
typedef struct {
    FDCAN_TxHeaderType header;
    uint8_t data[CAN_FRAME_LIST_MAX_DLC];
} can_frame_list_frame_t;

// Reset both buffers (call on bus enable/disable)
void can_frame_list_init(void);

// ---- pending (host data frames accumulate here) ----
void    can_frame_list_pending_clear(void);          // start a new burst
int32_t can_frame_list_pending_add(const FDCAN_TxHeaderType *header,
                                   const uint8_t *data); // -1 if full/invalid
uint16_t can_frame_list_pending_count(void);

// ---- commit (TX-start command): pending -> active, replacing the old list ----
void can_frame_list_commit(void);

// ---- active (read by the TX pump ISR only) ----
uint16_t can_frame_list_active_count(void);
const can_frame_list_frame_t *can_frame_list_active_get(uint16_t idx); // NULL if oob

#endif /* _CAN_FRAME_LIST_H */
