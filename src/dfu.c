/**
 * @file    dfu.c
 * @brief   DFU (system bootloader) entry implementation (N32H473)
 *
 * Ported from the ODrive N32 firmware (proj1, MotorControl/main.cpp).
 *
 * The N32H47x factory bootloader is located in the system-memory area and
 * supports DFU download over USB. Jumping to it from the running
 * application requires:
 *   1. disabling the flash i-cache (it is cleaned before leaving),
 *   2. probing the bootloader version (Get_NVR in system memory),
 *   3. building a 128-word vector table at the TOP of SRAM that redirects
 *      the interrupts used by the bootloader (SysTick / USART3 / USB FS)
 *      into the boot code (0x1FFF2xxx),
 *   4. disabling interrupts, re-arming the IWDG and switching the system
 *      clock back to HSI (the bootloader starts from reset-state clocks),
 *   5. setting the stack pointer / VTOR to the boot image and jumping.
 *
 * NOTE: the ODrive source used a 144KB SRAM size (0x24000); this chip
 * (N32H473, 128KB SRAM) places the vector table at 0x2001FE00 instead.
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "dfu.h"

/* ---- Constants from the N32 system bootloader (same chip family) ---- */
#define SRAM_VECTOR_WORD_SIZE   (128)
#define SRAM_VECTOR_ADDR        (0x2001FE00u)   /* top of 128KB SRAM - 0x200 */

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
 * @brief  Jump into the N32 DFU bootloader (never returns on success).
 */
static void dfu_jump_to_boot(void)
{
    uint8_t value1[8] = {0x00};
    uint8_t value2 = 8;
    uint32_t i;
    uint32_t *pVec;

    /* Build an empty vector table at the top of SRAM */
    pVec = (uint32_t *)SRAM_VECTOR_ADDR;
    for (i = 0; i < SRAM_VECTOR_WORD_SIZE; i++)
        pVec[i] = 0;

    /* Disable / reset the flash instruction cache */
    FLASH_iCacheCmd(FLASH_iCache_DIS);
    FLASH_iCacheRST();

    /* Probe the bootloader version */
    if (Get_NVR != NULL)
        Get_NVR(0x1FFF37A8u, value1, value2);

    if ((value1[0] & 0xFFu) == 0x11u)   /* BOOT V1.1 */
    {
        SPAddr = 0x200024C0u;
        pVec[SysTick_IRQn + 16]  = 0x1FFF2F15u;
        pVec[USART3_IRQn + 16]   = 0x1FFF2F95u;
        pVec[USB_FS_LP_IRQn + 16]= 0x1FFF3095u;
    }
    else                              /* BOOT V1.0 */
    {
        pVec[SysTick_IRQn + 16]  = 0x1FFF2D31u;
        pVec[USART3_IRQn + 16]   = 0x1FFF2DB1u;
        pVec[USB_FS_LP_IRQn + 16]= 0x1FFF2EB1u;
    }

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
    SCB->VTOR = SRAM_VECTOR_ADDR;
    jump_boot();
}


/**
 * @brief  Public API: switch to the DFU bootloader.
 *         Never returns on success.
 */
void dfu_enter_bootloader(void)
{
    dfu_jump_to_boot();
    /* Not reached on success. Keep the compiler / watchdog honest. */
    for (;;)
        ;
}
