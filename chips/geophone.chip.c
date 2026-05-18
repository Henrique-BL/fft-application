// Wokwi custom chip that emulates an SM-6 geophone.
//
// Role in the signal chain:
//   Geophone (this chip) --> Instrumentation Amplifier (instamp.chip.c)
//
// Produces two anti-phase analog outputs (OUT_P, OUT_N) that connect directly
// to the instrumentation amplifier's IN_P and IN_N inputs.  Both legs are
// biased at V_BIAS = 2.5 V so the simulated bipolar AC signal remains within
// Wokwi's 0-5 V analog range.  The differential voltage seen by the instamp is:
//   (OUT_P - OUT_N) = 2 * amp_v * sin(phase)
// which doubles the effective swing while completely cancelling common-mode bias
// and noise shared between the two legs before amplification.
//
// Attributes (set from diagram.json or chip controls):
//   frequencyHz   sine frequency in Hz (default 15)
//   amplitudeMv   single-ended swing on each leg, in millivolts (default 50)
//   noiseMv       white noise amplitude added to both legs equally (default 1)
//   eventBurst    non-zero multiplies amplitude by 10 to mimic a seismic event

#include "wokwi-api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Timer fires at 4 kHz, matching the instamp and ADS1256 simulation update rate
// so every chip in the chain processes each sample at the same moment.
#define UPDATE_RATE_HZ 4000
#define UPDATE_PERIOD_US (1000000 / UPDATE_RATE_HZ)

// Mid-rail bias applied to both legs; must match the instamp VREF attribute so
// the common-mode component at the instamp input is exactly cancelled.
#define V_BIAS 2.5f

typedef struct {
  pin_t pin_out_p;        // Connects to instamp IN_P; carries V_BIAS + signal.
  pin_t pin_out_n;        // Connects to instamp IN_N; carries V_BIAS - signal.
  uint32_t attr_freq;
  uint32_t attr_amp_mv;
  uint32_t attr_noise_mv;
  uint32_t attr_event_burst;
  double phase;
} chip_state_t;

static void chip_timer_event(void *user_data);

static float frand_signed(void) {
  // Generates a pseudo-random float in [-1.0, 1.0] for white-noise injection.
  return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));

  chip->pin_out_p = pin_init("OUT_P", ANALOG);
  chip->pin_out_n = pin_init("OUT_N", ANALOG);

  chip->attr_freq        = attr_init_float("frequencyHz", 15.0f);
  chip->attr_amp_mv      = attr_init_float("amplitudeMv", 50.0f);
  chip->attr_noise_mv    = attr_init_float("noiseMv", 1.0f);
  chip->attr_event_burst = attr_init("eventBurst", 0);

  chip->phase = 0.0;

  const timer_config_t timer_config = {
      .callback  = chip_timer_event,
      .user_data = chip,
  };
  timer_t timer = timer_init(&timer_config);
  timer_start(timer, UPDATE_PERIOD_US, true);

  printf("Geophone chip initialized\n");
}

static void chip_timer_event(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;

  float freq     = attr_read_float(chip->attr_freq);
  float amp_mv   = attr_read_float(chip->attr_amp_mv);
  float noise_mv = attr_read_float(chip->attr_noise_mv);
  uint32_t burst = attr_read(chip->attr_event_burst);

  // Event burst pushes the swing ~10x to verify transient/FFT response
  // as seen by the instamp and, after amplification, by the ADS1256.
  float burst_gain = burst ? 10.0f : 1.0f;
  float amp_v   = (amp_mv * burst_gain) / 1000.0f;
  // Noise is the same on both legs (common-mode), so the instamp will reject
  // most of it when computing the differential (IN_P - IN_N).
  float noise_v = (noise_mv / 1000.0f) * frand_signed();

  chip->phase += 2.0 * M_PI * (double)freq / (double)UPDATE_RATE_HZ;
  if (chip->phase >= 2.0 * M_PI) {
    chip->phase -= 2.0 * M_PI;
  }

  float s = (float)sin(chip->phase);
  // Anti-phase legs mimic the differential nature of a moving-coil geophone;
  // OUT_P - OUT_N = 2*amp_v*s, which is what the instamp amplifies.
  float out_p = V_BIAS + amp_v * s + noise_v;
  float out_n = V_BIAS - amp_v * s + noise_v;

  pin_dac_write(chip->pin_out_p, out_p);
  pin_dac_write(chip->pin_out_n, out_n);
}
