/***************************************************************************//**
 * NTC_meas — BRD2601 + MCP3425A0 + 47k NTC (B=3960)
 *
 * All application code is in this file. sl_main_init() still comes from the
 * Silabs SDK (clocks, I2C pins PC04/PC05, VCOM).
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sl_board_control.h"
#include "sl_i2cspm.h"
#include "sl_i2cspm_instances.h"
#include "sl_main_init.h"
#include "sl_main_process_action.h"
#include "sl_simple_led.h"
#include "sl_simple_led_instances.h"
#include "sl_sleeptimer.h"

/* ---- hardware / NTC (change these) ---- */
#define MCP3425_ADDR            0x68
#define MCP3425_RESOLUTION_BITS 16        /* 12, 14 or 16 — not 18 */
#define MCP3425_VREF            2.048f
#define NTC_VDD                 3.3f
#define NTC_RSERIES             50000.0f
#define NTC_R25                 47000.0f
#define NTC_BETA                3960.0f

#if MCP3425_RESOLUTION_BITS == 12
#define ADC_CONFIG  0x90
#define ADC_WAIT_MS 20
#elif MCP3425_RESOLUTION_BITS == 14
#define ADC_CONFIG  0x94
#define ADC_WAIT_MS 40
#elif MCP3425_RESOLUTION_BITS == 16
#define ADC_CONFIG  0x98
#define ADC_WAIT_MS 80
#else
#error "MCP3425_RESOLUTION_BITS must be 12, 14 or 16"
#endif

static sl_sleeptimer_timer_handle_t sample_timer;
static volatile bool sample_due;

static I2C_TransferReturn_TypeDef i2c(uint8_t addr7, uint16_t flags,
                                      uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len)
{
  I2C_TransferSeq_TypeDef seq = { 0 };

  seq.addr = addr7 << 1;
  seq.flags = flags;
  if (flags == I2C_FLAG_READ) {
    seq.buf[0].data = rx;
    seq.buf[0].len = rx_len;
  } else {
    seq.buf[0].data = tx;
    seq.buf[0].len = tx_len;
    seq.buf[1].data = rx;
    seq.buf[1].len = rx_len;
  }
  return I2CSPM_Transfer(sl_i2cspm_sensor, &seq);
}

static void i2c_scan(void)
{
  uint8_t dummy = 0;

  printf("I2C scan:\r\n");
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    if (i2c(addr, I2C_FLAG_WRITE, &dummy, 1, NULL, 0) == i2cTransferDone) {
      printf("  ACK 0x%02X\r\n", addr);
    }
  }
}

static bool adc_start(void)
{
  uint8_t cfg = ADC_CONFIG;

  if (i2c(MCP3425_ADDR, I2C_FLAG_WRITE, &cfg, 1, NULL, 0) != i2cTransferDone) {
    printf("ADC config write failed\r\n");
    return false;
  }
  sl_sleeptimer_delay_millisecond(ADC_WAIT_MS);
  printf("ADC cfg=0x%02X (%d-bit continuous)\r\n", cfg, MCP3425_RESOLUTION_BITS);
  return true;
}

static bool adc_read(int16_t *code, uint8_t *cfg)
{
  uint8_t rx[3];

  if (i2c(MCP3425_ADDR, I2C_FLAG_READ, NULL, 0, rx, 3) != i2cTransferDone) {
    return false;
  }
  *code = (int16_t)(((uint16_t)rx[0] << 8) | rx[1]);
  *cfg = rx[2];
  return true;
}

/* cfg bits 3-2 = 12/14/16. 12-bit may be left- or right-justified. */
static bool code_to_temp(int16_t code, uint8_t cfg,
                         float *vtherm, float *rntc, float *t_c, int *bits)
{
  uint16_t raw = (uint16_t)code;
  int32_t aligned;
  int32_t fs;
  int b;

  switch ((cfg >> 2) & 3) {
    case 0:
      b = 12;
      fs = 2048;
      if ((raw & 0xF) == 0) {
        raw >>= 4;
      }
      raw &= 0xFFF;
      aligned = (raw & 0x800) ? (int32_t)raw - 4096 : (int32_t)raw;
      break;
    case 1:
      b = 14;
      fs = 8192;
      if ((raw & 3) == 0) {
        raw >>= 2;
      }
      raw &= 0x3FFF;
      aligned = (raw & 0x2000) ? (int32_t)raw - 16384 : (int32_t)raw;
      break;
    default:
      b = 16;
      fs = 32768;
      aligned = code;
      break;
  }
  *bits = b;
  if (aligned <= 0) {
    return false;
  }

  *vtherm = ((float)aligned / (float)fs) * MCP3425_VREF;
  if (*vtherm >= (MCP3425_VREF - 0.002f) || aligned >= (fs - 2)) {
    return false;
  }

  *rntc = NTC_RSERIES * (*vtherm) / (NTC_VDD - *vtherm);
  *t_c = (1.0f / ((1.0f / 298.15f) + (logf(*rntc / NTC_R25) / NTC_BETA))) - 273.15f;
  return true;
}

static void sample_once(void)
{
  int16_t code;
  uint8_t cfg;
  int bits;
  float v, r, t;

  sl_led_toggle(&sl_led_led0);

  if (!adc_read(&code, &cfg)) {
    printf("I2C read failed\r\n");
    return;
  }
  if (!code_to_temp(code, cfg, &v, &r, &t, &bits)) {
    printf("code=%d cfg=0x%02X %dbit  (sat or invalid)\r\n", code, cfg, bits);
    return;
  }
  printf("code=%d cfg=0x%02X %dbit  V=%.3f V  R=%.0f ohm  T=%.2f C\r\n",
         code, cfg, bits, v, r, t);
}

static void on_tick(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  sample_due = true;
}

int main(void)
{
  sl_main_init();

  printf("\r\n*** NTC_meas BUILD 10 — single file ***\r\n");
  sl_board_enable_sensor(SL_BOARD_SENSOR_RHT);
  sl_sleeptimer_delay_millisecond(80);

  i2c_scan();
  adc_start();
  sample_once();
  sl_sleeptimer_start_periodic_timer(&sample_timer,
                                     sl_sleeptimer_ms_to_tick(1000),
                                     on_tick, NULL, 0, 0);

  while (1) {
    sl_main_process_action();
    if (sample_due) {
      sample_due = false;
      sample_once();
    }
  }
}
