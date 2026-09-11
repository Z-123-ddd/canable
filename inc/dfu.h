/**
 * @file    dfu.h
 * @brief   DFU (system bootloader) entry for CANable2-N32
 *
 * The N32H47x factory bootloader lives in system memory (0x1FFFxxxx) and
 * provides USB DFU download. It does NOT enumerate USB reliably when jumped
 * to from a fully-initialised application (USB/CAN/clocks/NVIC left set) -
 * the reference (ODrive) resets the chip first and jumps from the clean
 * reset state. So we do the same:
 *
 *   host cmd 5  ->  dfu_enter_bootloader(): set a magic cookie in RAM, then
 *                   NVIC_SystemReset().
 *   next boot   ->  dfu_bootloader_if_pending() at the very TOP of main():
 *                   if the cookie is set, jump into the bootloader.
 *
 * The cookie lives in RAM above the application image (not zeroed by the
 * C runtime) so it survives the soft reset.
 *
 * The host sees the device re-enumerate as a USB DFU device, vendor
 * "Nationstech DFU" VID/PID 0x19F5:0x4537 (NOT ST's 0x0483:0xDF11).
 */

#ifndef _DFU_H
#define _DFU_H

// Request the DFU bootloader: store a cookie and reset the chip. Returns no
// meaningful value - the reset happens inside (never returns).
void dfu_enter_bootloader(void);

// If a DFU-bootloader jump is pending (cookie set by dfu_enter_bootloader
// before the reset), perform the jump now. Call this at the VERY START of
// main(), before any peripheral/clock init, so the bootloader starts from a
// clean reset state. Returns normally if no jump is pending.
void dfu_bootloader_if_pending(void);

#endif /* _DFU_H */
