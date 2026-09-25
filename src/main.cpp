// Arduino Uno: uma cadeia geofone (geo -> instamp/INA128U -> ADS1256) em SPI.
// A cada ciclo do loop(), lê o ADC, grava a amostra em binário no cartão SD
// e envia a mesma amostra em texto pela Serial para o sistema host.
//
// Mapa de pinos (barramento SPI: MOSI D11, MISO D12, SCK D13):
//   ADS1256: CS D9, DRDY D8, RESET D7
//   SD:      CS D10
//
// Serial (texto): timestamp_us,counts
// SD (binário little-endian, 8 bytes): uint32 timestamp_us, int32 counts
// A tensão não entra no registro. O host multiplica counts por (2*2,5/8388608).

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "ADS1256.h"

#define ADS1256_SPI_ALREADY_STARTED

namespace {
// --- Configuração de hardware e arquivo ---

constexpr uint8_t kSdChipSelect = 10;   // CS do cartão microSD
constexpr uint8_t kAdcCsPin     =  9;   // CS do ADS1256
constexpr uint8_t kAdcDrdyPin   =  8;   // DRDY do ADS1256
constexpr uint8_t kAdcResetPin  =  7;   // RESET do ADS1256
constexpr float   kAdcVrefVolts = 2.5f; // Tensão de referência usada na conversão counts -> volts

constexpr char kFilename[] = "data.bin";

// Registro fixo de 8 bytes. Sem CSV no cartão: dtostrf/snprintf de volts
// alongava o loop e a taxa caía. packed evita padding entre os campos,
// então o layout no arquivo é exatamente esses 8 bytes, um após o outro.
struct SampleRecord {
  uint32_t timestamp_us; // micros() desta amostra
  int32_t counts;        // leitura crua do ADS1256, já com sinal
} __attribute__((packed));

ADS1256 adc(kAdcDrdyPin, kAdcResetPin, ADS1256::PIN_UNUSED, kAdcCsPin, kAdcVrefVolts);

File database_file;
char rowBuffer[32];
bool sd_available = false;
}  // namespace anônimo

// Deixa todos os pinos CS em HIGH (inativos) antes de usar SPI/SD.
static void initChipSelects(void) {
  pinMode(kSdChipSelect, OUTPUT);
  digitalWrite(kSdChipSelect, HIGH);

  pinMode(kAdcCsPin, OUTPUT);
  digitalWrite(kAdcCsPin, HIGH);
}

void setup() {
  Serial.begin(500000);
  delay(500);

  initChipSelects();
  SPI.begin();

  adc.InitializeADC();
  digitalWrite(kAdcCsPin, HIGH);
  adc.setDRATE(DRATE_1000SPS);
  digitalWrite(kAdcCsPin, HIGH);

  // SD no mesmo SPI: CS do ADC fica inativo durante SD.begin/open.
  if (!SD.begin(kSdChipSelect)) {
    Serial.println(F("SD: falha ao iniciar"));
  } else {
    // Apaga a captura anterior para o arquivo não misturar dois ensaios.
    if (SD.exists(kFilename)) {
      SD.remove(kFilename);
    }
    // Aberto uma vez e mantido aberto. Abrir/fechar a cada amostra é lento.
    database_file = SD.open(kFilename, FILE_WRITE);
    if (!database_file) {
      Serial.println(F("SD: falha ao abrir data.bin"));
    } else {
      sd_available = true;
      Serial.println(F("SD: gravando data.bin"));
    }
  }
  digitalWrite(kSdChipSelect, HIGH);

  Serial.println(F("timestamp_us,counts"));
}

void loop() {
  const unsigned long t_us = micros();
  const long counts = adc.readSingle();

  // Texto só na Serial, para o run_tests.py medir a taxa. Sem coluna de volts.
  snprintf(rowBuffer, sizeof(rowBuffer), "%lu,%ld", t_us, counts);
  Serial.println(rowBuffer);

  // Copia os 8 bytes do struct para o cache da biblioteca SD (setor de 512 B).
  // O cartão só é acessado quando esse cache enche; não há flush() aqui,
  // porque cada sync segura o SPI por vários milissegundos e perde amostras.
  if (sd_available && database_file) {
    const SampleRecord rec = {static_cast<uint32_t>(t_us), static_cast<int32_t>(counts)};
    database_file.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
  }
}