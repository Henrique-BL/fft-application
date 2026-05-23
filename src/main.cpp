// Arduino Uno: three geophone chains (geo -> instamp -> ADS1256) on shared SPI.
// Logs one CSV row per sample instant with all three channels.
//
// Pin map (per channel — shared bus: MOSI D11, MISO D12, SCK D13, RESET D7):
//   Ch0: CS D9,  DRDY D8
//   Ch1: CS D6,  DRDY D4
//   Ch2: CS D5,  DRDY D2
//   SD:  CS D10

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "ADS1256.h"

#define ADS1256_SPI_ALREADY_STARTED

namespace {
constexpr uint8_t kSdChipSelect = 10;
constexpr uint8_t kAdcResetPin  =  7;
constexpr float   kAdcVrefVolts = 2.5f;

constexpr uint8_t kAdcCsPins[]  = {9, 6, 5};
constexpr uint8_t kAdcDrdyPins[] = {8, 4, 2};
constexpr uint8_t kChannelCount = 3;

constexpr char kFilename[] = "data.csv";
constexpr uint16_t kFlushEverySamples = 16;

ADS1256 adc0(kAdcDrdyPins[0], kAdcResetPin, ADS1256::PIN_UNUSED, kAdcCsPins[0],
             kAdcVrefVolts);
ADS1256 adc1(kAdcDrdyPins[1], kAdcResetPin, ADS1256::PIN_UNUSED, kAdcCsPins[1],
             kAdcVrefVolts);
ADS1256 adc2(kAdcDrdyPins[2], kAdcResetPin, ADS1256::PIN_UNUSED, kAdcCsPins[2],
             kAdcVrefVolts);

ADS1256 *const kAdcs[] = {&adc0, &adc1, &adc2};

File database_file;
char rowBuffer[128];
uint16_t samplesSinceFlush = 0;
}  // namespace

static void halt(const __FlashStringHelper *msg) {
  Serial.println(msg);
  while (true) {
    delay(1000);
  }
}

static void initChipSelects(void) {
  pinMode(kSdChipSelect, OUTPUT);
  digitalWrite(kSdChipSelect, HIGH);
  for (uint8_t i = 0; i < kChannelCount; i++) {
    pinMode(kAdcCsPins[i], OUTPUT);
    digitalWrite(kAdcCsPins[i], HIGH);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("3x Geophone + ADS1256 + SD logger"));

  initChipSelects();
  SPI.begin();

  Serial.println(F("Init ADS1256 channels..."));
  for (uint8_t i = 0; i < kChannelCount; i++) {
    kAdcs[i]->InitializeADC();
    digitalWrite(kAdcCsPins[i], HIGH);
    Serial.print(F("  CH"));
    Serial.println(i);
  }
  Serial.println(F("ADS1256 ready"));

  Serial.println(F("Init SD card..."));
  for (uint8_t i = 0; i < kChannelCount; i++) {
    digitalWrite(kAdcCsPins[i], HIGH);
  }
  if (!SD.begin(kSdChipSelect)) {
    halt(F("SD init failed (CS=D10, MOSI=D11, MISO=D12, SCK=D13)"));
  }
  Serial.println(F("SD ready"));

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
  const unsigned long t_us = micros();
  long counts[kChannelCount];
  float volts[kChannelCount];

  for (uint8_t ch = 0; ch < kChannelCount; ch++) {
    counts[ch] = kAdcs[ch]->readSingle();
    volts[ch] = kAdcs[ch]->convertToVoltage(counts[ch]);
  }

  char v0[12];
  char v1[12];
  char v2[12];
  dtostrf(volts[0], 0, 5, v0);
  dtostrf(volts[1], 0, 5, v1);
  dtostrf(volts[2], 0, 5, v2);
  snprintf(rowBuffer, sizeof(rowBuffer), "%lu,%ld,%s,%ld,%s,%ld,%s", t_us,
           counts[0], v0, counts[1], v1, counts[2], v2);

  database_file = SD.open(kFilename, FILE_WRITE);
  if (database_file) {
    database_file.println(rowBuffer);
    database_file.close();
    Serial.println(rowBuffer);
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
