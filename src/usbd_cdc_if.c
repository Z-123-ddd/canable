/**
 * @file    usbd_cdc_if.c
 * @brief   USB CDC interface implementation for N32H473
 * 
 * Implements a simple USB CDC ACM device for the CANable2.
 * Data flow:
 *   Host -> USB OUT EP1 -> rx_buffer -> cdc_process() -> usb_proto parser -> CAN TX
 *   CAN RX -> usb_proto binary frame -> cdc_transmit() -> packet queue -> USB IN EP3
 *          (EP3 is bulk IN and double buffered: one 64-byte PMA packet per CAN
 *           report, frames are never merged into a shared packet)
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include "usbfsd_lib.h"
#include "usb_conf.h"
#include "usbd_cdc_if.h"
#include "usb_proto.h"
#include "error.h"
#include "usb_pwr.h"
#include "system.h"

// Private variables
static usbrx_buf_t rxbuf = {0};
static usbtx_ring_t txbuf = {0};
static uint8_t rx_packet_buf[CDC_DATA_FS_MAX_PACKET_SIZE];

/* ---- Uplink pipeline (endpoint 3 IN, bulk, double buffered) ---------------
 *
 * PMA holds two 64-byte packets for EP3 IN and the core transmits them in
 * strict alternation, so the software only has to track
 *     txq_buf[] / txq_n : PMA buffers already handed to the core, in order
 *     tx_fill           : PMA buffer the next packet is written into
 * and refill the buffer that has just left the wire from the completion
 * interrupt.  The next packet is therefore already in PMA before the host
 * polls the endpoint again, which removes the NAK gap between packets (the
 * old code waited for the main loop, costing ~25us of dead bus time each).
 */
#define CDC_TX_DEPTH  (CDC_TX_USE_DBUF ? 2 : 1)

/* Hand buffer 1 to the core by toggling DTOG_RX (USB_FreeUserBuf).  Kept as
 * a local switch because the double-buffer handshake of this IP is
 * undocumented: on silicon where the core tracks the release itself the
 * extra toggle desynchronises the pipeline after a couple of packets. */
#define CDC_TX_RELEASE_BUF1  1

static uint8_t txq_buf[2];
static volatile uint8_t txq_n;        // read from the CAN RX ISR too
static uint8_t tx_fill;


#if CDC_TX_DEBUG
volatile uint32_t dbg_txmit;   /* cdc_transmit() calls that queued data   */
volatile uint32_t dbg_drop;    /* cdc_transmit() calls refused (queue)    */
volatile uint32_t dbg_pkts;    /* packets moved into PMA                  */
volatile uint32_t dbg_ctr;     /* completion interrupts serviced          */
volatile uint32_t dbg_nak;     /* arm() runs that left the endpoint NAKed */
volatile uint32_t dbg_fill1;   /* buffer-1 loads                          */
#endif

#if CDC_TX_USE_DBUF
#define CDC_TX_EP  ENDP3
#else
#define CDC_TX_EP  ENDP1
#endif

static uint16_t pma_addr_of(uint8_t buf)
{
#if CDC_TX_USE_DBUF
    return (buf == 0) ? (uint16_t)ENDP3_TXBUF0_ADDR : (uint16_t)ENDP3_TXBUF1_ADDR;
#else
    (void)buf;
    return (uint16_t)ENDP1_TXADDR;
#endif
}

static uint32_t tx_pkts_used(void)
{
    return (txbuf.head >= txbuf.tail) ? (txbuf.head - txbuf.tail)
                                      : (USBTX_PKT_NUM - txbuf.tail + txbuf.head);
}

/* One slot is always kept empty so head == tail means "empty". */
static uint32_t tx_pkts_free(void)
{
    return (uint32_t)(USBTX_PKT_NUM - 1u) - tx_pkts_used();
}

/* Move the oldest queued packet into PMA buffer `buf`.
 * Returns the packet length, or 0 when nothing is queued. */
static uint16_t tx_load_pma(uint8_t buf)
{
    if (txbuf.tail == txbuf.head)
        return 0;

    uint16_t n = txbuf.len[txbuf.tail];

    USB_CopyUserToPMABuf(txbuf.data[txbuf.tail], pma_addr_of(buf), n);

#if CDC_TX_USE_DBUF
    if (buf == 0)
        USB_SetEpDblBuf0Cnt(CDC_TX_EP, EP_DBUF_IN, n);   /* ADDR_TX / COUNT_TX  */
    else
        USB_SetEpDblBuf1Cnt(CDC_TX_EP, EP_DBUF_IN, n);   /* ADDR_RX / COUNT_RX  */
#else
    (void)buf;
    USB_SetEpTxCnt(CDC_TX_EP, n);
#endif

    txbuf.tail = (txbuf.tail + 1u) % USBTX_PKT_NUM;
#if CDC_TX_DEBUG
    dbg_pkts++;
    if (buf == 1)
        dbg_fill1++;
#endif
    return n;
}

/* Load every packet the core has room for, then set the endpoint status.
 * Must run with USB interrupts masked: interrupt context, or a critical
 * section from the main loop. */
static void cdc_tx_arm(void)
{
    while (txq_n < CDC_TX_DEPTH)
    {
        uint8_t buf = (uint8_t)(CDC_TX_USE_DBUF ? tx_fill : 0u);

        if (tx_load_pma(buf) == 0)
            break;

        txq_buf[txq_n++] = buf;
        tx_fill ^= 1u;

#if CDC_TX_RELEASE_BUF1
        /* Buffer 1 only becomes visible to the core once the software hands it
         * over (toggles DTOG_RX); buffer 0 needs no handshake. */
        if (buf == 1)
            USB_FreeUserBuf(CDC_TX_EP, EP_DBUF_IN);
#endif
    }

    if (txq_n != 0)
    {
#if CDC_TX_USE_DBUF
        SetEPRxStatus(CDC_TX_EP, EP_RX_NAK);
#endif
        SetEPTxStatus(CDC_TX_EP, EP_TX_VALID);
    }
    else
    {
        SetEPTxStatus(CDC_TX_EP, EP_TX_NAK);
#if CDC_TX_DEBUG
        dbg_nak++;
#endif
    }
}

/* Has a host completed enumeration?  Callable from interrupt context. */
uint8_t usb_host_available(void)
{
    return (uint8_t)((bDeviceState == CONFIGURED) ? 1u : 0u);
}


/* Is the uplink backed up because the host is not draining it?
 *
 * Deliberately state-based, not time-based: every PMA buffer has been handed to
 * the core (txq_n == CDC_TX_DEPTH) and packets are still queued behind them.
 * That can only persist while nobody polls the IN endpoint.  Only consulted
 * when a frame is already being discarded, so a momentarily busy host never
 * causes a drop by itself.
 *
 * Callable from interrupt context. */
uint8_t usb_tx_saturated(void)
{
    if (txq_n < CDC_TX_DEPTH)
        return 0;
    return (uint8_t)((txbuf.tail != txbuf.head) ? 1u : 0u);
}


/* Drop the pipeline bookkeeping.  A bus reset makes the core forget whatever
 * was armed, so the software view has to go with it or the uplink wedges. */
void cdc_tx_reset(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    txbuf.head = 0;
    txbuf.tail = 0;
    txq_buf[0] = 0;
    txq_buf[1] = 0;
    txq_n      = 0;
    tx_fill    = CDC_TX_FIRST_BUF;

    __set_PRIMASK(primask);
}

/* One packet left the wire: free its PMA buffer, refill it, re-arm. */
void cdc_tx_service(void)
{
#if CDC_TX_DEBUG
    dbg_ctr++;
#endif
    if (txq_n != 0)
    {
        uint8_t sent = txq_buf[0];

        txq_buf[0] = txq_buf[1];
        txq_n--;

        /* The core alternates buffers.  While a packet is still queued the one
         * that just went out is the only free buffer; once the pipeline is
         * empty both are free, and we must resume on the buffer the core looks
         * at next. */
        tx_fill = (txq_n != 0) ? sent : (uint8_t)(sent ^ 1u);
    }

    cdc_tx_arm();
}


// USB device initialization
void usb_init(void)
{
    NVIC_InitType nvic;
    EXTI_InitType exti;
    GPIO_InitType gpio;

    // Configure USB clock: PLL output -> /5 -> 48MHz for USB FS
    RCC_ConfigUSBFSClk(RCC_USBFS_CLKSRC_PLLPRES);
    RCC_ConfigUSBPLLPresClk(RCC_USBPLLCLK_SRC_PLL, RCC_USBPLLCLK_DIV5);
    RCC->CFG3 |= RCC_CFG3_USBFSTM;

    // Enable USB clock
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_USBFS, ENABLE);

    // Enable AFIO and GPIO clocks
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);
    RCC_EnableAHB1PeriphClk(RCC_AHB_PERIPHEN_GPIOA, ENABLE);

    // Configure USB pins: PA11=DM, PA12=DP, AF10
    GPIO_InitStruct(&gpio);
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    gpio.GPIO_Mode = GPIO_MODE_AF_PP;
    gpio.GPIO_Alternate = GPIO_AF10;
    gpio.GPIO_Pull = GPIO_NO_PULL;
    gpio.GPIO_Slew_Rate = GPIO_SLEW_RATE_FAST;
    gpio.GPIO_Current = GPIO_DC_12mA;
    GPIO_InitPeripheral(GPIOA, &gpio);

    // Enable USB LP interrupt
    nvic.NVIC_IRQChannel = USB_FS_LP_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 6;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    // Enable USB Wakeup interrupt
    nvic.NVIC_IRQChannel = USB_FS_WKUP_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 9;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    // Configure EXTI line 18 for USB wakeup
    EXTI_ClrITPendBit(EXTI_LINE18);
    exti.EXTI_Line = EXTI_LINE18;
    exti.EXTI_Mode = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Rising;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_InitPeripheral(&exti);

    // Initialize RX/TX buffers
    rxbuf.head = 0;
    rxbuf.tail = 0;
    cdc_tx_reset();

    // Initialize USB core
    USB_Init();

    // Prepare first RX on EP1 OUT (endpoint address = 0x01 for EP1 OUT)
    // The N32 USB library reads directly from PMA in the OUT callback
}


// CDC process: check for received data and process TX
void cdc_process(void)
{
    // Process received data from USB
    while (rxbuf.tail != rxbuf.head)
    {
        uint8_t *msg = rxbuf.buf[rxbuf.tail];
        uint32_t len = rxbuf.msglen[rxbuf.tail];

        // Feed every byte into the binary USB protocol parser (byte stream:
        // frames may span several USB packets, packet boundaries ignored)
        for (uint32_t i = 0; i < len; i++)
            usb_proto_feed(msg[i]);

        rxbuf.tail = (rxbuf.tail + 1) % NUM_RX_BUFS;
    }

    // The uplink normally runs from cdc_transmit() (kick) plus the EP3
    // completion interrupt (refill).  This is only a safety net for a lost
    // interrupt, so it must not run while a packet is already in flight.
    if (txq_n == 0 && txbuf.tail != txbuf.head)
    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();
        if (txq_n == 0 && txbuf.tail != txbuf.head)
            cdc_tx_arm();
        __set_PRIMASK(primask);
    }
}


// CDC transmit: queue one report for the host.
//
// The report is chopped into 64-byte packets and queued as whole packets, all
// or nothing: a partial report on the wire desynchronises the host byte-stream
// parser (e.g. stuck in LOG_TEXT until overflow), which shows as "LED on, no
// log".  Packets from different calls are never merged, so one CAN/FD frame
// never shares a packet with another frame; a frame too long for 64 bytes
// simply spans two packets.
void cdc_transmit(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0)
        return;

    uint16_t npkt = (uint16_t)((len + USBTX_PKT_SIZE - 1u) / USBTX_PKT_SIZE);

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (tx_pkts_free() < npkt)
    {
        __set_PRIMASK(primask);
        // Sticky raise (no report flood): TX full is often a consequence of
        // CAN RX backlog before the host opens the port.
        error_raise(ERR_FULLBUF_USBTX);
#if CDC_TX_DEBUG
        dbg_drop++;
#endif
        return;
    }

    uint16_t off = 0;
    for (uint16_t p = 0; p < npkt; p++)
    {
        uint16_t n = (uint16_t)(len - off);
        if (n > USBTX_PKT_SIZE)
            n = USBTX_PKT_SIZE;

        for (uint16_t i = 0; i < n; i++)
            txbuf.data[txbuf.head][i] = buf[off + i];

        txbuf.len[txbuf.head] = (uint8_t)n;
        txbuf.head = (txbuf.head + 1u) % USBTX_PKT_NUM;
        off = (uint16_t)(off + n);
    }

    // Successfully queued: clear the TX-full sticky if it was set.
    error_clear(ERR_FULLBUF_USBTX);
#if CDC_TX_DEBUG
    dbg_txmit++;
#endif

    // Start the transfer right away instead of waiting for the next
    // cdc_process() turn; the following packets continue from the completion
    // interrupt.
    cdc_tx_arm();

    __set_PRIMASK(primask);
}


uint32_t cdc_tx_free(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t free_bytes = tx_pkts_free() * USBTX_PKT_SIZE;
    __set_PRIMASK(primask);
    return free_bytes;
}


void cdc_tx_drop_oldest(uint32_t nbytes)
{
    if (nbytes == 0)
        return;

    // Whole packets only: dropping half a packet would corrupt the stream.
    uint32_t npkt = (nbytes + USBTX_PKT_SIZE - 1u) / USBTX_PKT_SIZE;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t used = tx_pkts_used();
    if (npkt > used)
        npkt = used;
    txbuf.tail = (txbuf.tail + npkt) % USBTX_PKT_NUM;

    __set_PRIMASK(primask);
}


// EP1 OUT callback - called when USB host sends data to us
void CDC_EP1_OUT_Callback(void)
{
    // Read received data from USB PMA using SIL layer
    // EP1 OUT: endpoint address = 0x01
    uint32_t len = USB_SilRead(0x01, rx_packet_buf);

    if (len > 0 && len <= CDC_DATA_FS_MAX_PACKET_SIZE)
    {
        uint32_t next_head = (rxbuf.head + 1) % NUM_RX_BUFS;
        if (next_head != rxbuf.tail)
        {
            for (uint32_t i = 0; i < len && i < RX_BUF_SIZE; i++)
            {
                rxbuf.buf[rxbuf.head][i] = rx_packet_buf[i];
            }
            rxbuf.msglen[rxbuf.head] = len;
            rxbuf.head = next_head;
        }
        else
        {
            error_assert(ERR_FULLBUF_USBRX);
        }
    }

    // Re-arm EP1 OUT to receive next packet
    SetEPRxStatus(ENDP1, EP_RX_VALID);
}
