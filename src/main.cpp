#include <Adafruit_MPU6050.h>
#include <Wire.h>
#include <SD.h>

Adafruit_MPU6050 mpu;
const uint8_t chipSelect = 10;

void setup() {

  Serial.begin(9600);
  while (!Serial) {
    ;
  }
  Serial.println(F("MPU6050 and SD card demo"));
  
  // Initialize I2C (Arduino Nano uses A4=SDA, A5=SCL by default)
  Wire.begin();
  Wire.setClock(100000); // Set I2C clock to 100kHz for better compatibility

  Serial.println(F("Attempting to initialize MPU6050..."));
  if (!mpu.begin()) {
    Serial.println(F("Sensor init failed"));
    while (1)
      yield();
  }

  Serial.println(F("Attempting to initialize SD card..."));
  if (!SD.begin(chipSelect)) {
    Serial.println(F("SD card initialization failed"));
    while (1)
      yield();
  }
  Serial.println(F("SD card initialized"));
  File dataFile = SD.open("wokwi.txt", FILE_WRITE); // Open the file "data.txt" on the SD card for writing
  
  if (dataFile) {
    dataFile.println(F("Hello, world!")); // Write data to the file
    dataFile.close(); // Close the file
    Serial.println(F("Data written to SD card."));
  }
  else {
    Serial.println(F("Error opening file."));
  }
}

void loop() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  Serial.print(F("Accelerometer "));
  Serial.print(F("X: "));
  Serial.print(a.acceleration.x, 1);
  Serial.print(F(" m/s^2, "));
  Serial.print(F("Y: "));
  Serial.print(a.acceleration.y, 1);
  Serial.print(F(" m/s^2, "));
  Serial.print(F("Z: "));
  Serial.print(a.acceleration.z, 1);
  Serial.println(F(" m/s^2"));

  Serial.print(F("Gyroscope "));
  Serial.print(F("X: "));
  Serial.print(g.gyro.x, 1);
  Serial.print(F(" rps, "));
  Serial.print(F("Y: "));
  Serial.print(g.gyro.y, 1);
  Serial.print(F(" rps, "));
  Serial.print(F("Z: "));
  Serial.print(g.gyro.z, 1);
  Serial.println(F(" rps"));

  Serial.print(F("Temperature: "));
  Serial.print(temp.temperature);
  Serial.print(F(" degC"));
  Serial.println(F(" "));

  delay(100);
}

