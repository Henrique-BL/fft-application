// Wokwi custom chip that models an instrumentation amplifier (INA128U-style)
// running from a single 5 V rail.
//
// Role in the signal chain:
//   Geophone (geophone.chip.c) --> Instrumentation Amplifier (this chip) --> ADS1256 (ads1256.chip.c)
//
// Receives the geophone's anti-phase differential pair on IN_P and IN_N.
// The amplifier extracts only the differential component:
//   Vdiff = IN_P - IN_N = 2 * amp_v * sin(phase)
// amplifies it by `gain`, then adds `vref` (default 2.5 V) as a DC offset so
// the single-ended output stays within the 0-5 V range accepted by the ADS1256:
//   Vout = clamp(gain * Vdiff + vref, 0, 5)
//
// The VREF pin re-emits the same vref voltage onto ADS1256 AIN1 so the ADC
// can perform a differential read (AIN0 - AIN1) that cancels the 2.5 V offset
// in hardware, recovering the true bipolar geophone signal in digital counts.

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

// Matches geophone and ADS1256 update rates so every chip advances in lockstep.
#define UPDATE_RATE_HZ 4000
#define UPDATE_PERIOD_US (1000000 / UPDATE_RATE_HZ)

typedef struct {
  pin_t pin_in_p;   // Driven by geophone OUT_P: V_BIAS + signal.
  pin_t pin_in_n;   // Driven by geophone OUT_N: V_BIAS - signal.
  pin_t pin_out;    // Drives ADS1256 AIN0: gain * Vdiff + vref.
  pin_t pin_vref;   // Drives ADS1256 AIN1: the static vref bias to be cancelled.
  uint32_t attr_gain;
  uint32_t attr_vref;
} chip_state_t;

static void chip_timer_event(void *user_data);

static float clampf(float v, float lo, float hi) {
  // Models op-amp rail saturation; keeps Vout inside the 0-5 V supply rails
  // so the ADS1256 never receives a voltage outside its absolute-maximum input range.
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));

  chip->pin_in_p  = pin_init("IN_P", ANALOG);
  chip->pin_in_n  = pin_init("IN_N", ANALOG);
  chip->pin_out   = pin_init("OUT",  ANALOG);
  chip->pin_vref  = pin_init("VREF", ANALOG);

  chip->attr_gain = attr_init_float("gain", 10.0f);
  chip->attr_vref = attr_init_float("vref", 2.5f);

  const timer_config_t timer_config = {
      .callback  = chip_timer_event,
      .user_data = chip,
  };
  timer_t timer = timer_init(&timer_config);
  timer_start(timer, UPDATE_PERIOD_US, true);

  printf("Instrumentation amp chip initialized\n");
}

static void chip_timer_event(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;

  float gain = attr_read_float(chip->attr_gain);
  float vref = attr_read_float(chip->attr_vref);

  // Reads the two legs produced by the geophone; common-mode (noise + V_BIAS)
  // is eliminated by the subtraction, leaving only the pure differential signal.
  float vp = pin_adc_read(chip->pin_in_p);
  float vn = pin_adc_read(chip->pin_in_n);

  // Amplified differential output shifted to mid-rail by vref so it fits the
  // ADS1256's 0-5 V input window on AIN0.
  float vout = clampf(gain * (vp - vn) + vref, 0.0f, 5.0f);

  pin_dac_write(chip->pin_out,  vout);
  // Re-emits vref unchanged onto AIN1 of the ADS1256 so the ADC differential
  // read (AIN0 - AIN1) subtracts exactly the same offset that was added above,
  // returning a bipolar value centered on zero in the digital domain.
  pin_dac_write(chip->pin_vref, vref);
}
