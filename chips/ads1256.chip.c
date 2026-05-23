// Wokwi custom chip that emulates the ADS1256 24-bit delta-sigma ADC.
//
// Role in the signal chain:
//   Instrumentation Amplifier (instamp.chip.c) --> ADS1256 (this chip) --> Arduino via SPI
//
// Implements the SPI/register/DRDY behavior expected by the Curious Scientist ADS1256 library.
// Each diagram instance allocates its own chip_state_t (required for multi-ADC setups).

#include "wokwi-api.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define REG_COUNT 11u

#define REG_STATUS 0x00
#define REG_MUX    0x01
#define REG_ADCON  0x02
#define REG_DRATE  0x03
#define REG_IO     0x04

#define COUNTS_FULL_SCALE 8388608.0f

typedef struct {
  pin_t pin_ain0;
  pin_t pin_ain1;
  pin_t pin_vrefp;
  pin_t pin_vrefn;

  pin_t pin_cs;
  pin_t pin_sck;
  pin_t pin_din;
  pin_t pin_dout;
  pin_t pin_drdy;
  pin_t pin_reset;

  uint8_t regs[REG_COUNT];

  bool rdatas_mode;
  bool drdy_ready;  // true when DRDY pin is LOW (conversion data available)

  uint8_t rx_byte;
  uint8_t rx_bit_count;

  uint8_t tx_buf[4];
  uint8_t tx_len;
  uint8_t tx_idx;
  uint8_t tx_bit;

  bool expect_wreg_count;
  bool expect_wreg_data;
  bool expect_rreg_count;
  uint8_t wreg_addr;
  uint8_t wreg_count;
  uint8_t rreg_addr;

  uint8_t staged_data[3];
  bool data_staged;

  timer_t drdy_timer;
} chip_state_t;

static uint32_t drate_period_us(uint8_t drate) {
  switch (drate) {
    case 0xF0: return 34;
    case 0xE0: return 67;
    case 0xD0: return 100;
    case 0xC0: return 133;
    case 0xB0: return 200;
    case 0xA1: return 1000;
    case 0x92: return 2000;
    case 0x82: return 10000;
    case 0x63: return 20000;
    case 0x43: return 40000;
    case 0x33: return 50000;
    case 0x23: return 66667;
    case 0x13: return 200000;
    case 0x03: return 500000;
    default:   return 10000;
  }
}

static int32_t clamp24(int32_t value) {
  if (value > 8388607) return 8388607;
  if (value < -8388608) return -8388608;
  return value;
}

static float read_vref(chip_state_t *chip) {
  float vrefp = pin_adc_read(chip->pin_vrefp);
  float vrefn = pin_adc_read(chip->pin_vrefn);
  float vref = vrefp - vrefn;
  if (vref < 0.01f) vref = 2.5f;
  return vref;
}

static float mux_differential_volts(chip_state_t *chip) {
  float vp = pin_adc_read(chip->pin_ain0);
  float vn = pin_adc_read(chip->pin_ain1);
  float vcom = pin_adc_read(chip->pin_vrefn);
  uint8_t mux = chip->regs[REG_MUX];

  switch (mux) {
    case 0x01:
      return vp - vn;
    case 0x23:
      return vp - vn;
    case 0x45:
      return vp - vn;
    case 0x67:
      return vp - vn;
    case 0x0F:
      return vp - vcom;
    case 0x1F:
      return vn - vcom;
    case 0x2F:
    case 0x3F:
    case 0x4F:
    case 0x5F:
    case 0x6F:
    case 0x7F:
      return vp - vcom;
    default:
      return vp - vn;
  }
}

static void pack_counts(chip_state_t *chip, int32_t counts) {
  uint32_t raw24 = (uint32_t)counts & 0x00FFFFFFu;
  chip->staged_data[0] = (uint8_t)((raw24 >> 16) & 0xFFu);
  chip->staged_data[1] = (uint8_t)((raw24 >> 8) & 0xFFu);
  chip->staged_data[2] = (uint8_t)(raw24 & 0xFFu);
  chip->data_staged = true;
}

static void load_conversion_result(chip_state_t *chip) {
  float vref = read_vref(chip);
  int pga = chip->regs[REG_ADCON] & 0x07;
  float vdiff = mux_differential_volts(chip);
  float scale = (COUNTS_FULL_SCALE * (float)(1u << pga)) / (2.0f * vref);
  int32_t counts = clamp24((int32_t)(vdiff * scale));
  pack_counts(chip, counts);
}

static void set_drdy_ready(chip_state_t *chip, bool ready) {
  chip->drdy_ready = ready;
  pin_write(chip->pin_drdy, ready ? LOW : HIGH);
}

static void schedule_next_conversion(chip_state_t *chip);
static void on_tx_complete(chip_state_t *chip);
static void drdy_after_host_access(chip_state_t *chip);
static void reset_protocol_state(chip_state_t *chip);
static void reset_register_defaults(chip_state_t *chip);
static void reset_device_state(chip_state_t *chip);
static void queue_tx_bytes(chip_state_t *chip, const uint8_t *data, uint8_t len);
static void shift_out_next_bit(chip_state_t *chip);
static void handle_received_byte(chip_state_t *chip, uint8_t data);

static void drdy_timer_callback(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  if (!chip->drdy_ready) {
    if (!chip->data_staged) {
      load_conversion_result(chip);
    }
    set_drdy_ready(chip, true);
  }
}

static void schedule_next_conversion(chip_state_t *chip) {
  uint32_t period = drate_period_us(chip->regs[REG_DRATE]);
  timer_start(chip->drdy_timer, period, false);
}

static void drdy_after_host_access(chip_state_t *chip) {
  set_drdy_ready(chip, false);
  schedule_next_conversion(chip);
}

static void on_tx_complete(chip_state_t *chip) {
  chip->tx_len = 0;
  chip->tx_idx = 0;
  chip->tx_bit = 0;
  pin_mode(chip->pin_dout, INPUT);
  chip->data_staged = false;
  drdy_after_host_access(chip);
}

static void reset_protocol_state(chip_state_t *chip) {
  chip->rx_byte = 0;
  chip->rx_bit_count = 0;
  chip->expect_wreg_count = false;
  chip->expect_wreg_data = false;
  chip->expect_rreg_count = false;
  chip->wreg_addr = 0;
  chip->wreg_count = 0;
  chip->rreg_addr = 0;
  chip->tx_len = 0;
  chip->tx_idx = 0;
  chip->tx_bit = 0;
  pin_mode(chip->pin_dout, INPUT);
}

static void reset_register_defaults(chip_state_t *chip) {
  chip->regs[REG_STATUS] = 0x30;
  chip->regs[REG_MUX] = 0x01;
  chip->regs[REG_ADCON] = 0x20;
  chip->regs[REG_DRATE] = 0xA0;
  chip->regs[REG_IO] = 0xE0;
  for (uint8_t i = 5; i < REG_COUNT; i++) {
    chip->regs[i] = 0;
  }
}

static void reset_device_state(chip_state_t *chip) {
  reset_register_defaults(chip);
  chip->rdatas_mode = false;
  chip->data_staged = false;
  reset_protocol_state(chip);
  set_drdy_ready(chip, true);
  load_conversion_result(chip);
}

static void queue_tx_bytes(chip_state_t *chip, const uint8_t *data, uint8_t len) {
  if (len > sizeof(chip->tx_buf)) len = (uint8_t)sizeof(chip->tx_buf);
  for (uint8_t i = 0; i < len; i++) {
    chip->tx_buf[i] = data[i];
  }
  chip->tx_len = len;
  chip->tx_idx = 0;
  chip->tx_bit = 0;
  pin_mode(chip->pin_dout, OUTPUT);
}

static void shift_out_next_bit(chip_state_t *chip) {
  uint8_t out_bit = 0;
  if (chip->tx_idx < chip->tx_len) {
    uint8_t current = chip->tx_buf[chip->tx_idx];
    out_bit = (uint8_t)((current >> (7 - chip->tx_bit)) & 0x01u);
    chip->tx_bit++;
    if (chip->tx_bit >= 8) {
      chip->tx_bit = 0;
      chip->tx_idx++;
      if (chip->tx_idx >= chip->tx_len) {
        on_tx_complete(chip);
      }
    }
  }
  pin_write(chip->pin_dout, out_bit ? HIGH : LOW);
}

static void handle_cal_command(chip_state_t *chip) {
  load_conversion_result(chip);
  set_drdy_ready(chip, true);
}

static void handle_received_byte(chip_state_t *chip, uint8_t data) {
  if (chip->expect_wreg_count) {
    chip->wreg_count = (uint8_t)((data & 0x0Fu) + 1u);
    chip->expect_wreg_count = false;
    chip->expect_wreg_data = true;
    return;
  }

  if (chip->expect_wreg_data) {
    if (chip->wreg_addr < REG_COUNT) {
      chip->regs[chip->wreg_addr] = data;
    }
    if (chip->wreg_count > 0) chip->wreg_count--;
    chip->wreg_addr++;
    if (chip->wreg_count == 0) {
      chip->expect_wreg_data = false;
      drdy_after_host_access(chip);
    }
    return;
  }

  if (chip->expect_rreg_count) {
    chip->expect_rreg_count = false;
    uint8_t reg_val =
        (chip->rreg_addr < REG_COUNT) ? chip->regs[chip->rreg_addr] : 0;
    queue_tx_bytes(chip, &reg_val, 1);
    return;
  }

  if (data == 0xFE) {
    reset_device_state(chip);
    return;
  }

  if (data == 0x00 || data == 0xFF) {
    return;
  }

  if (data == 0x0F) {
    chip->rdatas_mode = false;
    return;
  }

  if (data == 0xFC) {
    load_conversion_result(chip);
    set_drdy_ready(chip, true);
    schedule_next_conversion(chip);
    return;
  }

  if (data >= 0xF0 && data <= 0xF4) {
    handle_cal_command(chip);
    return;
  }

  if ((data & 0xF0u) == 0x50u) {
    chip->wreg_addr = (uint8_t)(data & 0x0Fu);
    chip->expect_wreg_count = true;
    return;
  }

  if ((data & 0xF0u) == 0x10u) {
    chip->rreg_addr = (uint8_t)(data & 0x0Fu);
    chip->expect_rreg_count = true;
    return;
  }

  if (data == 0x01) {
    if (!chip->data_staged) {
      load_conversion_result(chip);
    }
    queue_tx_bytes(chip, chip->staged_data, 3);
    return;
  }

  if (data == 0x03) {
    chip->rdatas_mode = true;
    if (!chip->data_staged) {
      load_conversion_result(chip);
    }
    queue_tx_bytes(chip, chip->staged_data, 3);
    return;
  }
}

static void pin_change_handler(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *chip = (chip_state_t *)user_data;
  (void)value;

  if (pin == chip->pin_reset) {
    if (pin_read(chip->pin_reset) == LOW) {
      reset_device_state(chip);
    }
    return;
  }

  if (pin == chip->pin_cs) {
    if (pin_read(chip->pin_cs) == LOW) {
      reset_protocol_state(chip);
    } else {
      pin_mode(chip->pin_dout, INPUT);
    }
    return;
  }

  if (pin != chip->pin_sck || pin_read(chip->pin_cs) == HIGH) return;

  if (pin_read(chip->pin_sck) == HIGH) {
    if (chip->tx_len > 0) {
      shift_out_next_bit(chip);
    }
    return;
  }

  if (chip->rdatas_mode && chip->drdy_ready && chip->data_staged &&
      chip->tx_len == 0) {
    queue_tx_bytes(chip, chip->staged_data, 3);
  }

  chip->rx_byte =
      (uint8_t)((chip->rx_byte << 1) | (pin_read(chip->pin_din) ? 1u : 0u));
  chip->rx_bit_count++;
  if (chip->rx_bit_count >= 8) {
    handle_received_byte(chip, chip->rx_byte);
    chip->rx_byte = 0;
    chip->rx_bit_count = 0;
  }
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));
  if (!chip) {
    printf("ADS1256: malloc failed\n");
    return;
  }

  chip->pin_ain0 = pin_init("AIN0", ANALOG);
  chip->pin_ain1 = pin_init("AIN1", ANALOG);
  chip->pin_vrefp = pin_init("VREFP", ANALOG);
  chip->pin_vrefn = pin_init("VREFN", ANALOG);

  chip->pin_cs = pin_init("CS", INPUT_PULLUP);
  chip->pin_sck = pin_init("SCK", INPUT);
  chip->pin_din = pin_init("DIN", INPUT);
  chip->pin_dout = pin_init("DOUT", OUTPUT_LOW);
  chip->pin_drdy = pin_init("DRDY", OUTPUT_LOW);
  chip->pin_reset = pin_init("RESET", INPUT_PULLUP);

  const pin_watch_config_t watch = {
      .user_data = chip,
      .edge = BOTH,
      .pin_change = pin_change_handler,
  };
  pin_watch(chip->pin_reset, &watch);
  pin_watch(chip->pin_cs, &watch);
  pin_watch(chip->pin_sck, &watch);

  const timer_config_t drdy_cfg = {
      .user_data = chip,
      .callback = drdy_timer_callback,
  };
  chip->drdy_timer = timer_init(&drdy_cfg);

  reset_device_state(chip);
  printf("ADS1256 chip initialized\n");
}
