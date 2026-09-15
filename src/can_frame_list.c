/**
 * @file    can_frame_list.c
 * @brief   Frame-list cache (double buffer) implementation
 *
 * Storage: two fixed arrays of CAN_FRAME_LIST_MAX_FRAMES entries.
 *   - pending[] : filled by can_frame_list_pending_add() (host data frames,
 *                 main-loop / USB context)
 *   - active[]  : committed list, read by the TX pump ISR
 *
 * When pending is full, the oldest entry is dropped and the new frame is
 * appended (caller reports ERR_CAN_TXFAIL + drop count).
 *
 * Concurrency:
 *   - pending writes happen in the main loop (USB parser); commit() copies
 *     pending -> active under __disable_irq() so a concurrent pump ISR never
 *     sees a half-updated active list.
 *   - active is only read by the pump ISR (can_frame_list_active_count/get).
 */

#include <string.h>
#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "can.h"
#include "can_frame_list.h"


// Storage (single instance, module-private)
static can_frame_list_frame_t pending_list[CAN_FRAME_LIST_MAX_FRAMES];
static can_frame_list_frame_t active_list[CAN_FRAME_LIST_MAX_FRAMES];
static uint16_t pending_count = 0;
static uint16_t active_count = 0;


// Reset both buffers (bus enable/disable)
void can_frame_list_init(void)
{
    __disable_irq();
    pending_count = 0;
    active_count = 0;
    __enable_irq();
}


// Append one frame to the pending burst.
// Returns 0 on success, 1 if the oldest was dropped to make room, -1 invalid.
int32_t can_frame_list_pending_add(const FDCAN_TxHeaderType *header,
                                   const uint8_t *data)
{
    if (header == NULL)
        return -1;

    int8_t bytes = hal_dlc_code_to_bytes(header->DataLength);
    if (bytes < 0 || bytes > (int8_t)CAN_FRAME_LIST_MAX_DLC)
        return -1;

    int32_t dropped = 0;

    __disable_irq();

    if (pending_count >= CAN_FRAME_LIST_MAX_FRAMES)
    {
        // Overwrite oldest: shift left one slot, keep count at MAX.
        memmove(&pending_list[0], &pending_list[1],
                (size_t)(CAN_FRAME_LIST_MAX_FRAMES - 1u) *
                    sizeof(can_frame_list_frame_t));
        pending_count = (uint16_t)(CAN_FRAME_LIST_MAX_FRAMES - 1u);
        dropped = 1;
    }

    pending_list[pending_count].header = *header;
    memset(pending_list[pending_count].data, 0, CAN_FRAME_LIST_MAX_DLC);
    if (data != NULL && bytes > 0)
        memcpy(pending_list[pending_count].data, data, (size_t)bytes);

    pending_count++;

    __enable_irq();
    return dropped;
}


uint16_t can_frame_list_pending_count(void)
{
    return pending_count;
}


// Commit the pending burst as the new active list (replace). When the pending
// burst is empty (host sent a command without new data), the previous active
// list is kept so the last committed list is replayed again.
void can_frame_list_commit(void)
{
    if (pending_count == 0)
        return;   // nothing new -> keep the current active list

    __disable_irq();
    memcpy(active_list, pending_list,
           (size_t)pending_count * sizeof(can_frame_list_frame_t));
    active_count = pending_count;
    pending_count = 0;
    __enable_irq();
}


uint16_t can_frame_list_active_count(void)
{
    return active_count;
}


// Read-only access to one active entry (safe to call from the pump ISR).
const can_frame_list_frame_t *can_frame_list_active_get(uint16_t idx)
{
    if (idx >= active_count)
        return NULL;
    return &active_list[idx];
}
