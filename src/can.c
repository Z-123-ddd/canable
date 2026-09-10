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
#include "can_timer.h"
#include "can_frame_list.h"
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

// Binary USB protocol configuration (defaults used until a command changes them)
static can_id_format_t can_id_format = CAN_ID_FORMAT_STANDARD;
static can_send_mode_t can_send_mode = CAN_SEND_MODE_SINGLE;

// Software RX queue: FDCAN ISR pushes frames here, main loop drains them
static can_rxbuf_t rxqueue = {0};

// ---- TX job state (single shot / continuous) ----
// 'Most recent frame' cache: source for immediate single send and for the
// continuous single-frame job. Written by can_tx_data() (main-loop / USB
// context) under critical section; read by the timer ISR.
static FDCAN_TxHeaderType cur_tx_hdr;
static uint8_t cur_tx_data[64];
static uint8_t cur_tx_valid = 0;

// Running TX job state.  Updated only with interrupts disabled
// (can_tx_start / can_tx_stop); read by can_send_timer_tick.
static uint8_t tx_job_active = 0;          // 1 = a periodic job is being pumped
static uint8_t tx_job_single_pass = 0;     // FRAMELIST: 1 = stop after one pass
static uint16_t tx_list_pos = 0;           // FRAMELIST: FIFO cursor into active list

// ---- Auto bitrate scan state ----
#define CAN_SCAN_ERR_THRESHOLD   8u     // REC/TEC at/above this => bus trouble
#define CAN_SCAN_WINDOW_MS       120u   // dwell time per candidate bitrate (ms)
#define CAN_SCAN_RECENT_MS       200u   // "a frame was received recently" guard

// Candidates, tried in order (all supported nominal rates). Starts with the
// common rates then the rest.
static const enum can_bitrate can_scan_rates[] = {
    CAN_BITRATE_1000K, CAN_BITRATE_500K, CAN_BITRATE_250K, CAN_BITRATE_125K,
    CAN_BITRATE_100K,  CAN_BITRATE_83_3K, CAN_BITRATE_50K, CAN_BITRATE_20K,
    CAN_BITRATE_10K,   CAN_BITRATE_750K,  CAN_BITRATE_5000K,
};
#define CAN_SCAN_NUM  (sizeof(can_scan_rates) / sizeof(can_scan_rates[0]))

typedef enum { CAN_SCAN_IDLE = 0, CAN_SCAN_RUNNING, CAN_SCAN_LOCKED } can_scan_state_t;

static volatile uint32_t rx_frame_count = 0;    // frames stored by can_rx_isr
static uint32_t current_bitrate_hz = CAN_BITRATE_HZ_1000K;
static can_scan_state_t scan_state = CAN_SCAN_IDLE;
static uint8_t  scan_idx = 0;
static uint32_t scan_last_count = 0;
static uint32_t scan_last_rx_tick = 0;
static uint32_t scan_win_start = 0;
static uint32_t scan_win_base = 0;
static volatile int32_t scan_lock_hz = -1;


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


// ---- Binary USB protocol configuration (ID width) ----

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


// ---------------------------------------------------------------------------
// Low-level FDCAN hardware bring-up / teardown.
// These touch ONLY the peripheral and its interrupts - never the software
// caches - so they can be reused to hot-reconfigure the bitrate without
// dropping a queued TX frame or a running job (see can_apply_bitrate_hz()).
// ---------------------------------------------------------------------------

// Configure the FDCAN peripheral from the current bit-timing fields and start
// it: init + accept-all filter + timestamp + TX delay compensation + the
// RX FIFO0 / bus-off interrupts and their NVIC line.
static void can_hw_start(void)
{
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

// Deactivate FDCAN1 interrupts, stop and de-init the peripheral. Marks the
// bus OFF first so the periodic pump / can_tx_hw() bail out immediately.
static void can_hw_stop(void)
{
    bus_state = OFF_BUS;

    FDCAN_DeactivateInt(FDCAN1, FDCAN_INT_RX_FIFO0_NEW_MESSAGE | FDCAN_INT_BUS_OFF);
    FDCAN_Stop(FDCAN1);
    FDCAN_DeInit(FDCAN1);
}

// Drop the software-side queues / caches: RX queue, the single-frame TX cache,
// any running TX job and the frame lists. Used only on a clean open/close -
// a bitrate change must NOT call this (queued frames/jobs have to survive).
static void can_clear_software_state(void)
{
    rxqueue.head = 0;
    rxqueue.tail = 0;
    cur_tx_valid = 0;

    __disable_irq();
    tx_job_active = 0;
    tx_job_single_pass = 0;
    tx_list_pos = 0;
    __enable_irq();

    can_frame_list_init();
}

// Open the CAN channel: clean software state + hardware bring-up.
void can_enable(void)
{
    if (bus_state == OFF_BUS)
    {
        // De-init first to clear state
        FDCAN_DeInit(FDCAN1);

        // Fresh session: drop any stale software state
        can_clear_software_state();

        // Configure and start the peripheral
        can_hw_start();
    }
}

// Close the CAN channel: stop the hardware, then drop the software state.
void can_disable(void)
{
    if (bus_state == ON_BUS)
    {
        can_hw_stop();
        can_clear_software_state();
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

    current_bitrate_hz = hz;

    if (bus_state == ON_BUS)
    {
        // Hot-reconfigure the peripheral for the new nominal bitrate WITHOUT
        // dropping the software cache: a frame queued before the rate change
        // (and any running job / frame list) must survive so the host can
        // still send it. can_hw_stop() flips the bus OFF (which gates the
        // periodic pump) and can_hw_start() brings it back up; neither touches
        // the TX caches, so an active continuous job simply resumes.
        can_hw_stop();
        can_set_bitrate(match);
        can_hw_start();
    }
    else
    {
        can_set_bitrate(match);
    }
    return 0;
}


// ---- Auto bitrate scan implementation ----

// enum rate -> Hz
static uint32_t can_bitrate_to_hz(enum can_bitrate b)
{
    switch (b) {
        case CAN_BITRATE_10K:   return CAN_BITRATE_HZ_10K;
        case CAN_BITRATE_20K:   return CAN_BITRATE_HZ_20K;
        case CAN_BITRATE_50K:   return CAN_BITRATE_HZ_50K;
        case CAN_BITRATE_100K:  return CAN_BITRATE_HZ_100K;
        case CAN_BITRATE_125K:  return CAN_BITRATE_HZ_125K;
        case CAN_BITRATE_250K:  return CAN_BITRATE_HZ_250K;
        case CAN_BITRATE_500K:  return CAN_BITRATE_HZ_500K;
        case CAN_BITRATE_750K:  return CAN_BITRATE_HZ_750K;
        case CAN_BITRATE_1000K: return CAN_BITRATE_HZ_1000K;
        case CAN_BITRATE_5000K: return CAN_BITRATE_HZ_5000K;
        case CAN_BITRATE_83_3K:
        default:                return CAN_BITRATE_HZ_83_3K;
    }
}

// Bus trouble indicator: high error counters / error-passive. With the wrong
// nominal bitrate but live traffic the receive error counter climbs (frames
// fail form/stuff/CRC); at the right rate it stays low.
static uint8_t can_bus_error_detected(void)
{
    FDCAN_ErrorCounters ec;
    FDCAN_GetErrorCounters(FDCAN1, &ec);
    if (ec.RxErrorPassive)
        return 1;
    if (ec.RxErrorCnt >= CAN_SCAN_ERR_THRESHOLD)
        return 1;
    if (ec.TxErrorCnt >= CAN_SCAN_ERR_THRESHOLD)
        return 1;
    return 0;
}

// Switch to candidate slot idx and open a fresh observation window.
static void can_scan_switch(uint8_t idx, uint32_t now)
{
    uint32_t hz = can_bitrate_to_hz(can_scan_rates[idx]);
    can_apply_bitrate_hz(hz);          // hot reconfig; TX cache is preserved
    current_bitrate_hz = hz;
    scan_win_start = now;
    scan_win_base  = rx_frame_count;
}

void can_scan_poll(void)
{
    uint32_t now = system_get_ticks();

    // Track the most recent RX activity (frames stored by the ISR).
    if (rx_frame_count != scan_last_count) {
        scan_last_count = rx_frame_count;
        scan_last_rx_tick = now;
    }

    if (bus_state != ON_BUS)
        return;

    switch (scan_state) {
    case CAN_SCAN_IDLE:
        // Trigger: bus-error activity while nothing is being received.
        if (can_bus_error_detected() &&
            (now - scan_last_rx_tick) >= CAN_SCAN_RECENT_MS) {
            scan_idx = 0;
            scan_state = CAN_SCAN_RUNNING;
            can_scan_switch(0, now);
        }
        break;

    case CAN_SCAN_RUNNING:
        if (rx_frame_count != scan_win_base) {
            // A valid frame arrived at this rate -> lock it.
            scan_lock_hz = (int32_t)current_bitrate_hz;
            scan_state = CAN_SCAN_LOCKED;
        } else if ((now - scan_win_start) >= CAN_SCAN_WINDOW_MS) {
            scan_idx = (uint8_t)((scan_idx + 1u) % CAN_SCAN_NUM);
            can_scan_switch(scan_idx, now);
        }
        break;

    case CAN_SCAN_LOCKED:
    default:
        // Re-arm: if trouble appears again with no reception, rescan.
        if (can_bus_error_detected() &&
            (now - scan_last_rx_tick) >= CAN_SCAN_RECENT_MS) {
            scan_idx = 0;
            scan_state = CAN_SCAN_RUNNING;
            can_scan_switch(0, now);
        }
        break;
    }
}

int32_t can_scan_take_lock(void)
{
    int32_t hz;
    __disable_irq();
    hz = scan_lock_hz;
    scan_lock_hz = -1;
    __enable_irq();
    return hz;
}

uint32_t can_get_bitrate_hz(void)
{
    return current_bitrate_hz;
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


// ---------------------------------------------------------------------------
// TX engine: single-shot vs continuous (periodic) sending.
//
// Data frames from the USB host arrive via can_tx_data() and are cached in RAM
// as the 'most recent frame'; nothing is transmitted until a trigger starts a
// job. A job (can_tx_start) then makes the periodic timer ISR
// (can_send_timer_tick) transmit the cached frame:
//   trigger SINGLE     -> send the cached frame once (can_tx_start does it
//                          directly, no timer needed)
//   trigger CONTINUOUS -> re-send the cached frame on every timer tick
// ---------------------------------------------------------------------------

// Write one frame to the FDCAN hardware TX FIFO (fire-and-forget).
// Returns 1 when queued to the hardware, 0 when the bus is off / HW FIFO full.
static uint8_t can_tx_hw(const FDCAN_TxHeaderType *hdr, const uint8_t *data)
{
    if (bus_state != ON_BUS)
        return 0;
    if (FDCAN_GetTxFifoFreeLevel(FDCAN1) == 0)
        return 0;
    if (FDCAN_AddMsgToTxFifoQ(FDCAN1, (FDCAN_TxHeaderType *)hdr,
                               (uint8_t *)data) == SUCCESS)
        return 1;
    return 0;
}

// Host data-frame entry point (called from the binary USB protocol parser).
// A data frame only loads the cache in RAM - it is never transmitted
// directly; a TX command (can_tx_start / future USB command) decides when
// the cached source goes on the bus. In single mode the frame replaces the
// 'most recent frame'; in list mode it is appended to the pending burst
// (FIFO). A burst longer than the list capacity is refused.
void can_tx_data(const FDCAN_TxHeaderType *hdr, const uint8_t *data)
{
    if (hdr == NULL)
        return;

    if (can_send_mode == CAN_SEND_MODE_FRAMELIST)
    {
        // Append this frame to the pending burst (FIFO).
        if (can_frame_list_pending_add(hdr, data) != 0)
            error_assert(ERR_FRAMELIST_FULL);
        return;
    }

    // Single-frame mode: remember this frame as the most recent one.
    int8_t len = hal_dlc_code_to_bytes(hdr->DataLength);
    if (len < 0)
        return;
    if (len > (int8_t)sizeof(cur_tx_data))
        return;

    __disable_irq();
    cur_tx_hdr = *hdr;
    for (int8_t i = 0; i < len; i++)
        cur_tx_data[i] = data[i];
    cur_tx_valid = 1;
    __enable_irq();
}

// Start a transmit job on the cached source.
//   SINGLE     -> send the source once and stop. Single mode: the cached
//                 frame is fired immediately (no timer needed). List mode:
//                 the pending burst is committed (replacing the old list,
//                 or kept when no new data arrived) and then replayed once
//                 in FIFO order by the timer pump.
//   CONTINUOUS -> keep sending on every timer tick (single frame re-sent, or
//                 list stepped FIFO and wrapped).
// Returns 0 on success, -1 when there is nothing to send.
int32_t can_tx_start(can_tx_trigger_t trigger)
{
    if (can_send_mode == CAN_SEND_MODE_FRAMELIST)
    {
        // Commit: a non-empty pending burst replaces the active list; an empty
        // pending (host sent command without new data) keeps the last list.
        can_frame_list_commit();
        if (can_frame_list_active_count() == 0)
            return -1;   // nothing committed to send

        __disable_irq();
        tx_list_pos = 0;
        tx_job_single_pass = (trigger == CAN_TX_TRIGGER_SINGLE) ? 1 : 0;
        tx_job_active = 1;   // the timer pump steps through the active list
        __enable_irq();
        return 0;
    }

    // ---- single-frame mode ----
    __disable_irq();
    if (trigger == CAN_TX_TRIGGER_SINGLE)
    {
        if (!cur_tx_valid)
        {
            __enable_irq();
            return -1;
        }
        // Single shot of the most recent frame: no periodic job needed.
        tx_job_active = 0;
        __enable_irq();
        return can_tx_hw(&cur_tx_hdr, cur_tx_data) ? 0 : -1;
    }
    else /* CONTINUOUS */
    {
        if (!cur_tx_valid)
        {
            __enable_irq();
            return -1;
        }
        tx_job_active = 1;
    }

    __enable_irq();
    return 0;
}

// Abort any running TX job (stops the periodic pump) and disables the
// periodic timer so no tick is serviced while idle.
void can_tx_stop(void)
{
    __disable_irq();
    tx_job_active = 0;
    tx_job_single_pass = 0;
    tx_list_pos = 0;
    __enable_irq();
    can_timer_disable();
}

// True while a periodic TX job is running.
uint8_t can_tx_job_running(void)
{
    return tx_job_active;
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
            rx_frame_count++;
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


// Periodic pump, invoked by the GTIM1 timer ISR at the configured interval.
// Only acts while a periodic TX job is active (see can_tx_start CONTINUOUS):
// re-send the cached 'most recent frame'. A SINGLE job is fired directly in
// can_tx_start and never activates the timer pump.
// Periodic pump, invoked by the GTIM1 timer ISR at the configured interval.
// Only acts while a TX job is active (see can_tx_start):
//   single-frame mode  -> re-send the cached 'most recent frame'
//   list mode          -> send the next active-list frame (FIFO); stop after
//                          one pass (SINGLE) or wrap and keep cycling
//                          (CONTINUOUS).
void can_send_timer_tick(void)
{
    if (bus_state != ON_BUS)
        return;

    if (!tx_job_active)
        return;

    if (can_send_mode == CAN_SEND_MODE_FRAMELIST)
    {
        uint16_t n = can_frame_list_active_count();
        if (n == 0)
        {
            can_tx_stop();
            return;
        }

        if (tx_list_pos >= n)
            tx_list_pos = 0;

        const can_frame_list_frame_t *f = can_frame_list_active_get(tx_list_pos);
        if (f == NULL)
        {
            can_tx_stop();
            return;
        }

        if (!can_tx_hw(&f->header, f->data))
            return;   // HW FIFO full: retry on the next tick (cursor not moved)

        tx_list_pos++;
        if (tx_job_single_pass)
        {
            // One FIFO pass only: stop once the whole list has been sent.
            if (tx_list_pos >= n)
                can_tx_stop();
        }
        else /* CONTINUOUS */
        {
            if (tx_list_pos >= n)
                tx_list_pos = 0;   // wrap around and keep cycling
        }
        return;
    }

    // single-frame mode: only a CONTINUOUS job reaches here (SINGLE is fired
    // directly in can_tx_start and never activates the timer pump).
    if (!cur_tx_valid)
        return;
    can_tx_hw(&cur_tx_hdr, cur_tx_data);
}


// ---- Host status reporting (GET_STATUS command) ----
// One-line ASCII report so the host can see, after a command, whether the
// frame was cached, a job is running, the timer is gated on, and whether any
// error bit is set (e.g. ERR_USB_BADFRAME when a command id is unknown).

static void st_hex8(char *p, uint32_t v)
{
    for (int i = 7; i >= 0; i--)
    {
        uint8_t n = (uint8_t)((v >> (i * 4)) & 0xFu);
        p[7 - i] = (n < 10u) ? (char)('0' + n) : (char)('A' + n - 10);
    }
}

static void st_put(char *buf, uint16_t max, uint16_t *n, char c)
{
    if (*n + 1u < max)
        buf[(*n)++] = c;
}

// append an unsigned decimal number (1-2 digits, up to 99)
static void st_put_u16(char *buf, uint16_t max, uint16_t *n, uint16_t v)
{
    if (v >= 100u)
        v = 99u;
    if (v >= 10u)
        st_put(buf, max, n, (char)('0' + (v / 10u)));
    st_put(buf, max, n, (char)('0' + (v % 10u)));
}

uint16_t can_format_status(char *buf, uint16_t max)
{
    if (buf == NULL || max < 2)
        return 0;

    char err[9];
    st_hex8(err, error_reg());
    err[8] = 0;

    static const char hdr[] = "ST:bus=";
    static const char mode_s[] = " mode=S";
    static const char mode_l[] = " mode=L";
    static const char j0[] = " job=0";
    static const char j1[] = " job=1";
    static const char t0[] = " tmr=0";
    static const char t1[] = " tmr=1";
    static const char c0[] = " cur=0";
    static const char c1[] = " cur=1";
    static const char pend[] = " pend=";
    static const char act[] = " act=";
    static const char errt[] = " err=";

    uint16_t n = 0;
    uint16_t i;
    const char *q;

    for (i = 0; hdr[i]; i++) st_put(buf, max, &n, hdr[i]);
    st_put(buf, max, &n, (char)('0' + (bus_state == ON_BUS ? 1 : 0)));

    q = (can_send_mode == CAN_SEND_MODE_FRAMELIST) ? mode_l : mode_s;
    for (i = 0; q[i]; i++) st_put(buf, max, &n, q[i]);

    q = tx_job_active ? j1 : j0;
    for (i = 0; q[i]; i++) st_put(buf, max, &n, q[i]);

    q = can_timer_is_enabled() ? t1 : t0;
    for (i = 0; q[i]; i++) st_put(buf, max, &n, q[i]);

    q = cur_tx_valid ? c1 : c0;
    for (i = 0; q[i]; i++) st_put(buf, max, &n, q[i]);

    for (i = 0; pend[i]; i++) st_put(buf, max, &n, pend[i]);
    st_put_u16(buf, max, &n, (uint16_t)can_frame_list_pending_count());

    for (i = 0; act[i]; i++) st_put(buf, max, &n, act[i]);
    st_put_u16(buf, max, &n, (uint16_t)can_frame_list_active_count());

    for (i = 0; errt[i]; i++) st_put(buf, max, &n, errt[i]);
    for (i = 0; i < 8 && n + 1u < max; i++)
        buf[n++] = err[i];

    st_put(buf, max, &n, '\r');
    buf[n] = 0;
    return n;
}
