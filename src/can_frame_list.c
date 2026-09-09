/**
 * @file    can_frame_list.c
 * @brief   Frame list cache implementation (CANable2-N32)
 *
 * Ordered, append-only-when-adding buffer of TX frames (max
 * CAN_FRAME_LIST_MAX_FRAMES entries, up to CAN_FRAME_LIST_MAX_DLC data
 * bytes each). Frames are stored in the order they were added; clearing
 * empties the whole list.
 *
 * Concurrency: add/clear are called from the command (main-loop) context
 * and are guarded with __disable_irq() so a future periodic-send ISR that
 * only READS via can_frame_list_count()/can_frame_list_get() stays safe.
 */

#include <string.h>
#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "can.h"
#include "can_frame_list.h"


// Frame list storage (single instance, module-private)
static can_frame_list_frame_t frame_list[CAN_FRAME_LIST_MAX_FRAMES];
static uint16_t frame_count = 0;


// Reset the list (same as clear; provided for explicit one-time setup)
void can_frame_list_init(void)
{
    can_frame_list_clear();
}


// Append a frame at the end of the list.
// Returns 0 on success, -1 if the list is full or the arguments are invalid.
int32_t can_frame_list_add(const FDCAN_TxHeaderType *header, const uint8_t *data)
{
    if (header == NULL)
        return -1;

    // Validate the DLC fits the fixed 64-byte payload
    int8_t bytes = hal_dlc_code_to_bytes(header->DataLength);
    if (bytes < 0 || bytes > (int8_t)CAN_FRAME_LIST_MAX_DLC)
        return -1;

    __disable_irq();

    if (frame_count >= CAN_FRAME_LIST_MAX_FRAMES)
    {
        __enable_irq();
        return -1;   // list full
    }

    frame_list[frame_count].header = *header;
    memset(frame_list[frame_count].data, 0, CAN_FRAME_LIST_MAX_DLC);
    if (data != NULL && bytes > 0)
        memcpy(frame_list[frame_count].data, data, (size_t)bytes);

    frame_count++;

    __enable_irq();
    return 0;
}


// Remove all frames from the list
void can_frame_list_clear(void)
{
    __disable_irq();
    frame_count = 0;
    __enable_irq();
}


// Current number of valid frames in the list
uint16_t can_frame_list_count(void)
{
    return frame_count;
}


// Read-only access to one entry (safe to call from an ISR).
// Returns NULL when idx is out of range.
const can_frame_list_frame_t *can_frame_list_get(uint16_t idx)
{
    if (idx >= frame_count)
        return NULL;
    return &frame_list[idx];
}
