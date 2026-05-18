// Arduino Uno firmware: reads a geophone signal through the full analog chain
// and logs 24-bit samples to a microSD card as CSV (timestamp_us, counts, volts).
//
// Complete signal chain (simulation mirrors real hardware):
//
//   Geophone (geophone.chip.c)
//     Generates a bipolar AC sine wave as two anti-phase legs biased at 2.5 V.
//     OUT_P = 2.5 V + signal  |  OUT_N = 2.5 V - signal
//
//   Instrumentation Amplifier (instamp.chip.c)
//     IN_P/IN_N receive geophone OUT_P/OUT_N.
//     Amplifies the differential (OUT_P - OUT_N) and adds a 2.5 V offset:
//       AIN0 = gain * 2 * signal + 2.5 V
//     VREF pin re-emits the 2.5 V offset:
//       AIN1 = 2.5 V  (fed directly into ADS1256 AIN1 for bias cancellation)
//
//   ADS1256 24-bit ADC (ads1256.chip.c)
//     AIN0 - AIN1 subtracts the 2.5 V bias in hardware, yielding:
//       counts = (gain * 2 * signal / Vref) * 8388607
//     The Arduino reads this result via SPI after DRDY goes LOW.
//
//   Arduino (this file)
//     Drives ADS1256 CS/RESET (D9/D7) and monitors DRDY (D8).
//     SD card shares the SPI bus on a separate CS line (D10).
//     Each sample is formatted as CSV and appended to data.csv.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// Define to dump CSV back over serial after a few writes (debug only).
// #define DEBUG_READBACK

namespace {
// SD card and ADS1256 sit on the same hardware SPI bus (D11/D12/D13);
// their CS pins are kept HIGH whenever the other device is addressed.
constexpr uint8_t kSdChipSelect  = 10;   // SD  CS -- deselected while ADS1256 talks
constexpr uint8_t kAdcChipSelect =  9;   // ADS1256 CS -- driven by ads1256WriteRegister / ads1256ReadDifferential01
constexpr uint8_t kAdcDrdyPin    =  8;   // ADS1256 DRDY -- LOW signals a conversion is ready
constexpr uint8_t kAdcResetPin   =  7;   // ADS1256 RESET -- pulled LOW during initAds1256() to clear state

constexpr char kFilename[] = "data.csv";
constexpr uint16_t kFlushEverySamples = 16;

// ADS1256 reference voltage; must match the instamp VREF attribute (default 2.5 V)
// so countsToVolts() converts raw counts back to the original bipolar signal.
constexpr float kAdcVrefVolts = 2.5f;
// PGA gain applied in the ADS1256 ADCON register (gain=1 => 0x00 in lower 3 bits).
constexpr float kAdcGain = 1.0f;

constexpr unsigned long kDrdyTimeoutUs = 50000UL;

// ADS1256 SPI command bytes used during init and per-sample acquisition.
constexpr uint8_t kCmdRdata = 0x01;   // Triggers conversion and stages 24-bit result
constexpr uint8_t kCmdWreg  = 0x50;   // WREG opcode; OR with register address
constexpr uint8_t kCmdReset = 0xFE;   // Software reset; restores register defaults
constexpr uint8_t kRegAdcon = 0x02;   // ADCON register: holds PGA gain bits

// SPI runs at 1 MHz, MODE1; the ADS1256 chip model in ads1256.chip.c samples
// DIN on SCK falling and shifts DOUT on SCK rising to match this mode.
SPISettings ads1256Spi(1000000, MSBFIRST, SPI_MODE1);

File database_file;

// Tight CSV row buffer kept inside SRAM-friendly bounds for the Uno's 2 KB.
char rowBuffer[48];
uint16_t samplesSinceFlush    = 0;
uint16_t writesSinceReadback  = 0;
}  // namespace

// Blocks until the ADS1256 DRDY pin goes LOW, indicating that the chip has
// finished converting the current differential sample (AIN0 - AIN1) and the
// 24-bit result is ready to be clocked out via RDATA.
static bool waitDrdyLow(unsigned long timeoutUs) {
  unsigned long start = micros();
  while (digitalRead(kAdcDrdyPin) != LOW) {
    if ((micros() - start) >= timeoutUs) {
      return false;
    }
  }
  return true;
}

// Writes a single byte to an ADS1256 register over SPI.
// CS is held LOW for the duration so the SD card (CS=D10, still HIGH)
// does not interpret these bytes as addressed to it.
static void ads1256WriteRegister(uint8_t reg, uint8_t value) {
  digitalWrite(kAdcChipSelect, LOW);
  SPI.beginTransaction(ads1256Spi);
  SPI.transfer(kCmdWreg | (reg & 0x0F));  // WREG opcode + register address
  SPI.transfer(0x00);                      // Writes exactly one register.
  SPI.transfer(value);
  SPI.endTransaction();
  digitalWrite(kAdcChipSelect, HIGH);
}

// Issues RDATA (0x01) to the ADS1256 and clocks back the 24-bit signed result.
// The ADS1256 chip model (ads1256.chip.c) stages this value by computing
// (AIN0 - AIN1) which cancels the instamp VREF bias before encoding.
static int32_t ads1256ReadDifferential01() {
  digitalWrite(kAdcChipSelect, LOW);
  SPI.beginTransaction(ads1256Spi);
  SPI.transfer(kCmdRdata);
  delayMicroseconds(2);  // Datasheet t6: 50 DRDY cycles before data is valid.
  int32_t raw = ((int32_t)SPI.transfer(0xFF) << 16) |
                ((int32_t)SPI.transfer(0xFF) <<  8) |
                 (int32_t)SPI.transfer(0xFF);
  SPI.endTransaction();
  digitalWrite(kAdcChipSelect, HIGH);

  // Sign-extends from 24-bit two's complement to 32-bit for arithmetic.
  if (raw & 0x800000) {
    raw |= (int32_t)0xFF000000;
  }
  return raw;
}

// Configures the ADS1256 for use with the instrumentation amplifier output:
//   1. Resets the chip to clear any leftover state from a previous run.
//   2. Writes ADCON with PGA=1 (gain bits = 0b000), matching kAdcGain=1.0f
//      so countsToVolts() uses the same scale factor as the chip model.
//   3. Waits for DRDY LOW to confirm the chip is ready to accept RDATA commands.
static bool initAds1256() {
  pinMode(kAdcChipSelect, OUTPUT);
  pinMode(kAdcResetPin,   OUTPUT);
  pinMode(kAdcDrdyPin,    INPUT_PULLUP);

  // Both CS lines start HIGH to prevent spurious transactions on the shared SPI bus.
  digitalWrite(kAdcChipSelect, HIGH);
  digitalWrite(kAdcResetPin,   HIGH);

  SPI.begin();
  delay(2);

  // Software reset clears the ADS1256 protocol state machine (ads1256.chip.c
  // responds to 0xFE by calling reset_device_state()).
  digitalWrite(kAdcChipSelect, LOW);
  SPI.beginTransaction(ads1256Spi);
  SPI.transfer(kCmdReset);
  SPI.endTransaction();
  digitalWrite(kAdcChipSelect, HIGH);
  delay(2);

  // PGA gain = 1 (ADCON bits [2:0] = 0b000); 0x20 keeps the clock-out rate bits
  // at their default.  gain_from_adcon() in ads1256.chip.c maps this to 1.0f.
  ads1256WriteRegister(kRegAdcon, 0x20);

  return waitDrdyLow(kDrdyTimeoutUs);
}

// Converts a raw 24-bit count from the ADS1256 back to the equivalent voltage
// at the instrumentation amplifier output (before the 2.5 V bias was cancelled).
// The instamp gain and the kAdcGain PGA factor must be consistent with this formula.
static float countsToVolts(int32_t counts) {
  return ((float)counts / 8388607.0f) * (kAdcVrefVolts / kAdcGain);
}

// Reports a fatal initialisation failure over serial and parks the MCU;
// on real hardware a watchdog reset would recover; in simulation the loop stops.
static void halt(const __FlashStringHelper *msg) {
  Serial.println(msg);
  while (true) {
    delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("Geophone + ADS1256 + SD logger"));

  // Both SPI slaves must be deselected before SPI.begin() to avoid bus contention.
  pinMode(kSdChipSelect,  OUTPUT);
  digitalWrite(kSdChipSelect,  HIGH);
  pinMode(kAdcChipSelect, OUTPUT);
  digitalWrite(kAdcChipSelect, HIGH);

  // Initialises the ADS1256; if DRDY does not respond the instamp or
  // ADC chip model may not be wired correctly in diagram.json.
  Serial.println(F("Init ADS1256..."));
  if (!initAds1256()) {
    halt(F("ADS1256 not ready (DRDY timeout)"));
  }
  Serial.println(F("ADS1256 ready"));

  // Deselects the ADS1256 before SD.begin() so the SD library's own SPI
  // transactions to D10 do not accidentally toggle the ADC's CS line.
  Serial.println(F("Init SD card..."));
  digitalWrite(kAdcChipSelect, HIGH);
  if (!SD.begin(kSdChipSelect)) {
    halt(F("SD init failed (CS=D10, MOSI=D11, MISO=D12, SCK=D13)"));
  }
  Serial.println(F("SD ready"));

  // Creates data.csv with a header row on the first run; appends on subsequent runs.
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
  unsigned long t_us = micros();

  // Waits for the ADS1256 to signal that the differential conversion is complete;
  // the chip model holds DRDY LOW permanently, so this returns immediately in simulation.
  if (!waitDrdyLow(kDrdyTimeoutUs)) {
    Serial.println(F("ADC DRDY timeout"));
    delay(10);
    return;
  }

  // Reads AIN0 - AIN1: the ADC has already subtracted the instamp VREF bias,
  // returning the true bipolar geophone-equivalent signal in 24-bit signed counts.
  int32_t counts = ads1256ReadDifferential01();
  float volts    = countsToVolts(counts);

  // dtostrf avoids pulling in the printf-float library, preserving flash space.
  char vbuf[12];
  dtostrf(volts, 0, 5, vbuf);
  snprintf(rowBuffer, sizeof(rowBuffer), "%lu,%ld,%s", t_us, (long)counts, vbuf);

  // Opens, writes, and closes the file every sample; this is SD-safe but slow --
  // kFlushEverySamples controls how often a progress message is printed.
  database_file = SD.open(kFilename, FILE_WRITE);
  if (database_file) {
    database_file.println(rowBuffer);
    database_file.close();
    Serial.println(rowBuffer);
    samplesSinceFlush++;
    if (samplesSinceFlush >= kFlushEverySamples) {
      Serial.print(F("Logged "));
      Serial.print(samplesSinceFlush);
      Serial.println(F(" samples"));
      samplesSinceFlush = 0;
      writesSinceReadback++;
    }
  } else {
    Serial.println(F("SD write failed"));
  }

#ifdef DEBUG_READBACK
  // Dumps the full CSV file back over serial to verify the ADC-to-SD pipeline
  // end-to-end; enabled by defining DEBUG_READBACK at the top of this file.
  if (writesSinceReadback >= 2) {
    writesSinceReadback = 0;
    database_file = SD.open(kFilename, FILE_READ);
    if (database_file) {
      while (database_file.available()) {
        Serial.write(database_file.read());
      }
      database_file.close();
    }
  }
#endif
}
