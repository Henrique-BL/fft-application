// Wokwi custom chip that emulates the ADS1256 24-bit delta-sigma ADC.
//
// Role in the signal chain:
//   Instrumentation Amplifier (instamp.chip.c) --> ADS1256 (this chip) --> Arduino via SPI
//
// AIN0 receives the instrumentation amplifier's OUT pin (amplified + biased signal).
// AIN1 receives the instrumentation amplifier's VREF pin (the static 2.5 V bias).
// The differential read (AIN0 - AIN1) cancels the bias in hardware, so the
// 24-bit result represents only the amplified geophone signal, bipolar around zero.
//
// The Arduino firmware (src/main.cpp) drives this chip over SPI:
//   CS  (pin 9)  -- selects/deselects this chip on the shared SPI bus
//   SCK (pin 13) -- SPI clock produced by the Arduino SPI peripheral
//   DIN (pin 11) -- commands and register writes from the Arduino (MOSI)
//   DOUT(pin 12) -- 24-bit conversion result shifted back to Arduino (MISO)
//   DRDY(pin  8) -- pulled LOW by this chip when a new sample is ready
//   RESET(pin 7) -- hard reset line from the Arduino; active-LOW

#include "wokwi-api.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  // Analog inputs from the instrumentation amplifier.
  pin_t pin_ain0;    // InstAmp OUT: gain*(Vdiff) + vref
  pin_t pin_ain1;    // InstAmp VREF: the bias reference to be subtracted

  // Reference voltage pins; vref = VREFP - VREFN normalises the ADC full scale.
  pin_t pin_vrefp;
  pin_t pin_vrefn;

  // SPI digital interface to the Arduino firmware.
  pin_t pin_cs;      // Active-LOW chip select; gates all SPI traffic
  pin_t pin_sck;     // SPI clock; DIN sampled on falling, DOUT shifted on rising
  pin_t pin_din;     // Incoming command/data byte stream from the Arduino
  pin_t pin_dout;    // Outgoing 24-bit conversion result to the Arduino
  pin_t pin_drdy;    // Signals the Arduino that a conversion result is available
  pin_t pin_reset;   // Hard reset; restores default register values when asserted

  // Mirror of the ADCON register; lower 3 bits set the PGA gain.
  uint8_t adcon;

  // SPI receive shift register: accumulates bits from DIN into complete bytes.
  uint8_t rx_byte;
  uint8_t rx_bit_count;

  // 24-bit transmit buffer holding the last conversion result, sent MSB-first
  // over DOUT in response to a RDATA (0x01) command from the Arduino.
  uint8_t tx_bytes[3];
  uint8_t tx_byte_index;
  uint8_t tx_bit_index;

  // WREG command parser state: the ADS1256 WREG opcode (0x5x) is followed by
  // a count byte then one or more data bytes; these flags track that sequence.
  bool    expect_wreg_count;
  bool    expect_wreg_data;
  uint8_t wreg_addr;
  uint8_t wreg_count;
} chip_state_t;

static chip_state_t chip;

// Clamps a value to the 24-bit signed integer range [-8388608, 8388607].
static int32_t clamp24(int32_t value) {
  if (value >  8388607) return  8388607;
  if (value < -8388608) return -8388608;
  return value;
}

// Decodes the PGA gain bits (lower 3 bits of ADCON) written by the Arduino
// via WREG so the simulated conversion result reflects the configured amplification.
static float gain_from_adcon(uint8_t adcon) {
  switch (adcon & 0x07) {
    case 0x00: return  1.0f;
    case 0x01: return  2.0f;
    case 0x02: return  4.0f;
    case 0x03: return  8.0f;
    case 0x04: return 16.0f;
    case 0x05: return 32.0f;
    case 0x06: return 64.0f;
    default:   return  1.0f;
  }
}

// Samples AIN0 and AIN1 from the instrumentation amplifier, applies the PGA gain
// from ADCON, normalises against the reference voltage, and packs the signed
// 24-bit result into tx_bytes[] ready to be clocked out on DOUT.
static void load_conversion_result(void) {
  float vp    = pin_adc_read(chip.pin_ain0);   // InstAmp amplified + biased output
  float vn    = pin_adc_read(chip.pin_ain1);   // InstAmp VREF (bias reference)
  float vrefp = pin_adc_read(chip.pin_vrefp);
  float vrefn = pin_adc_read(chip.pin_vrefn);
  float gain  = gain_from_adcon(chip.adcon);
  float vref  = vrefp - vrefn;
  if (vref < 0.01f) vref = 2.5f;  // Falls back to nominal if reference pins are floating.

  // (vp - vn) subtracts the instamp VREF bias, recovering the true bipolar signal.
  // Multiplying by gain applies the PGA factor configured by the Arduino firmware.
  // Dividing by vref normalises to the [-1, 1] range that maps to ±full-scale counts.
  float normalized = ((vp - vn) * gain) / vref;
  if (normalized >  1.0f) normalized =  1.0f;
  if (normalized < -1.0f) normalized = -1.0f;

  // Scales to the 24-bit signed integer range; this is the value the Arduino
  // reads via RDATA and converts back to volts using countsToVolts().
  int32_t counts  = clamp24((int32_t)(normalized * 8388607.0f));
  uint32_t raw24  = (uint32_t)counts & 0x00FFFFFFu;

  chip.tx_bytes[0]   = (raw24 >> 16) & 0xFFu;  // MSB sent first over SPI DOUT
  chip.tx_bytes[1]   = (raw24 >>  8) & 0xFFu;
  chip.tx_bytes[2]   =  raw24        & 0xFFu;
  chip.tx_byte_index = 0;
  chip.tx_bit_index  = 0;
}

// Shifts the next bit of the 24-bit result onto DOUT so the Arduino SPI
// peripheral can clock it in as the MISO byte stream.
static void shift_out_next_bit(void) {
  uint8_t out_bit = 0;
  if (chip.tx_byte_index < 3) {
    uint8_t current = chip.tx_bytes[chip.tx_byte_index];
    out_bit = (current >> (7 - chip.tx_bit_index)) & 0x01u;
    chip.tx_bit_index++;
    if (chip.tx_bit_index >= 8) {
      chip.tx_bit_index = 0;
      chip.tx_byte_index++;
    }
  }
  pin_write(chip.pin_dout, out_bit ? HIGH : LOW);
}

// Resets the SPI byte-level protocol state without changing register values;
// called whenever CS is de-asserted so a new transaction starts cleanly.
static void reset_protocol_state(void) {
  chip.rx_byte          = 0;
  chip.rx_bit_count     = 0;
  chip.expect_wreg_count = false;
  chip.expect_wreg_data  = false;
  chip.wreg_addr         = 0;
  chip.wreg_count        = 0;
  chip.tx_bytes[0]       = 0;
  chip.tx_bytes[1]       = 0;
  chip.tx_bytes[2]       = 0;
  chip.tx_byte_index     = 3;   // Marks tx buffer as empty; no bits will be shifted out.
  chip.tx_bit_index      = 0;
  pin_mode(chip.pin_dout, INPUT);
}

// Full hardware reset: restores ADCON to the power-on default (PGA=1, gain=0x20)
// and clears the protocol state; mirrors what the Arduino firmware does by
// sending the RESET (0xFE) command or pulling the RESET pin LOW.
static void reset_device_state(void) {
  chip.adcon = 0x20;
  reset_protocol_state();
}

// Processes a fully received command or data byte from the Arduino over DIN.
// Implements the subset of the ADS1256 SPI protocol used by the firmware:
//   0xFE        -- RESET: equivalent to a hardware reset pulse
//   0x5x        -- WREG: write registers starting at address x
//   0x01        -- RDATA: trigger a differential conversion and stage the result
static void handle_received_byte(uint8_t data) {
  if (chip.expect_wreg_count) {
    // Second byte of WREG sequence: encodes (number_of_registers - 1).
    chip.wreg_count        = (data & 0x1Fu) + 1u;
    chip.expect_wreg_count = false;
    chip.expect_wreg_data  = true;
    return;
  }

  if (chip.expect_wreg_data) {
    // Subsequent bytes are register data; only ADCON (addr 0x02) is acted on,
    // as it holds the PGA gain bits that affect the conversion result.
    if (chip.wreg_addr == 0x02) {
      chip.adcon = data;
    }
    if (chip.wreg_count > 0) chip.wreg_count--;
    if (chip.wreg_count == 0) chip.expect_wreg_data = false;
    chip.wreg_addr++;
    return;
  }

  if (data == 0xFE) {
    // RESET command from the Arduino firmware (sent during initAds1256()).
    reset_device_state();
    return;
  }

  if ((data & 0xF0u) == 0x50u) {
    // WREG opcode: lower nibble is the starting register address.
    chip.wreg_addr         = data & 0x0Fu;
    chip.expect_wreg_count = true;
    return;
  }

  if (data == 0x01) {
    // RDATA command: the Arduino is requesting a new sample; captures the current
    // analog voltages from the instrumentation amplifier and stages the 24-bit result.
    load_conversion_result();
    return;
  }
}

// Handles all digital pin changes on the SPI interface and RESET line.
// The three monitored pins (RESET, CS, SCK) drive the complete SPI state machine.
static void pin_change_handler(void *user_data, pin_t pin, uint32_t value) {
  (void)user_data;
  (void)value;

  if (pin == chip.pin_reset) {
    // RESET pulled LOW by the Arduino firmware during initAds1256() to clear
    // any leftover state before writing configuration registers.
    if (pin_read(chip.pin_reset) == LOW) {
      reset_device_state();
    }
    return;
  }

  if (pin == chip.pin_cs) {
    if (pin_read(chip.pin_cs) == LOW) {
      // CS asserted: Arduino is starting a new SPI transaction; arm DOUT as output
      // and shift the first bit so it is ready before the first SCK rising edge.
      reset_protocol_state();
      pin_mode(chip.pin_dout, OUTPUT);
      shift_out_next_bit();
    } else {
      // CS de-asserted: transaction ended; release DOUT so the SD card (also on
      // the SPI bus, CS=D10) can drive MISO without contention.
      pin_mode(chip.pin_dout, INPUT);
    }
    return;
  }

  // All remaining events are SCK edges; ignore them when CS is HIGH (chip not selected).
  if (pin != chip.pin_sck || pin_read(chip.pin_cs) == HIGH) return;

  if (pin_read(chip.pin_sck) == HIGH) {
    // SCK rising edge: shift the next result bit onto DOUT (SPI MODE1 outputs on rising).
    shift_out_next_bit();
    return;
  }

  // SCK falling edge: sample DIN and accumulate into the receive shift register.
  chip.rx_byte = (chip.rx_byte << 1) | (pin_read(chip.pin_din) ? 1u : 0u);
  chip.rx_bit_count++;
  if (chip.rx_bit_count >= 8) {
    // Full byte received from the Arduino; interpret it as a command or data byte.
    handle_received_byte(chip.rx_byte);
    chip.rx_byte      = 0;
    chip.rx_bit_count = 0;
  }
}

void chip_init(void) {
  // Analog inputs driven by the instrumentation amplifier outputs.
  chip.pin_ain0  = pin_init("AIN0",  ANALOG);
  chip.pin_ain1  = pin_init("AIN1",  ANALOG);
  chip.pin_vrefp = pin_init("VREFP", ANALOG);
  chip.pin_vrefn = pin_init("VREFN", ANALOG);

  // SPI digital interface connected to the Arduino (firmware in src/main.cpp).
  chip.pin_cs    = pin_init("CS",    INPUT_PULLUP);
  chip.pin_sck   = pin_init("SCK",   INPUT);
  chip.pin_din   = pin_init("DIN",   INPUT);
  chip.pin_dout  = pin_init("DOUT",  OUTPUT_LOW);
  chip.pin_drdy  = pin_init("DRDY",  OUTPUT_LOW);
  chip.pin_reset = pin_init("RESET", INPUT_PULLUP);

  // Watches RESET, CS, and SCK for any edge; the handler implements the full
  // SPI protocol state machine that services commands from the Arduino firmware.
  const pin_watch_config_t watch = {
      .user_data  = 0,
      .edge       = BOTH,
      .pin_change = pin_change_handler,
  };
  pin_watch(chip.pin_reset, &watch);
  pin_watch(chip.pin_cs,    &watch);
  pin_watch(chip.pin_sck,   &watch);

  reset_device_state();
  // Pulls DRDY LOW immediately so the Arduino firmware's waitDrdyLow() call
  // in loop() succeeds on the first iteration without a timeout.
  pin_write(chip.pin_drdy, LOW);
}
