// Chip customizado Wokwi que emula um geofone SM-6.
//
// Papel na cadeia de sinal:
//   Geofone (este chip) --> Amplificador de instrumentação (instamp.chip.c)
//
// Produz duas saídas analógicas em antifase (OUT_P, OUT_N) conectadas diretamente
// às entradas IN_P e IN_N do amplificador de instrumentação. Ambas as pernas são
// polarizadas em V_BIAS = 2,5 V para manter o sinal AC bipolar simulado dentro
// da faixa analógica 0–5 V do Wokwi. A tensão diferencial vista pelo instamp é:
//   (OUT_P - OUT_N) = 2 * amp_v * sin(fase)
// o que dobra a excursão efetiva e cancela completamente o bias e o ruído
// em modo comum compartilhados entre as duas pernas antes da amplificação.
//
// Atributos (definidos em diagram.json ou nos controles do chip):
//   frequencyHz   frequência senoidal em Hz (padrão 15)
//   amplitudeMv   excursão single-ended em cada perna, em milivolts (padrão 50)
//   noiseMv       amplitude de ruído branco adicionada igualmente às duas pernas (padrão 1)
//   eventBurst    valor não nulo multiplica a amplitude por 10 para simular um evento sísmico

#include "wokwi-api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Timer dispara a 4 kHz, igual à taxa de atualização da simulação do instamp e ADS1256,
// para que cada chip da cadeia processe cada amostra no mesmo instante.
#define UPDATE_RATE_HZ 4000
#define UPDATE_PERIOD_US (1000000 / UPDATE_RATE_HZ)

// Polarização em meia escala aplicada às duas pernas; deve coincidir com o atributo VREF
// do instamp para cancelar exatamente o componente em modo comum na entrada do instamp.
#define V_BIAS 2.5f

typedef struct {
  pin_t pin_out_p;        // Conecta ao IN_P do instamp; transporta V_BIAS + sinal.
  pin_t pin_out_n;        // Conecta ao IN_N do instamp; transporta V_BIAS - sinal.
  uint32_t attr_freq;
  uint32_t attr_amp_mv;
  uint32_t attr_noise_mv;
  uint32_t attr_event_burst;
  double phase;
} chip_state_t;

static void chip_timer_event(void *user_data);

static float frand_signed(void) {
  // Gera um float pseudoaleatório em [-1,0, 1,0] para injeção de ruído branco.
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

  // Rajada de evento amplia a excursão em ~10x para verificar resposta transitória/FFT
  // vista pelo instamp e, após amplificação, pelo ADS1256.
  float burst_gain = burst ? 10.0f : 1.0f;
  float amp_v   = (amp_mv * burst_gain) / 1000.0f;
  // Ruído é igual nas duas pernas (modo comum); o instamp rejeita a maior parte
  // ao calcular o diferencial (IN_P - IN_N).
  float noise_v = (noise_mv / 1000.0f) * frand_signed();

  chip->phase += 2.0 * M_PI * (double)freq / (double)UPDATE_RATE_HZ;
  if (chip->phase >= 2.0 * M_PI) {
    chip->phase -= 2.0 * M_PI;
  }

  float s = (float)sin(chip->phase);
  // Pernas em antifase simulam a natureza diferencial de um geofone de bobina móvel;
  // OUT_P - OUT_N = 2*amp_v*s, que é o que o instamp amplifica.
  float out_p = V_BIAS + amp_v * s + noise_v;
  float out_n = V_BIAS - amp_v * s + noise_v;

  pin_dac_write(chip->pin_out_p, out_p);
  pin_dac_write(chip->pin_out_n, out_n);
}
