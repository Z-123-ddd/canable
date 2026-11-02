/**
 * @file    rs485.h
 * @brief   RS485 half-duplex driver for CANable2-N32 (USART1 + DMA + DE)
 *
 * Hardware (per board wiring):
 *   PA9  = USART1_TX  (AF5)
 *   PA10 = USART1_RX  (AF5)
 *   PC6  = DE / RE direction control (output):  high = transmit, low = receive
 *
 * TX and RX both use DMA (DMA1 Channel4 = USART1_TX, DMA1 Channel5 = USART1_RX,
 * matching the N32H47x USART1 request table). The DE pin is driven low (receive)
 * by default; a transmit asserts it and it is released again only after the last
 * stop bit has shifted out (USART TC), so the bus is never driven while idle.
 *
 * Reference: proj1 (ODrive) Board/v3 usart.c + communication/rs485 modbus port.
 */

#ifndef _RS485_H
#define _RS485_H

#include <stdint.h>

/* Internal buffer sizes */
#define RS485_TX_BUFFER_SIZE   256U
#define RS485_RX_BUFFER_SIZE   256U

/**
 * @brief  Initialise USART1 (PA9/PA10), the DE pin (PC6) and DMA, then arm the
 *         RX DMA. DE is left in the receive state.
 * @param  baud  baud rate in bit/s (e.g. 115200)
 */
void rs485_init(uint32_t baud);

/**
 * @brief  Queue a buffer for transmission (copied into an internal staging
 *         buffer, so the caller's buffer may be reused immediately).
 * @return 0 on success; -1 if a transfer is still in progress or len is 0 /
 *         larger than RS485_TX_BUFFER_SIZE.
 */
int32_t rs485_send(const uint8_t *data, uint16_t len);

/** @return 1 while a DMA transmission is in progress (DE still asserted), else 0. */
uint8_t rs485_tx_busy(void);

/** @brief  Stop the RS485 (disable DMA channels + USART, park DE in receive).
 *  Safe to call even if rs485_init() was never called. */
void rs485_stop(void);

/**
 * @brief  Enable or disable the USART1 interrupt line in the NVIC.
 *
 * rs485_init() runs at boot even while the adapter is in CAN mode.  With an
 * undriven/floating RS485 RX line the USART asserts IDLE and framing/overrun
 * flags continuously, and because USART1_IRQHandler only clears TC the
 * interrupt is re-entered forever - that starves the main loop and takes the
 * whole device down (USB included).  The line is therefore only enabled once
 * RS485 mode is actually selected, and disabled again when leaving it.
 * @param  state  ENABLE while in RS485 mode, DISABLE otherwise
 */
void rs485_irq_set(int state);

/** @return number of received bytes currently available in the RX ring. */
uint16_t rs485_rx_available(void);

/** @brief  Copy up to @p max received bytes out of the RX ring.
 *  @return the number of bytes actually copied. */
uint16_t rs485_rx_read(uint8_t *out, uint16_t max);

#endif /* _RS485_H */
