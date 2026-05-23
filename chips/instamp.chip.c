// Chip customizado Wokwi que modela um amplificador de instrumentação (estilo INA128U)
// alimentado por uma única fonte de 5 V.
//
// Papel na cadeia de sinal:
//   Geofone (geophone.chip.c) --> Amplificador de instrumentação (este chip) --> ADS1256 (ads1256.chip.c)
//
// Recebe o par diferencial em antifase do geofone em IN_P e IN_N.
// O amplificador extrai apenas o componente diferencial:
//   Vdiff = IN_P - IN_N = 2 * amp_v * sin(fase)
// amplifica por `gain` e adiciona `vref` (padrão 2,5 V) como offset DC para manter
// a saída single-ended dentro da faixa 0–5 V aceita pelo ADS1256:
//   Vout = clamp(gain * Vdiff + vref, 0, 5)
//
// O pino VREF reemite a mesma tensão vref no AIN1 do ADS1256 para que o ADC
// faça uma leitura diferencial (AIN0 - AIN1) que cancela o offset de 2,5 V
// em hardware, recuperando o sinal bipolar real do geofone em contagens digitais.

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

// Iguala as taxas de atualização do geofone e do ADS1256 para avanço sincronizado dos chips.
#define UPDATE_RATE_HZ 4000
#define UPDATE_PERIOD_US (1000000 / UPDATE_RATE_HZ)

typedef struct {
  pin_t pin_in_p;   // Acionado por OUT_P do geofone: V_BIAS + sinal.
  pin_t pin_in_n;   // Acionado por OUT_N do geofone: V_BIAS - sinal.
  pin_t pin_out;    // Aciona AIN0 do ADS1256: gain * Vdiff + vref.
  pin_t pin_vref;   // Aciona AIN1 do ADS1256: polarização estática vref a ser cancelada.
  uint32_t attr_gain;
  uint32_t attr_vref;
} chip_state_t;

static void chip_timer_event(void *user_data);

static float clampf(float v, float lo, float hi) {
  // Modela a saturação dos trilhos do op-amp; mantém Vout dentro dos trilhos 0–5 V
  // para que o ADS1256 nunca receba tensão fora da faixa absoluta máxima de entrada.
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

  // Lê as duas pernas produzidas pelo geofone; modo comum (ruído + V_BIAS)
  // é eliminado pela subtração, restando apenas o sinal diferencial puro.
  float vp = pin_adc_read(chip->pin_in_p);
  float vn = pin_adc_read(chip->pin_in_n);

  // Saída diferencial amplificada deslocada para meia escala por vref, cabendo na
  // janela de entrada 0–5 V do ADS1256 em AIN0.
  float vout = clampf(gain * (vp - vn) + vref, 0.0f, 5.0f);

  pin_dac_write(chip->pin_out,  vout);
  // Reemite vref inalterado em AIN1 do ADS1256 para que a leitura diferencial
  // (AIN0 - AIN1) subtraia exatamente o mesmo offset adicionado acima,
  // retornando um valor bipolar centrado em zero no domínio digital.
  pin_dac_write(chip->pin_vref, vref);
}
