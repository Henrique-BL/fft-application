// Arduino Uno: uma cadeia geofone (geo -> instamp/INA128U -> ADS1256) em SPI.
// A cada ciclo do loop(), lê o ADC e grava uma linha no CSV do cartão SD.
//
// Mapa de pinos (barramento SPI: MOSI D11, MISO D12, SCK D13):
//   ADS1256: CS D9, DRDY D8, RESET D7
//   SD:      CS D10
//
// Formato de cada linha em data.csv:
//   timestamp_us,counts,volts

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "ADS1256.h"

// Evita que o ADS1256 chame SPI.begin() de novo; o barramento é iniciado uma vez no setup().
#define ADS1256_SPI_ALREADY_STARTED

namespace {
// --- Configuração de hardware e arquivo ---

constexpr uint8_t kSdChipSelect = 10;   // CS do cartão microSD
constexpr uint8_t kAdcCsPin     =  9;   // CS do ADS1256
constexpr uint8_t kAdcDrdyPin   =  8;   // DRDY do ADS1256
constexpr uint8_t kAdcResetPin  =  7;   // RESET do ADS1256
constexpr float   kAdcVrefVolts = 2.5f; // Tensão de referência usada na conversão counts -> volts

constexpr char kFilename[] = "data.csv";
constexpr uint16_t kFlushEverySamples = 16; // Intervalo de mensagens de progresso na serial

ADS1256 adc(kAdcDrdyPin, kAdcResetPin, ADS1256::PIN_UNUSED, kAdcCsPin, kAdcVrefVolts);

File database_file;           // Handle temporário do arquivo CSV no SD
char rowBuffer[64];           // Linha CSV montada antes de gravar
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

  pinMode(kAdcCsPin, OUTPUT);
  digitalWrite(kAdcCsPin, HIGH);
}

void setup() {
  // --- Passo 1: inicia comunicação serial para depuração ---
  Serial.begin(115200);
  delay(500);
  Serial.println(F("Geophone + INA128U + ADS1256 + SD logger"));

  // --- Passo 2: prepara pinos CS e barramento SPI ---
  initChipSelects();
  SPI.begin();

  // --- Passo 3: inicializa o ADS1256 (reset, registradores, calibração) ---
  Serial.println(F("Init ADS1256..."));
  adc.InitializeADC();
  digitalWrite(kAdcCsPin, HIGH);
  Serial.println(F("ADS1256 ready"));

  // --- Passo 4: inicializa cartão SD (SPI compartilhado com o ADC) ---
  Serial.println(F("Init SD card..."));
  digitalWrite(kAdcCsPin, HIGH);
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
    database_file.println(F("timestamp_us,counts,volts"));
    database_file.close();
    Serial.println(F("CSV header written"));
  } else {
    Serial.println(F("Appending to existing data.csv"));
  }
}

void loop() {
  // --- Passo 1: marca o instante desta amostragem (microssegundos desde boot) ---
  const unsigned long t_us = micros();

  // --- Passo 2: lê o ADS1256 (espera DRDY e lê uma conversão) ---
  const long counts = adc.readSingle();
  const float volts = adc.convertToVoltage(counts);

  // --- Passo 3: formata tensão e monta a linha CSV ---
  char vStr[12];
  dtostrf(volts, 0, 5, vStr);
  snprintf(rowBuffer, sizeof(rowBuffer), "%lu,%ld,%s", t_us, counts, vStr);

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
      Serial.println(F(" samples"));
      samplesSinceFlush = 0;
    }
  } else {
    Serial.println(F("SD write failed"));
  }
}
