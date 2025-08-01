// SPDX-License-Identifier: MIT
/*
 * Copyright 2021 Álvaro Fernández Rojas <noltari@gmail.com>
 * Cleanup/modifications Copyright 2023 Andrew J. Kroll <xxxajk at gmail>
 *
 */

#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/structs/sio.h>
#include <hardware/uart.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <string.h>
#include <tusb.h>

#if !defined(MIN)
#define MIN(a, b) ((a > b) ? b : a)
#endif /* MIN */

#define BUFFER_SIZE 2560

#define UART1_TX 16
#define UART1_RX 17

#define DEF_BIT_RATE 115200
#define DEF_STOP_BITS 1
#define DEF_PARITY 0
#define DEF_DATA_BITS 8

#define USB_MODE_DEFAULT 0
#define USB_MODE_CMSISDAP 1
#define USB_MODE_UART 2
extern volatile int usbMode;

typedef struct {
  uart_inst_t * inst;
  uint irq;
  void *irq_fn;
  uint8_t tx_pin;
  uint8_t rx_pin;
} uart_id_t;

typedef struct {
  cdc_line_coding_t usb_lc;
  cdc_line_coding_t uart_lc;
  mutex_t lc_mtx;
  uint8_t uart_buffer[BUFFER_SIZE];
  uint32_t uart_pos;
  mutex_t uart_mtx;
  uint8_t usb_buffer[BUFFER_SIZE];
  uint32_t usb_pos;
  mutex_t usb_mtx;
} uart_data_t;

void uart0_irq_fn(void);

uart_id_t UART_ID[1] = {{
    .inst = uart0,
    .irq = UART0_IRQ,
    .irq_fn = &uart0_irq_fn,
    .tx_pin = UART1_TX,
    .rx_pin = UART1_RX,
}};

uart_data_t UART_DATA[1];
volatile bool ready = false;

static inline uint databits_usb2uart(uint8_t data_bits) {
  switch (data_bits) {
    case 5:
      return 5;
    case 6:
      return 6;
    case 7:
      return 7;
    default:
      return 8;
  }
}

static inline uart_parity_t parity_usb2uart(uint8_t usb_parity) {
  switch (usb_parity) {
    case 1:
      return UART_PARITY_ODD;
    case 2:
      return UART_PARITY_EVEN;
    default:
      return UART_PARITY_NONE;
  }
}

static inline uint stopbits_usb2uart(uint8_t stop_bits) {
  switch (stop_bits) {
    case 2:
      return 2;
    default:
      return 1;
  }
}

void update_uart_cfg(uint8_t itf) {
  uart_id_t *ui = &UART_ID[itf];
  uart_data_t *ud = &UART_DATA[itf];

  mutex_enter_blocking(&ud->lc_mtx);

  if (ud->usb_lc.bit_rate != ud->uart_lc.bit_rate) {
    uart_set_baudrate(ui->inst, ud->usb_lc.bit_rate);
    ud->uart_lc.bit_rate = ud->usb_lc.bit_rate;
  }

  if ((ud->usb_lc.stop_bits != ud->uart_lc.stop_bits) ||
      (ud->usb_lc.parity != ud->uart_lc.parity) ||
      (ud->usb_lc.data_bits != ud->uart_lc.data_bits)) {
    uart_set_format(ui->inst, databits_usb2uart(ud->usb_lc.data_bits),
                    stopbits_usb2uart(ud->usb_lc.stop_bits),
                    parity_usb2uart(ud->usb_lc.parity));
    ud->uart_lc.data_bits = ud->usb_lc.data_bits;
    ud->uart_lc.parity = ud->usb_lc.parity;
    ud->uart_lc.stop_bits = ud->usb_lc.stop_bits;
  }

  mutex_exit(&ud->lc_mtx);
}

void usb_read_bytes(uint8_t itf) {
  uart_data_t *ud = &UART_DATA[itf];
  uint32_t len = tud_cdc_n_available(itf);

  if (len && mutex_try_enter(&ud->usb_mtx, NULL)) {
    len = MIN(len, BUFFER_SIZE - ud->usb_pos);
    if (len) {
      uint32_t count;
      count = tud_cdc_n_read(itf, &ud->usb_buffer[ud->usb_pos], len);
      ud->usb_pos += count;
    }

    mutex_exit(&ud->usb_mtx);
  }
}

void usb_write_bytes(uint8_t itf) {
  uart_data_t *ud = &UART_DATA[itf];

  if (ud->uart_pos) {
    if (mutex_try_enter(&ud->uart_mtx, NULL)) {
      uint32_t count = tud_cdc_n_write(itf, ud->uart_buffer, ud->uart_pos);
      if (count < ud->uart_pos) {
        memmove(ud->uart_buffer, &ud->uart_buffer[count], ud->uart_pos - count);
      }
      ud->uart_pos -= count;
      mutex_exit(&ud->uart_mtx);

      if (count) tud_cdc_n_write_flush(itf);
    }
  }
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms) {
  uart_id_t *ui = &UART_ID[itf];
  uart_data_t *ud = &UART_DATA[itf];

  if (duration_ms == 0xffff) {
    uart_set_break(ui->inst, true);
  } else if (duration_ms == 0x0000) {
    uart_set_break(ui->inst, false);
  } else {
    uart_set_break(ui->inst, true);
    sleep_ms(duration_ms);
    uart_set_break(ui->inst, false);
  }
}

void usb_cdc_process(uint8_t itf) {
  uart_data_t *ud = &UART_DATA[itf];
  mutex_enter_blocking(&ud->lc_mtx);
  tud_cdc_n_get_line_coding(itf, &ud->usb_lc);
  mutex_exit(&ud->lc_mtx);

  usb_read_bytes(itf);
  usb_write_bytes(itf);
}

void core1_entry(void) {
  ready = true;

  if (usbMode == USB_MODE_DEFAULT || usbMode == USB_MODE_UART ) {
    while (1) {
      tud_task();
      if (tud_ready()) {
        usb_cdc_process(0);
      }
    }
  }
}

static inline void uart_read_bytes(uint8_t itf) {
  uart_data_t *ud = &UART_DATA[itf];
  uart_id_t *ui = &UART_ID[itf];

  if (uart_is_readable(ui->inst)) {
    mutex_enter_blocking(&ud->uart_mtx);
    if (ud->uart_pos < BUFFER_SIZE) {
      ud->uart_buffer[ud->uart_pos] = uart_getc(ui->inst);
      ud->uart_pos++;
    } else {
      uart_getc(ui->inst);
    }
    mutex_exit(&ud->uart_mtx);
  }
}

void uart_write_bytes(uint8_t itf) {
  uart_data_t *ud = &UART_DATA[itf];
  if (ud->usb_pos && mutex_try_enter(&ud->usb_mtx, NULL)) {
    uart_id_t *ui = &UART_ID[itf];
    if (uart_is_writable(ui->inst)) {
      uart_putc_raw(ui->inst, ud->usb_buffer[0]);
      if (ud->usb_pos > 1) {
        memmove(ud->usb_buffer, &ud->usb_buffer[1], ud->usb_pos - 1);
      }
      ud->usb_pos--;
    }
    mutex_exit(&ud->usb_mtx);
  }
}

void uart0_irq_fn(void) 
{ 
  if (usbMode == USB_MODE_DEFAULT || usbMode == USB_MODE_UART)
  {
    uart_read_bytes(0);
  } 
}

void init_uart_data(uint8_t itf) {
  uart_data_t *ud = &UART_DATA[itf];

  // USB CDC default config
  ud->usb_lc.bit_rate = DEF_BIT_RATE;
  ud->usb_lc.data_bits = DEF_DATA_BITS;
  ud->usb_lc.parity = DEF_PARITY;
  ud->usb_lc.stop_bits = DEF_STOP_BITS;

  // UART LC mirrors USB
  ud->uart_lc = ud->usb_lc;

  // Buffers
  ud->uart_pos = 0;
  ud->usb_pos = 0;

  // Mutexes
  mutex_init(&ud->lc_mtx);
  mutex_init(&ud->uart_mtx);
  mutex_init(&ud->usb_mtx);
}

void reconfigure_uart_pins(uint8_t itf, int mode) {
  uart_id_t *ui = &UART_ID[itf];
  uart_data_t *ud = &UART_DATA[itf];

  // Disable UART before reconfiguring
  uart_deinit(ui->inst);

  // Update pin numbers based on USB mode
  if (mode == USB_MODE_DEFAULT) {
    ui->tx_pin = 16;
    ui->rx_pin = 17;
  } else if (mode == USB_MODE_UART) {
    ui->tx_pin = 0;
    ui->rx_pin = 1;
  }

  // Configure GPIO pins
  gpio_set_function(ui->tx_pin, GPIO_FUNC_UART);
  gpio_set_function(ui->rx_pin, GPIO_FUNC_UART);
  gpio_pull_up(ui->rx_pin);

  // Init UART with current config
  uart_init(ui->inst, ud->usb_lc.bit_rate);
  uart_set_hw_flow(ui->inst, false, false);
  uart_set_format(ui->inst, databits_usb2uart(ud->usb_lc.data_bits),
                  stopbits_usb2uart(ud->usb_lc.stop_bits),
                  parity_usb2uart(ud->usb_lc.parity));
  uart_set_fifo_enabled(ui->inst, false);
  uart_set_translate_crlf(ui->inst, false);

  // IRQ setup
  irq_set_exclusive_handler(ui->irq, ui->irq_fn);
  irq_set_enabled(ui->irq, true);
  uart_set_irq_enables(ui->inst, true, false);
}

void start_uarts() {
  init_uart_data(0);                    // Setup buffers and locks
  reconfigure_uart_pins(0, usbMode);   // Actual UART + GPIO setup
}

void uartBootMode(void) {
  int itf;
  set_sys_clock_khz(125000, false);
  tusb_init();  
  multicore_reset_core1();
  start_uarts();
  
  multicore_launch_core1(core1_entry);
  do {
    sleep_us(1);
  } while (!ready);

  while (1) {
    if (tud_ready()) {
      for (itf = 0; itf < CFG_TUD_CDC; itf++) {
        update_uart_cfg(itf);
        uart_write_bytes(itf);
      }
    }
  }
}

void uartMode(void) {
  set_sys_clock_khz(125000, false);
  start_uarts();
  int itf;
  do {
    sleep_us(1);
  } while (!ready);

  while (1) {
    if (tud_ready()) {
      for (itf = 0; itf < CFG_TUD_CDC; itf++) {
        update_uart_cfg(itf);
        uart_write_bytes(itf);
      }
    }
  }
}


int initUART(void)
{
        int itf;
        tusb_init();    
        for (itf = 0; itf < CFG_TUD_CDC; itf++)
        {
                init_uart_data(itf);
        }
        multicore_launch_core1(core1_entry);
        return 0;
}


void initUARTt(void) {
  int itf;
  multicore_reset_core1();
  for (itf = 0; itf < CFG_TUD_CDC; itf++) {
    init_uart_data(itf);
  }

  for (itf = 0; itf < CFG_TUD_CDC; itf++) {
    uart_id_t *ui = &UART_ID[itf];
    irq_set_enabled(ui->irq, true);
    uart_set_irq_enables(ui->inst, true, false);
  }

   
  multicore_launch_core1(core1_entry);

  do {
    sleep_us(1);
  } while (!ready);

}