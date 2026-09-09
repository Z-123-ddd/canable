/**
 * @file    can.c
 * @brief   FDCAN driver for N32H473
 * 
 * Uses FDCAN1 peripheral.
 * CAN clock: PLL /6 = 240MHz /6 = 40MHz
 * 
 * Bit timing formula: CAN baud = FDCAN_CLK / (Prescaler * (TimeSeg1 + TimeSeg2 + 1))
 * With TimeSeg1=14, TimeSeg2=2: tq_total = 14+2+1 = 17
 *   Prescaler=47  →  40M / (47*17) ≈ 50k
 *   Prescaler=24  →  40M / (24*17) ≈ 100k (actually ~98k, use 23 for ~102k)
 *   Prescaler=19  →  40M / (19*17) ≈ 125k (actually ~124k)
 *   Prescaler=9   →  40M / (9*17)  ≈ 250k (actually ~261k, use 10 for ~235k)
 *   Prescaler=5   →  40M / (5*17)  ≈ 500k (actually ~470k, use 4 for ~588k)
 *   Prescaler=2   →  40M / (2*17)  ≈ 1M (actually ~1.18M)
 */

#include "n32h47x_48x.h"
#include "n32h47x_48x_conf.h"
#include <string.h>
#include "can.h"
#include "led.h"
#include "error.h"
#include "system.h"


// N32 FDCAN message RAM (must be in SRAM, 32-bit aligned)
static uint32_t FDCAN_ram[1024];
static FDCAN_MsgRamType can_msg_ram;

// Private variables
static FDCAN_InitType can_handle;
static uint32_t prescaler, sync_jump_width, time_seg1, time_seg2;
static uint32_t data_prescaler, data_sync_jump_width, data_time_seg1, data_time_seg2;
enum can_bus_state bus_state;
static uint8_t can_autoretransmit = ENABLE;
static can_txbuf_t txqueue = {0};

// Binary USB protocol configuration (defaults used until a command changes them)
static can_id_format_t can_id_format = CAN_ID_FORMAT_STANDARD;
static can_send_mode_t can_send_mode = CAN_SEND_MODE_TXRING;

// Software RX queue: FDCAN ISR pushes frames here, main loop drains them
static can_rxbuf_t rxqueue = {0};


// DLC code to byte count conversion
int8_t hal_dlc_code_to_bytes(uint32_t hal_dlc_code)
{
    switch (hal_dlc_code) {
        case FDCAN_DLC_BYTES_0:  return 0;
        case FDCAN_DLC_BYTES_1:  return 1;
        case FDCAN_DLC_BYTES_2:  return 2;
        case FDCAN_DLC_BYTES_3:  return 3;
        case FDCAN_DLC_BYTES_4:  return 4;
        case FDCAN_DLC_BYTES_5:  return 5;
        case FDCAN_DLC_BYTES_6:  return 6;
        case FDCAN_DLC_BYTES_7:  return 7;
        case FDCAN_DLC_BYTES_8:  return 8;
        case FDCAN_DLC_BYTES_12: return 12;
        case FDCAN_DLC_BYTES_16: return 16;
        case FDCAN_DLC_BYTES_20: return 20;
        case FDCAN_DLC_BYTES_24: return 24;
        case FDCAN_DLC_BYTES_32: return 32;
        case FDCAN_DLC_BYTES_48: return 48;
        case FDCAN_DLC_BYTES_64: return 64;
        default: return -1;
    }
}


// Convert a raw nibble DLC code (0-15, incl. CAN FD codes) to an N32 HAL DLC code.
// Shared by the binary USB protocol (data-frame control byte low nibble).
uint32_t can_dlc_code_to_hal_dlc(uint8_t dlc_code)
{
    if (dlc_code <= 8)
        return (uint32_t)(dlc_code << 16);   // 0-8 map directly to FDCAN DLC bytes
    else if (dlc_code == 9)
        return FDCAN_DLC_BYTES_12;
    else if (dlc_code == 10)
        return FDCAN_DLC_BYTES_16;
    else if (dlc_code == 11)
        return FDCAN_DLC_BYTES_20;
    else if (dlc_code == 12)
        return FDCAN_DLC_BYTES_24;
    else if (dlc_code == 13)
        return FDCAN_DLC_BYTES_32;
    else if (dlc_code == 14)
        return FDCAN_DLC_BYTES_48;
    else if (dlc_code == 15)
        return FDCAN_DLC_BYTES_64;
    else
        return FDCAN_DLC_BYTES_8;
}


// ---- Binary USB protocol configuration (ID width / send mode) ----

void can_set_id_format(can_id_format_t fmt)
{
    can_id_format = fmt;
}

can_id_format_t can_get_id_format(void)
{
    return can_id_format;
}

void can_set_send_mode(can_send_mode_t mode)
{
    can_send_mode = mode;
}

can_send_mode_t can_get_send_mode(void)
{
    return can_send_mode;
}


// Initialize CAN peripheral settings, but don't start the peripheral
void can_init(void)
{
    // Configure CAN clock: PLL /6 = 40MHz
    RCC_ConfigFDCANPllClk(RCC_FDCAN_PLLSRC_DIV6);
    RCC_ConfigFDCANClksrc(RCC_FDCAN_CLKSRC_PLL);

    // Enable FDCAN1 clock
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_FDCAN1, ENABLE);
    RCC_EnableAPB1PeriphReset(RCC_APB1_PERIPH_FDCAN1);

    // Enable GPIO clocks
    RCC_EnableAHB1PeriphClk(RCC_AHB_PERIPHEN_GPIOA |
                            RCC_AHB_PERIPHEN_GPIOB |
                            RCC_AHB_PERIPHEN_GPIOC, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);

    GPIO_InitType gpio;
    GPIO_InitStruct(&gpio);

    // CAN Standby pin (PA0) - turn standby off (active low)
    // gpio.Pin = GPIO_PIN_0;
    // gpio.GPIO_Mode = GPIO_MODE_OUTPUT_PP;
    // gpio.GPIO_Pull = GPIO_PULL_DOWN;
    // gpio.GPIO_Slew_Rate = GPIO_SLEW_RATE_SLOW;
    // gpio.GPIO_Current = GPIO_DC_2mA;
    // gpio.GPIO_Alternate = 0;
    // GPIO_InitPeripheral(GPIOA, &gpio);
    // GPIO_WriteBits(GPIOA, GPIO_PIN_0, Bit_RESET); // STB = low (active)

    // CAN IO power (PC13) - enable CAN transceiver power
    // gpio.Pin = GPIO_PIN_13;
    // gpio.GPIO_Pull = GPIO_PULL_DOWN;
    // GPIO_InitPeripheral(GPIOC, &gpio);
    // GPIO_WriteBits(GPIOC, GPIO_PIN_13, Bit_SET);  // Power ON

    // FDCAN1_RX on PB12 (AF26), FDCAN1_TX on PB13 (AF25)
    // Note: N32 FDCAN1 TX and RX use DIFFERENT alternate function numbers!
    gpio.Pin = GPIO_PIN_12;
    gpio.GPIO_Mode = GPIO_MODE_INPUT;
    gpio.GPIO_Pull = GPIO_PULL_UP;
    gpio.GPIO_Slew_Rate = GPIO_SLEW_RATE_FAST;
    gpio.GPIO_Current = GPIO_DC_12mA;
    gpio.GPIO_Alternate = GPIO_AF_FDCAN1_RX;  // AF26
    GPIO_InitPeripheral(GPIOB, &gpio);

    gpio.Pin = GPIO_PIN_13;
    gpio.GPIO_Mode = GPIO_MODE_AF_PP;
    gpio.GPIO_Alternate = GPIO_AF_FDCAN1_TX;  // AF25
    GPIO_InitPeripheral(GPIOB, &gpio);

    // Build FDCAN init structure (no InitStruct function in N32 HAL — zero it out)
    memset(&can_handle, 0, sizeof(can_handle));

    // Default to 1M nominal arbitration, 5M FD data (CAN FD max rate)
    can_set_bitrate(CAN_BITRATE_1000K);
    can_set_data_bitrate(CAN_DATA_BITRATE_5M);  // FD data segment default: 5M (CAN FD max)

    can_handle.FrameFormat = FDCAN_FRAME_FD_BRS;
#ifdef CAN_LOOPBACK_TEST
    can_handle.Mode = FDCAN_MODE_INTERNAL_LOOPBACK;  // 自收发测试
#else
    can_handle.Mode = FDCAN_MODE_NORMAL;
#endif
    can_handle.AutoRetransmission = can_autoretransmit;
    can_handle.TransmitPause = DISABLE;
    can_handle.ProtocolException = DISABLE;

    // CAN bit timing (will be updated by can_set_bitrate)
    can_handle.Prescaler = 1;
    can_handle.SyncJumpWidth = 2;
    can_handle.TimeSeg1 = 34;
    can_handle.TimeSeg2 = 5;

    // CAN FD data bit timing
    can_handle.DataPrescaler = 1;
    can_handle.DataSyncJumpWidth = 1;
    can_handle.DataTimeSeg1 = 14;
    can_handle.DataTimeSeg2 = 5;

    // Message RAM configuration
    can_handle.MsgRamStrAddr = (uint32_t)FDCAN_ram;
    can_handle.MsgRamOffset = 0;
    can_handle.pMsgInfo = &can_msg_ram;

    // Filter: accept ALL frames via global filter (no per-ID filter needed)
    can_handle.StdFilterSize = 0;
    can_handle.ExtFilterSize = 0;
    can_handle.RxFifo0Size = 8;
    can_handle.RxFifo0DataSize = FDCAN_DATA_BYTES_64;
    can_handle.RxFifo1Size = 0;
    can_handle.RxFifo1DataSize = FDCAN_DATA_BYTES_8;
    can_handle.RxBufferSize = 0;
    can_handle.RxBufferDataSize = FDCAN_DATA_BYTES_8;
    can_handle.TxBufferSize = 0;
    can_handle.TxBufferDataSize = FDCAN_DATA_BYTES_64;
    can_handle.TxFifoQueueSize = 8;
    can_handle.TxFifoQueueMode = FDCAN_TX_FIFO_MODE;
    can_handle.TxEventSize = 0;

    bus_state = OFF_BUS;
}


// Start the CAN peripheral
void can_enable(void)
{
    if (bus_state == OFF_BUS)
    {
        // De-init first to clear state
        FDCAN_DeInit(FDCAN1);

        // Reset software RX/TX queues (drop any stale frames from a previous session)
        rxqueue.head = 0;
        rxqueue.tail = 0;
        txqueue.head = 0;
        txqueue.tail = 0;

        // Apply current bitrate settings
        can_handle.Prescaler = prescaler;
        can_handle.SyncJumpWidth = sync_jump_width;
        can_handle.TimeSeg1 = time_seg1;
        can_handle.TimeSeg2 = time_seg2;
        can_handle.DataPrescaler = data_prescaler;
        can_handle.DataSyncJumpWidth = data_sync_jump_width;
        can_handle.DataTimeSeg1 = data_time_seg1;
        can_handle.DataTimeSeg2 = data_time_seg2;

        // Initialize FDCAN
        FDCAN_Init(FDCAN1, &can_handle);

        // Global filter: accept all standard+extended frames into RX FIFO0
        FDCAN_ConfigGlobalFilter(FDCAN1,
            FDCAN_ACCEPT_STD_IN_RX_FIFO0, FDCAN_ACCEPT_EXT_IN_RX_FIFO0,
            FDCAN_FILTER_STD_REMOTE, FDCAN_FILTER_EXT_REMOTE);

        // Configure timestamp
        FDCAN_ConfigTSPrescaler(FDCAN1, FDCAN_TIMESTAMP_PRESC_16);
        FDCAN_Config_TS(FDCAN1, FDCAN_TIMESTAMP_DISABLE);

        // Configure Tx delay compensation for CAN FD
        FDCAN_ConfigTxDelayCompensation(FDCAN1, can_handle.DataTimeSeg1, can_handle.DataTimeSeg1);
        FDCAN_EnableTxDelayCompensation(FDCAN1);

        // Start FDCAN
        FDCAN_Start(FDCAN1);

        // Enable FDCAN1 interrupts on line 0:
        //   - Rx FIFO 0 new message -> ISR drains HW FIFO into software RX queue
        //   - Bus-Off             -> ISR restarts the CAN controller automatically
        FDCAN_ConfigIntLine(FDCAN1, FDCAN_INT_RX_FIFO0_NEW_MESSAGE, FDCAN_INTERRUPT_LINE0);
        FDCAN_ActivateInt(FDCAN1, FDCAN_INT_RX_FIFO0_NEW_MESSAGE, FDCAN_TX_BUFFER0);
        FDCAN_ConfigIntLine(FDCAN1, FDCAN_INT_BUS_OFF, FDCAN_INTERRUPT_LINE0);
        FDCAN_ActivateInt(FDCAN1, FDCAN_INT_BUS_OFF, FDCAN_TX_BUFFER0);

        NVIC_InitType nvic;
        nvic.NVIC_IRQChannel = FDCAN1_INT0_IRQn;
        nvic.NVIC_IRQChannelPreemptionPriority = 7; // below USB FS LP (6) / SysTick (0)
        nvic.NVIC_IRQChannelSubPriority = 0;
        nvic.NVIC_IRQChannelCmd = ENABLE;
        NVIC_Init(&nvic);

        bus_state = ON_BUS;
        led_blue_on();
    }
}


// Disable the CAN peripheral
void can_disable(void)
{
    if (bus_state == ON_BUS)
    {
        // Disable FDCAN1 interrupts first so no ISR can fire while stopping
        FDCAN_DeactivateInt(FDCAN1, FDCAN_INT_RX_FIFO0_NEW_MESSAGE | FDCAN_INT_BUS_OFF);

        FDCAN_Stop(FDCAN1);
        FDCAN_DeInit(FDCAN1);

        // Drop any frames still queued in software RX/TX buffers
        rxqueue.head = 0;
        rxqueue.tail = 0;
        txqueue.head = 0;
        txqueue.tail = 0;

        bus_state = OFF_BUS;
        led_green_on();
    }
}


// Set the CAN FD data bitrate
void can_set_data_bitrate(enum can_data_bitrate bitrate)
{
    if (bus_state == ON_BUS) return;

    switch (bitrate) {
        case CAN_DATA_BITRATE_2M:
            data_prescaler = 1;
            data_sync_jump_width = 1;
            data_time_seg1 = 15;
            data_time_seg2 = 4; // 采样点：（1+15）/（1+15+4）≈ 0.8
            break;
        case CAN_DATA_BITRATE_5M:
        default:
            data_prescaler = 1;
            data_sync_jump_width = 1;
            data_time_seg1 = 5;
            data_time_seg2 = 2; // 采样点：（1+5）/（1+5+2）≈ 0.75
            break;
    }
    led_green_on();
}


// Set the CAN bitrate
void can_set_bitrate(enum can_bitrate bitrate)
{
    if (bus_state == ON_BUS) return;

    // CAN clock = 40MHz. Formula: baud = 40M / (prescaler * (TimeSeg1+TimeSeg2+1))
    switch (bitrate) {
        case CAN_BITRATE_10K: // 40M / (100*(34+5+1)) ≈ 10k
            prescaler = 100;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8; // 采样点：（1+31）/（1+31+8）≈ 0.8
            break;
        case CAN_BITRATE_20K:
            prescaler = 50;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8;
            break;
        case CAN_BITRATE_50K:
            prescaler = 20;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8;
            break;
        case CAN_BITRATE_83_3K:
            prescaler = 12;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8;
            break;
        case CAN_BITRATE_100K:
            prescaler = 10;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8;
            break;
        case CAN_BITRATE_125K:
            prescaler = 8;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8;
            break;
        case CAN_BITRATE_250K:
            prescaler = 4;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8;
            break;
        case CAN_BITRATE_500K:
            prescaler = 2;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8;
            break;
        case CAN_BITRATE_750K:
            prescaler = 2; // 40M / (2*(22+5+1)) ≈ 740.7k
            sync_jump_width = 2;
            time_seg1 = 21;
            time_seg2 = 5;
            break;
        case CAN_BITRATE_5000K: // 40M / (1*(5+2+1)) = 5M, sample 6/8 = 75%
            prescaler = 1;
            sync_jump_width = 1;
            time_seg1 = 5;
            time_seg2 = 2;
            break;
        case CAN_BITRATE_1000K:
        default:
            prescaler = 1;
            sync_jump_width = 2;
            time_seg1 = 31;
            time_seg2 = 8;
            break;
    }
    led_green_on();
}


// Apply a nominal bitrate expressed in Hz (10k..1M + 83.3k, same set as the
// S<n> command). Accepts values within 2% of the table. When the bus is
// already open it is restarted so the new rate takes effect immediately.
// Returns 0 on success, -1 for an unsupported rate.
int32_t can_apply_bitrate_hz(uint32_t hz)
{
    // Macro-based lookup (no table). Exact Hz match against the supported
    // nominal rates; see CAN_BITRATE_HZ_* macros in can.h.
    enum can_bitrate match;
    if (hz == CAN_BITRATE_HZ_5000K)
        match = CAN_BITRATE_5000K;
    else if (hz == CAN_BITRATE_HZ_1000K)
        match = CAN_BITRATE_1000K;
    else if (hz == CAN_BITRATE_HZ_750K)
        match = CAN_BITRATE_750K;
    else if (hz == CAN_BITRATE_HZ_500K)
        match = CAN_BITRATE_500K;
    else if (hz == CAN_BITRATE_HZ_250K)
        match = CAN_BITRATE_250K;
    else if (hz == CAN_BITRATE_HZ_125K)
        match = CAN_BITRATE_125K;
    else if (hz == CAN_BITRATE_HZ_100K)
        match = CAN_BITRATE_100K;
    else if (hz == CAN_BITRATE_HZ_83_3K)
        match = CAN_BITRATE_83_3K;
    else if (hz == CAN_BITRATE_HZ_50K)
        match = CAN_BITRATE_50K;
    else if (hz == CAN_BITRATE_HZ_20K)
        match = CAN_BITRATE_20K;
    else if (hz == CAN_BITRATE_HZ_10K)
        match = CAN_BITRATE_10K;
    else
        return -1;

    if (bus_state == ON_BUS)
    {
        can_disable();          // stop & drop queues (set_bitrate is a no-op while ON_BUS)
        can_set_bitrate(match);
        can_enable();           // re-init, re-start, re-enable interrupts
    }
    else
    {
        can_set_bitrate(match);
    }
    return 0;
}


// Set CAN peripheral to silent mode
void can_set_silent(uint8_t silent)
{
    if (bus_state == ON_BUS) return;

    if (silent) {
        can_handle.Mode = FDCAN_MODE_BUS_MONITORING;
    } else {
        can_handle.Mode = FDCAN_MODE_NORMAL;
    }
    led_green_on();
}


// Enable/disable automatic retransmission
void can_set_autoretransmit(uint8_t autoretransmit)
{
    if (bus_state == ON_BUS) return;

    can_autoretransmit = autoretransmit;
    led_green_on();
}


// Queue a message for transmission on the CAN bus.
// Only pushes the frame into the software TX ring buffer; the actual bus
// transmission is driven by the periodic timer ISR (can_send_timer_tick).
// When the ring is full the OLDEST unsent frame is overwritten (drop-oldest).
uint32_t can_tx(FDCAN_TxHeaderType *tx_msg_header, uint8_t *tx_msg_data)
{
    // Convert DLC to bytes
    uint32_t len = hal_dlc_code_to_bytes(tx_msg_header->DataLength);
    if (len > TXQUEUE_DATALEN)
        return 1;

    // Guard head/tail against the timer ISR consuming frames concurrently.
    __disable_irq();

    // Ring full? Overwrite the oldest queued frame (advance tail).
    if (((txqueue.head + 1) % TXQUEUE_LEN) == txqueue.tail)
    {
        txqueue.tail = (uint16_t)((txqueue.tail + 1) % TXQUEUE_LEN);
    }

    // Save header to circular buffer
    txqueue.header[txqueue.head] = *tx_msg_header;

    // Copy data to circular buffer
    for (uint32_t i = 0; i < len; i++)
    {
        txqueue.data[txqueue.head][i] = tx_msg_data[i];
    }

    // Advance head pointer
    txqueue.head = (uint16_t)((txqueue.head + 1) % TXQUEUE_LEN);

    __enable_irq();

    return 0; // OK
}


// Drain the FDCAN hardware Rx FIFO 0 into the software RX queue.
// Called from the FDCAN1_INT0 ISR only (single producer).
// If the software ring is full the OLDEST queued frame is overwritten.
void can_rx_isr(void)
{
    while (FDCAN_GetRxFifoFillLevel(FDCAN1, FDCAN_RX_FIFO0) > 0)
    {
        uint16_t next = (uint16_t)((rxqueue.head + 1) % RXQUEUE_LEN);
        if (next == rxqueue.tail)
        {
            // Software ring full -> drop the oldest frame by advancing tail,
            // then store the new frame over the freed slot.
            rxqueue.tail = (uint16_t)((rxqueue.tail + 1) % RXQUEUE_LEN);
            error_assert(ERR_CANRXFIFO_OVERFLOW);
        }

        if (FDCAN_GetRxMsg(FDCAN1, FDCAN_RX_FIFO0,
                           &rxqueue.header[rxqueue.head],
                           rxqueue.data[rxqueue.head]) == SUCCESS)
        {
            rxqueue.head = next;
        }
        else
        {
            break;
        }
    }
}


// Receive a CAN message from the software RX queue (drained by main loop).
// Critical section guards against the FDCAN ISR overwriting the oldest frame
// while it is being copied out.
uint32_t can_rx(FDCAN_RxHeaderType *rx_msg_header, uint8_t *rx_msg_data)
{
    __disable_irq();

    if (rxqueue.head == rxqueue.tail)
    {
        __enable_irq();
        return ERROR;   // queue empty
    }

    *rx_msg_header = rxqueue.header[rxqueue.tail];

    int8_t len = hal_dlc_code_to_bytes(rx_msg_header->DataLength);
    if (len < 0 || len > RXQUEUE_DATALEN)
    {
        __enable_irq();
        return ERROR;
    }
    for (int8_t i = 0; i < len; i++)
        rx_msg_data[i] = rxqueue.data[rxqueue.tail][i];

    rxqueue.tail = (uint16_t)((rxqueue.tail + 1) % RXQUEUE_LEN);

    __enable_irq();
    return SUCCESS;
}


// Check if a CAN message is pending in the software RX queue
uint8_t is_can_msg_pending(uint8_t fifo)
{
    (void)fifo;   // always drained from software RX queue
    return (rxqueue.head != rxqueue.tail);
}


// Send one queued frame per call. Invoked by the periodic timer ISR
// (can_timer) once every configured interval. Sends nothing when:
//   - the CAN bus is not open (OFF_BUS)
//   - the TX ring is empty (no data from the host to send)
//   - the hardware TX FIFO has no free slot (retry on next tick)
void can_send_timer_tick(void)
{
    if (bus_state != ON_BUS)
        return;

    if (txqueue.tail == txqueue.head)
        return;   // nothing queued -> do not send

    if (FDCAN_GetTxFifoFreeLevel(FDCAN1) == 0)
        return;   // HW TX FIFO full -> try again next tick

    if (FDCAN_AddMsgToTxFifoQ(FDCAN1,
                               &txqueue.header[txqueue.tail],
                               txqueue.data[txqueue.tail]) == SUCCESS)
    {
        txqueue.tail = (uint16_t)((txqueue.tail + 1) % TXQUEUE_LEN);
    }
}
