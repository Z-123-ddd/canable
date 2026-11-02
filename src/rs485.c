/**
 * @file    rs485.c
 * @brief   RS485 half-duplex driver (USART1 + DMA TX/RX + DE on PC6)
 *
 * Ported from proj1 (ODrive) Board/v3/Src/usart.c (USART1 DMA setup) and the
 * RS485 direction-control pattern in communication/rs485/modbus/port/portserial.c.
 *
 * Direction handling:
 *   - transmit : drive PC6 high, start DMA TX
 *   - DMA TX complete (DMA1_Channel4 IRQ): enable USART TC interrupt
 *   - USART TC  (USART1 IRQ)             : last stop bit shifted out -> drive PC6
 *                                          low (receive) and clear busy
 *   RX is a free-running circular DMA into s_rx_buf; bytes are consumed with
 *   rs485_rx_read().
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "rs485.h"

/* ---- fixed hardware mapping ---- */
#define RS485_USART       USART1
#define RS485_USART_IRQn  USART1_IRQn
#define RS485_TX_DMA      DMA1_CH4        /* USART1_TX request -> channel 4 */
#define RS485_RX_DMA      DMA1_CH5        /* USART1_RX request -> channel 5 */
#define RS485_DE_PORT     GPIOC
#define RS485_DE_PIN      GPIO_PIN_6

/* ---- TX staging + state ---- */
static uint8_t           s_tx_buf[RS485_TX_BUFFER_SIZE];
static volatile uint16_t s_tx_len  = 0;
static volatile uint8_t  s_tx_busy = 0;

/* ---- RX ring ---- */
static uint8_t           s_rx_buf[RS485_RX_BUFFER_SIZE];
static volatile uint16_t s_rx_read = 0;   /* consumed index into s_rx_buf */

/* ---- direction control (high = transmit, low = receive) ---- */
static inline void rs485_de_tx(void) { GPIO_SetBits(RS485_DE_PORT, RS485_DE_PIN); }
static inline void rs485_de_rx(void) { GPIO_ResetBits(RS485_DE_PORT, RS485_DE_PIN); }

/* Current DMA write index into the RX ring ((BUF - CNT) wraps to 0 when full). */
static inline uint16_t rs485_rx_write_index(void)
{
    uint16_t cnt = DMA_GetCurrDataCounter(RS485_RX_DMA);
    if (cnt > RS485_RX_BUFFER_SIZE)
        cnt = RS485_RX_BUFFER_SIZE;
    return (uint16_t)(RS485_RX_BUFFER_SIZE - cnt) % RS485_RX_BUFFER_SIZE;
}


void rs485_init(uint32_t baud)
{
    GPIO_InitType  gpio;
    DMA_InitType   dma;
    NVIC_InitType  nvic;
    USART_InitType us;

    /* ---- clocks ---- */
    RCC_EnableAHB1PeriphClk(RCC_AHB_PERIPHEN_GPIOA | RCC_AHB_PERIPHEN_GPIOC, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_USART1, ENABLE);
    RCC_EnableAHBPeriphClk(RCC_AHB_PERIPHEN_DMA1, ENABLE);

    /* ---- DE pin (PC6): push-pull output, default = receive ---- */
    GPIO_InitStruct(&gpio);
    gpio.Pin            = RS485_DE_PIN;
    gpio.GPIO_Mode      = GPIO_MODE_OUTPUT_PP;
    gpio.GPIO_Pull      = GPIO_PULL_DOWN;
    gpio.GPIO_Slew_Rate = GPIO_SLEW_RATE_FAST;
    gpio.GPIO_Current   = GPIO_DC_12mA;
    gpio.GPIO_Alternate = 0;
    GPIO_InitPeripheral(RS485_DE_PORT, &gpio);
    rs485_de_rx();

    /* ---- TX PA9 = USART1_TX (AF5) ---- */
    gpio.Pin            = GPIO_PIN_9;
    gpio.GPIO_Mode      = GPIO_MODE_AF_PP;
    gpio.GPIO_Pull      = GPIO_NO_PULL;
    gpio.GPIO_Alternate = GPIO_AF_USART1_TX_PA9;
    GPIO_InitPeripheral(GPIOA, &gpio);

    /* ---- RX PA10 = USART1_RX (AF5) ---- */
    gpio.Pin            = GPIO_PIN_10;
    gpio.GPIO_Mode      = GPIO_MODE_INPUT;
    gpio.GPIO_Pull      = GPIO_PULL_UP;
    gpio.GPIO_Alternate = GPIO_AF_USART1_RX_PA10;
    GPIO_InitPeripheral(GPIOA, &gpio);

    /* ---- DMA TX (channel 4): peripheral = USART1->DAT, memory = s_tx_buf ---- */
    DMA_DeInit(RS485_TX_DMA);
    DMA_StructInit(&dma);
    dma.PeriphAddr     = (uint32_t)&RS485_USART->DAT;
    dma.MemAddr        = (uint32_t)s_tx_buf;
    dma.Direction      = DMA_DIR_PERIPH_DST;
    dma.BufSize        = 1;
    dma.PeriphInc      = DMA_PERIPH_INC_DISABLE;
    dma.MemoryInc      = DMA_MEM_INC_ENABLE;
    dma.PeriphDataSize = DMA_PERIPH_DATA_WIDTH_BYTE;
    dma.MemDataSize    = DMA_MEM_DATA_WIDTH_BYTE;
    dma.CircularMode   = DMA_MODE_NORMAL;
    dma.Priority       = DMA_PRIORITY_LOW;
    dma.Mem2Mem        = DMA_M2M_DISABLE;
    DMA_Init(RS485_TX_DMA, &dma);
    DMA_RequestRemap(DMA_REMAP_USART1_TX, RS485_TX_DMA, ENABLE);
    DMA_ConfigInt(RS485_TX_DMA, DMA_INT_TXC, ENABLE);   /* TX-complete interrupt */
    DMA_EnableChannel(RS485_TX_DMA, DISABLE);

    /* ---- DMA RX (channel 5): peripheral = USART1->DAT, memory = s_rx_buf, circular ---- */
    DMA_DeInit(RS485_RX_DMA);
    DMA_StructInit(&dma);
    dma.PeriphAddr     = (uint32_t)&RS485_USART->DAT;
    dma.MemAddr        = (uint32_t)s_rx_buf;
    dma.Direction      = DMA_DIR_PERIPH_SRC;
    dma.BufSize        = RS485_RX_BUFFER_SIZE;
    dma.PeriphInc      = DMA_PERIPH_INC_DISABLE;
    dma.MemoryInc      = DMA_MEM_INC_ENABLE;
    dma.PeriphDataSize = DMA_PERIPH_DATA_WIDTH_BYTE;
    dma.MemDataSize    = DMA_MEM_DATA_WIDTH_BYTE;
    dma.CircularMode   = DMA_MODE_CIRCULAR;
    dma.Priority       = DMA_PRIORITY_LOW;
    dma.Mem2Mem        = DMA_M2M_DISABLE;
    DMA_Init(RS485_RX_DMA, &dma);
    DMA_RequestRemap(DMA_REMAP_USART1_RX, RS485_RX_DMA, ENABLE);
    DMA_EnableChannel(RS485_RX_DMA, DISABLE);

    /* ---- USART1: 8N1, RX+TX ---- */
    us.BaudRate            = baud;
    us.WordLength          = USART_WL_8B;
    us.StopBits            = USART_STPB_1;
    us.Parity              = USART_PE_NO;
    us.Mode                = USART_MODE_RX | USART_MODE_TX;
    us.HardwareFlowControl = USART_HFCTRL_NONE;
    USART_Init(RS485_USART, &us);
    USART_EnableDMA(RS485_USART, USART_DMAREQ_TX | USART_DMAREQ_RX, ENABLE);

    /* ---- NVIC: DMA TX-complete + USART TC ---- */
    nvic.NVIC_IRQChannel                   = DMA1_Channel4_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 7;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);

    /* USART1 is deliberately left DISABLED in the NVIC here: rs485_init() also
     * runs at boot in CAN mode, and with a floating RS485 RX line the USART
     * would raise IDLE/error interrupts continuously with nobody servicing them
     * (USART1_IRQHandler only clears TC), wedging the main loop.  The line is
     * enabled by rs485_irq_set(ENABLE) when RS485 mode is selected. */
    nvic.NVIC_IRQChannel                   = RS485_USART_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 7;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = DISABLE;
    NVIC_Init(&nvic);

    USART_ConfigInt(RS485_USART, USART_INT_TXC, DISABLE);   /* enabled per TX */
    USART_Enable(RS485_USART, ENABLE);

    /* ---- arm the RX DMA (free-running) ---- */
    DMA_SetCurrDataCounter(RS485_RX_DMA, RS485_RX_BUFFER_SIZE);
    RS485_RX_DMA->MADDR = (uint32_t)s_rx_buf;
    DMA_EnableChannel(RS485_RX_DMA, ENABLE);
}


int32_t rs485_send(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > RS485_TX_BUFFER_SIZE)
        return -1;
    if (s_tx_busy)
        return -1;

    for (uint16_t i = 0; i < len; i++)
        s_tx_buf[i] = data[i];
    s_tx_len  = len;
    s_tx_busy = 1;

    /* Assert DE (transmit) BEFORE starting the DMA so the transceiver is
       driving by the time the first bit leaves the USART. */
    rs485_de_tx();

    DMA_SetCurrDataCounter(RS485_TX_DMA, len);
    RS485_TX_DMA->MADDR = (uint32_t)s_tx_buf;
    DMA_EnableChannel(RS485_TX_DMA, ENABLE);
    return 0;
}


uint8_t rs485_tx_busy(void)
{
    return s_tx_busy;
}


void rs485_irq_set(int state)
{
    if (state != 0)
        NVIC_EnableIRQ(RS485_USART_IRQn);
    else
        NVIC_DisableIRQ(RS485_USART_IRQn);
}


void rs485_stop(void)
{
    /* Stop the DMA channels, the USART TC interrupt and the USART itself, then
       park DE in the receive state. Safe to call even if never started. */
    rs485_irq_set(DISABLE);
    DMA_EnableChannel(RS485_TX_DMA, DISABLE);
    DMA_EnableChannel(RS485_RX_DMA, DISABLE);
    USART_ConfigInt(RS485_USART, USART_INT_TXC, DISABLE);
    USART_Enable(RS485_USART, DISABLE);
    rs485_de_rx();
    s_tx_busy = 0;
}


uint16_t rs485_rx_available(void)
{
    uint16_t w = rs485_rx_write_index();
    return (uint16_t)((w + RS485_RX_BUFFER_SIZE - s_rx_read) % RS485_RX_BUFFER_SIZE);
}


uint16_t rs485_rx_read(uint8_t *out, uint16_t max)
{
    if (out == NULL)
        return 0;
    uint16_t n = 0;
    while (n < max && s_rx_read != rs485_rx_write_index())
    {
        out[n++] = s_rx_buf[s_rx_read];
        s_rx_read = (uint16_t)((s_rx_read + 1) % RS485_RX_BUFFER_SIZE);
    }
    return n;
}


/* ---- ISRs ---------------------------------------------------------------- */

/* DMA TX complete: the whole buffer has been handed to the USART shift register.
   The last byte may still be on the wire, so wait for USART TC (below) before
   releasing DE. */
void DMA1_Channel4_IRQHandler(void)
{
    if (DMA_GetFlagStatus(DMA_FLAG_TC4, DMA1) == SET)
    {
        DMA_ClrIntPendingBit(DMA_INT_TXC4, DMA1);
        DMA_EnableChannel(RS485_TX_DMA, DISABLE);
        USART_ConfigInt(RS485_USART, USART_INT_TXC, ENABLE);   /* defer DE release to TC */
    }
}

/* USART TC: transmission of the final stop bit is complete -> release DE. */
void USART1_IRQHandler(void)
{
    if (USART_GetIntStatus(RS485_USART, USART_INT_TXC) == SET)
    {
        USART_ClrFlag(RS485_USART, USART_FLAG_TXC);
        USART_ConfigInt(RS485_USART, USART_INT_TXC, DISABLE);
        rs485_de_rx();          /* back to receive */
        s_tx_busy = 0;
    }
}
