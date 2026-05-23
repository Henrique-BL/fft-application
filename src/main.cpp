// Arduino Uno: três cadeias geofone (geo -> instamp -> ADS1256) em SPI compartilhado.
// A cada ciclo do loop(), lê os três ADCs e grava uma linha no CSV do cartão SD.
//
// Mapa de pinos (por canal — barramento compartilhado: MOSI D11, MISO D12, SCK D13, RESET D7):
//   Ch0 (ADS 1): CS D9,  DRDY D8
//   Ch1 (ADS 2): CS D6,  DRDY D4
//   Ch2 (ADS 3): CS D5,  DRDY D2
//   SD:          CS D10
//
// Formato de cada linha em data.csv:
//   timestamp_us,counts0,volts0,counts1,volts1,counts2,volts2

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "ADS1256.h"

// Evita que cada ADS1256 chame SPI.begin() de novo; o barramento é iniciado uma vez no setup().
#define ADS1256_SPI_ALREADY_STARTED

namespace {
// --- Configuração de hardware e arquivo ---

constexpr uint8_t kSdChipSelect = 10;   // CS do cartão microSD
constexpr uint8_t kAdcResetPin  =  7;   // RESET compartilhado pelos três ADS1256
constexpr float   kAdcVrefVolts = 2.5f; // Tensão de referência usada na conversão counts -> volts

constexpr uint8_t kAdcCsPins[]  = {9, 6, 5}; // CS individual de cada ADS1256
constexpr uint8_t kAdcDrdyPins[] = {8, 4, 2}; // DRDY individual de cada ADS1256
constexpr uint8_t kChannelCount = 3;

constexpr char kFilename[] = "data.csv";
constexpr uint16_t kFlushEverySamples = 16; // Intervalo de mensagens de progresso na serial

// Instancia um objeto ADS1256 por cadeia de sinal (geofone -> instamp -> ADC).
ADS1256 adc0(kAdcDrdyPins[0], kAdcResetPin, ADS1256::PIN_UNUSED, kAdcCsPins[0],
             kAdcVrefVolts); // ADS 1 — canal 0
ADS1256 adc1(kAdcDrdyPins[1], kAdcResetPin, ADS1256::PIN_UNUSED, kAdcCsPins[1],
             kAdcVrefVolts); // ADS 2 — canal 1
ADS1256 adc2(kAdcDrdyPins[2], kAdcResetPin, ADS1256::PIN_UNUSED, kAdcCsPins[2],
             kAdcVrefVolts); // ADS 3 — canal 2

ADS1256 *const kAdcs[] = {&adc0, &adc1, &adc2};

File database_file;           // Handle temporário do arquivo CSV no SD
char rowBuffer[128];          // Linha CSV montada antes de gravar
uint16_t samplesSinceFlush = 0;
}  // namespace anônimo

// Para a execução e imprime o erro na serial quando algo crítico falha.
static void halt(const __FlashStringHelper *msg) {
  Serial.println(msg);
  while (true) {
    delay(1000);
  }
}

// Deixa todos os pinos CS em HIGH (inativos) antes de usar SPI/SD.
static void initChipSelects(void) {
  pinMode(kSdChipSelect, OUTPUT);
  digitalWrite(kSdChipSelect, HIGH);

  for (uint8_t i = 0; i < kChannelCount; i++) {
    pinMode(kAdcCsPins[i], OUTPUT);
    digitalWrite(kAdcCsPins[i], HIGH);
  }
}

void setup() {
  // --- Passo 1: inicia comunicação serial para depuração ---
  Serial.begin(115200);
  delay(500);
  Serial.println(F("3x Geophone + ADS1256 + SD logger"));

  // --- Passo 2: prepara pinos CS e barramento SPI compartilhado ---
  initChipSelects();
  SPI.begin();

  // --- Passo 3: inicializa cada ADS1256 (reset, registradores, calibração) ---
  Serial.println(F("Init ADS1256 channels..."));
  for (uint8_t i = 0; i < kChannelCount; i++) {
    kAdcs[i]->InitializeADC();
    digitalWrite(kAdcCsPins[i], HIGH); // Garante CS alto após init de cada chip
    Serial.print(F("  CH"));
    Serial.println(i);
  }
  Serial.println(F("ADS1256 ready"));

  // --- Passo 4: inicializa cartão SD (SPI compartilhado com os ADCs) ---
  Serial.println(F("Init SD card..."));
  for (uint8_t i = 0; i < kChannelCount; i++) {
    digitalWrite(kAdcCsPins[i], HIGH); // Desseleciona ADCs antes de falar com o SD
  }
  if (!SD.begin(kSdChipSelect)) {
    halt(F("SD init failed (CS=D10, MOSI=D11, MISO=D12, SCK=D13)"));
  }
  Serial.println(F("SD ready"));

  // --- Passo 5: cria data.csv com cabeçalho, ou reutiliza arquivo existente ---
  if (!SD.exists(kFilename)) {
    database_file = SD.open(kFilename, FILE_WRITE);
    if (!database_file) {
      halt(F("Cannot create data.csv"));
    }
    database_file.println(
        F("timestamp_us,counts0,volts0,counts1,volts1,counts2,volts2"));
    database_file.close();
    Serial.println(F("CSV header written"));
  } else {
    Serial.println(F("Appending to existing data.csv"));
  }
}

void loop() {
  // --- Passo 1: marca o instante desta amostragem (microssegundos desde boot) ---
  const unsigned long t_us = micros();

  long counts[kChannelCount];
  float volts[kChannelCount];

  // --- Passo 2: lê os três ADS1256 em sequência pelo barramento SPI ---
  // Cada iteração seleciona um chip via CS, espera DRDY e lê uma conversão.
  for (uint8_t ch = 0; ch < kChannelCount; ch++) {
    // Aqui lê-se o valor bruto (counts) do ADS1256 do canal `ch` (0=ADS1, 1=ADS2, 2=ADS3).
    counts[ch] = kAdcs[ch]->readSingle();
    // Converte counts para volts usando VREF e ganho PGA configurados na biblioteca.
    volts[ch] = kAdcs[ch]->convertToVoltage(counts[ch]);
  }

  // --- Passo 3: formata tensões como texto decimal para montar a linha CSV ---
  char v0[12];
  char v1[12];
  char v2[12];
  dtostrf(volts[0], 0, 5, v0); // Volts do ADS 1
  dtostrf(volts[1], 0, 5, v1); // Volts do ADS 2
  dtostrf(volts[2], 0, 5, v2); // Volts do ADS 3

  // Monta uma linha: timestamp, counts/volts de cada um dos três canais.
  snprintf(rowBuffer, sizeof(rowBuffer), "%lu,%ld,%s,%ld,%s,%ld,%s", t_us,
           counts[0], v0, counts[1], v1, counts[2], v2);

  // --- Passo 4: abre data.csv, acrescenta a linha e fecha o arquivo ---
  database_file = SD.open(kFilename, FILE_WRITE);
  if (database_file) {
    database_file.println(rowBuffer); // Grava a linha no CSV
    database_file.close();

    // Ecoa a mesma linha na serial para acompanhar em tempo real.
    Serial.println(rowBuffer);

    // A cada kFlushEverySamples linhas, imprime contador de progresso na serial.
    samplesSinceFlush++;
    if (samplesSinceFlush >= kFlushEverySamples) {
      Serial.print(F("Logged "));
      Serial.print(samplesSinceFlush);
      Serial.println(F(" triple-samples"));
      samplesSinceFlush = 0;
    }
  } else {
    Serial.println(F("SD write failed"));
  }
}
