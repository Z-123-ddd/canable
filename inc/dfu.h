/**
 * @file    dfu.h
 * @brief   DFU (system bootloader) entry for CANable2-N32
 *
 * Ported from the ODrive N32 firmware (proj1, MotorControl/main.cpp).
 * The N32H47x factory bootloader lives in the system-memory area
 * (0x1FFFxxxx) and provides DFU download over USB. To enter it the
 * application must rebuild a small vector table in SRAM (so interrupts
 * during the bootloader still land in the boot code), switch the system
 * clock to HSI, disable the IWDG and finally jump to the boot entry.
 *
 * The host triggers this through the binary USB command frame:
 *   AA AA 05 00 00 00 00   -> enter DFU (firmware does not come back).
 */

#ifndef _DFU_H
#define _DFU_H

// Switch to the DFU bootloader. Never returns on success.
void dfu_enter_bootloader(void);

#endif /* _DFU_H */
