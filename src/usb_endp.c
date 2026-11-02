/**
 * @file    usb_endp.c
 * @brief   USB Endpoint callbacks for CANable2-N32 (CDC ACM)
 *
 * Called from the N32 USB interrupt handler (USB_Istr -> USB_CorrectTransferLp).
 * These handle data transfer on the CDC bulk endpoints.
 *
 * Endpoint map:
 *   EP1 OUT (0x01) — data host -> device
 *   EP2 IN  (0x82) — serial state notification
 *   EP1 IN  (0x81) — data device -> host (default, single buffered)
 *   EP3 IN  (0x83) — same uplink, double buffered build only (CDC_TX_USE_DBUF)
 */

#include "usbfsd_lib.h"
#include "usb_desc.h"
#include "usbfsd_mem.h"
#include "usbd_cdc_if.h"

/* External: CDC receive callback in usbd_cdc_if.c */
extern void CDC_EP1_OUT_Callback(void);


/**
 * @brief  EP1 IN callback - one uplink packet left the wire (default build).
 *
 * The next packet is written into the freed PMA buffer here, so it is already
 * waiting when the host issues the following IN token: no main-loop latency in
 * the path.  In the double-buffered build the uplink lives on EP3, so this
 * never fires (EP1 then has no IN direction at all).
 */
void EP1_IN_Callback(void)
{
    cdc_tx_service();
}


/**
 * @brief  EP1 OUT callback — called when bulk OUT data arrives from host.
 *         EP1 OUT is used for CDC data host->device.
 */
void EP1_OUT_Callback(void)
{
    CDC_EP1_OUT_Callback();
}


/**
 * @brief  EP2 IN callback — called when interrupt IN transfer completes.
 *         EP2 IN is used for CDC serial state notifications.
 */
void EP2_IN_Callback(void)
{
    SetEPTxStatus(ENDP2, EP_TX_NAK);
}


/**
 * @brief  EP3 IN callback — one uplink packet finished on the wire.
 *
 * This is the whole point of the double-buffered uplink: the next packet is
 * written into the freed PMA buffer here, so it is already waiting when the
 * host issues the following IN token.  No main-loop latency in the path.
 */
void EP3_IN_Callback(void)
{
    cdc_tx_service();
}


/**
 * @brief  EP3 OUT callback.
 *
 * EP3 is unidirectional (a double-buffered endpoint uses both buffer-table
 * halves for one direction), so no OUT data can arrive.  It is still wired up
 * because usbfsd_int.c dispatches CTR_RX to the OUT table and CTR_TX to the IN
 * table, and the reference documentation does not say which of the two the
 * hardware raises for the second buffer of a double-buffered IN endpoint.
 * Both paths converge on the same service routine, so either mapping works.
 */
void EP3_OUT_Callback(void)
{
    cdc_tx_service();
}
