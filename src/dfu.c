/**
 * @file    dfu.c
 * @brief   DFU (system bootloader) entry implementation (N32H473)
 *
 * Ported from the ODrive N32 firmware (proj1, MotorControl/main.cpp).
 *
 * The N32H47x factory bootloader is located in the system-memory area and
 * supports DFU download over USB. Jumping to it from the running application
 * requires:
 *   1. disabling the flash i-cache,
 *   2. probing the bootloader version (Get_NVR in system memory),
 *   3. building a 128-word vector table at the TOP of SRAM that redirects the
 *      interrupts used by the bootloader (SysTick / USART3 / USB FS) into the
 *      boot code (0x1FFF2xxx),
 *   4. disabling interrupts, re-arming the IWDG and switching the system
 *      clock back to HSI (the bootloader starts from reset-state clocks),
 *   5. setting the stack pointer / VTOR to the boot image and jumping.
 *
 * SRAM top depends on the part's density:
 *   ODrive reference board: 144 KB (112 KB SRAM + 32 KB CCM) -> 0x20023E00
 *   THIS board  N32H473CE : 192 KB (160 KB SRAM + 32 KB CCM) -> 0x2002FE00
 * The CCM bank sits contiguously above the main SRAM (Nations DFP: IRAM1 =
 * 0x20000000 length 0x30000 for N32H473CE), and 0x20023C00 is well below this
 * part's SRAM top, so both install points are valid RAM here. The address the
 * boot ROM actually reads is not documented per density, so an identical table
 * is installed at every candidate top and VTOR aims at the largest.
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "dfu.h"

/* ---- Constants from the N32 system bootloader (same chip family) ---- */
#define SRAM_VECTOR_WORD_SIZE   (128)

/* Candidate SRAM tops (address = top - 0x200), highest last. */
static const uint32_t SRAM_VECTOR_ADDRS[] = {
    0x20023E00u,   /* top of 144 KB (112 KB SRAM + 32 KB CCM, ODrive reference) */
    0x2002FE00u,   /* top of 192 KB (160 KB SRAM + 32 KB CCM, N32H473CE)      */
};
#define SRAM_VECTOR_COUNT \
    (sizeof(SRAM_VECTOR_ADDRS) / sizeof(SRAM_VECTOR_ADDRS[0]))

/* VTOR handed to the boot ROM: the largest candidate table. */
static const uint32_t SRAM_VECTOR_TARGET = 0x2002FE00u;

/* --- DFU request cookie (survives a soft reset; lives above the app image) ---
 * The app linker uses only 0x20000000..0x20020000 (128KB); this chip has RAM up
 * to 0x20030000, so 0x2002FC00 is free and is NOT zeroed by the C runtime.
 * 0x2002FC00 < vector table start 0x2002FE00, so it does not clash either. */
#define DFU_COOKIE_ADDR   ((volatile uint32_t *)0x2002FC00u)
#define DFU_COOKIE_MAGIC  0x44465521u   /* 'D','F','U','!' */

static uint32_t BootAddr  = 0x1FFF01A1u;        /* bootloader entry */
static uint32_t SPAddr    = 0x200024A8u;        /* boot stack (V1.0)  */

typedef enum {
    CMD_CR_SUCCESS  = 0x00,
    CMD_CR_FAILED   = 0x01,
} CMD_RETURN_CR;

/* Read function exported by the system bootloader (used to probe its version) */
static CMD_RETURN_CR (*Get_NVR)(uint32_t addr, uint8_t *data, uint32_t len) =
    (CMD_RETURN_CR (*)(uint32_t, uint8_t *, uint32_t))0x1FFF02A5u;

typedef void (*pFunction)(void);


/**
 * @brief  Reset the RCC and switch the system clock to HSI (8MHz).
 *         The bootloader expects to start from a clean reset-state clock.
 */
static ErrorStatus dfu_set_sysclock_to_hsi(void)
{
    uint32_t timeout_value = 0xFFFFFFFFu;
    ErrorStatus clock_status;

    /* Reset RCC to its default state */
    RCC_DeInit();

    /* Enable HSI and wait until it is stable */
    RCC_EnableHsi(ENABLE);
    clock_status = RCC_WaitHsiStable();
    if (clock_status != SUCCESS)
        return ERROR;

    /* Flash: prefetch on, zero wait states (8MHz is very slow) */
    FLASH_PrefetchBufSet(FLASH_PrefetchBuf_EN);
    FLASH_SetLatency(FLASH_LATENCY_0);

    /* HCLK = SYSCLK ; PCLK2 = HCLK ; PCLK1 = HCLK */
    RCC_ConfigHclk(RCC_SYSCLK_DIV1);
    RCC_ConfigPclk2(RCC_HCLK_DIV1);
    RCC_ConfigPclk1(RCC_HCLK_DIV1);

    /* Restore the PLL pre-divider default (as after reset) */
    RCC->PLLCTRL |= 0x00040000u;

    /* Select HSI as the system clock source */
    RCC_ConfigSysclk(RCC_SYSCLK_SRC_HSI);

    /* Wait until HSI is confirmed as SYSCLK */
    while (RCC_GetSysclkSrc() != RCC_CFG_SCLKSTS_HSI)
    {
        if ((timeout_value--) == 0u)
            return ERROR;
    }
    return SUCCESS;
}


/**
 * @brief  Install the 128-word bootloader vector table at one SRAM address.
 *         Redirects SysTick / USART3 / USB FS into the boot code.
 */
static void dfu_install_vector_table(uint32_t addr,
                                     uint32_t systick, uint32_t usart3, uint32_t usbfs)
{
    uint32_t i;
    uint32_t *pVec = (uint32_t *)addr;

    for (i = 0; i < SRAM_VECTOR_WORD_SIZE; i++)
        pVec[i] = 0;

    pVec[SysTick_IRQn + 16]   = systick;
    pVec[USART3_IRQn + 16]    = usart3;
    pVec[USB_FS_LP_IRQn + 16] = usbfs;
}


/**
 * @brief  Jump into the N32 DFU bootloader (never returns on success).
 */
static void dfu_jump_to_boot(void)
{
    uint8_t value1[8] = {0x00};
    uint8_t value2 = 8;
    uint32_t i;
    uint32_t systick, usart3, usbfs;

    /* Disable / reset the flash instruction cache */
    FLASH_iCacheCmd(FLASH_iCache_DIS);
    FLASH_iCacheRST();

    /* Probe the bootloader version */
    if (Get_NVR != NULL)
        Get_NVR(0x1FFF37A8u, value1, value2);

    if ((value1[0] & 0xFFu) == 0x11u)   /* BOOT V1.1 */
    {
        SPAddr  = 0x200024C0u;
        systick = 0x1FFF2F15u;
        usart3  = 0x1FFF2F95u;
        usbfs   = 0x1FFF3095u;
    }
    else                              /* BOOT V1.0 */
    {
        systick = 0x1FFF2D31u;
        usart3  = 0x1FFF2DB1u;
        usbfs   = 0x1FFF2EB1u;
    }

    /* Install the vector table at every candidate SRAM top */
    for (i = 0; i < SRAM_VECTOR_COUNT; i++)
        dfu_install_vector_table(SRAM_VECTOR_ADDRS[i], systick, usart3, usbfs);

    /* Disable all interrupts */
    __disable_irq();

    /* Re-arm and freeze the IWDG so it can not reset us during the jump */
    IWDG_ReloadKey();
    IWDG_WriteConfig(IWDG_WRITE_DISABLE);
    IWDG_SetPrescalerDiv(IWDG_PRESCALER_DIV256);

    /* Back to a reset-state clock (HSI) */
    dfu_set_sysclock_to_hsi();

    /* Entry point of the bootloader */
    pFunction jump_boot = (pFunction)BootAddr;

    /* Initialise the stack pointer */
    __set_MSP(SPAddr);

    /* Enable interrupts again (boot code expects them on) */
    __enable_irq();

    /* Point the vector table at the SRAM copy and jump */
    SCB->VTOR = SRAM_VECTOR_TARGET;
    jump_boot();
}


/**
 * @brief  Public API used by the ENTER_DFU command: mark a pending DFU jump
 *         and reset the chip. The actual jump runs from the clean reset state
 *         in main() (dfu_bootloader_if_pending). Never returns.
 */
void dfu_enter_bootloader(void)
{
    *DFU_COOKIE_ADDR = DFU_COOKIE_MAGIC;
    /* Ensure the cookie write reaches SRAM before the reset. */
    __DSB();
    __disable_irq();
    NVIC_SystemReset();
    /* Not reached. */
    for (;;)
        ;
}


/**
 * @brief  Run the DFU jump if one was requested before the last reset.
 *         Call at the very start of main(), before any peripheral init, so
 *         the bootloader starts from a reset-state clock/peripheral config
 *         (this is what makes its USB DFU enumerate). Returns if no jump is
 *         pending.
 */
void dfu_bootloader_if_pending(void)
{
    if (*DFU_COOKIE_ADDR != DFU_COOKIE_MAGIC)
        return;

    /* Clear the cookie first so a failed jump cannot cause a reset loop. */
    *DFU_COOKIE_ADDR = 0;
    __DSB();

    dfu_jump_to_boot();   /* never returns on success */
    for (;;)
        ;
}
