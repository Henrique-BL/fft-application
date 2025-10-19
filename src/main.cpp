#include <Adafruit_MPU6050.h>
#include <Wire.h>
#include <SD.h>

Adafruit_MPU6050 mpu;
const int chipSelect = 5;  // CS pin for ESP32
const char filename[] = "/data.csv";
File database_file;
String dataBuffer;
unsigned long lastMillis = 0;

// Sensor data arrays
float acceleration[3];
float gyroscope[3];
float temperature;
int i = 0;

void setup() {
  
  Serial.begin(115200);
  // Remove blocking serial wait for ESP32 compatibility
  delay(1000); // Give time for serial to initialize
  Serial.println(F("MPU6050 and SD card demo"));
  // dataBuffer.reserve(1024);
  
  // Initialize I2C for ESP32
  Wire.begin();
  Wire.setClock(100000); // Set I2C clock to 100kHz for better compatibility

  Serial.println(F("Attempting to initialize MPU6050..."));
  if (!mpu.begin()) {
    Serial.println(F("Sensor init failed"));
    Serial.println(F("Check I2C connections: SDA=D21, SCL=D22"));
    while (1)
      yield();
  }
  Serial.println(F("MPU6050 initialized successfully"));

  Serial.println(F("Attempting to initialize SD card..."));
  if (!SD.begin(chipSelect)) {
    Serial.println(F("SD card initialization failed"));
    Serial.println(F("Check SPI connections: CS=D5, MOSI=D23, MISO=D19, SCK=D18"));
    while (1)
      yield();
  }
  Serial.println(F("SD card initialized successfully"));
  
  // Write CSV headers
  bool file_exists = SD.exists(filename);
  if (file_exists) {
    Serial.println(F("File already exists"));
  }else{
    database_file = SD.open(filename, FILE_WRITE);
    if (database_file) {
      database_file.println("timestamp,accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z,temperature\n");
      database_file.close();
      Serial.println(F("CSV headers written to SD card."));
    }else{
      Serial.println(F("Error opening file for headers."));
      while (1);
    }
  }
}

void loop() {
  sensors_event_t a, g, temp;
  unsigned long now = millis();
  mpu.getEvent(&a, &g, &temp);
  acceleration[0] = a.acceleration.x;
  acceleration[1] = a.acceleration.y;
  acceleration[2] = a.acceleration.z;
  gyroscope[0] = g.gyro.x;
  gyroscope[1] = g.gyro.y;
  gyroscope[2] = g.gyro.z;
  temperature = temp.temperature;

  // Add data to buffer
  dataBuffer += String(now) + "," +
                String(acceleration[0], 2) + "," +
                String(acceleration[1], 2) + "," +
                String(acceleration[2], 2) + "," +
                String(gyroscope[0], 2) + "," +
                String(gyroscope[1], 2) + "," +
                String(gyroscope[2], 2) + "," +
                String(temperature, 2) + "\n";

  // Try to write buffer to file
  database_file = SD.open(filename, FILE_WRITE);
  if (database_file) {
    // Write all buffered data
    database_file.write((const uint8_t*)dataBuffer.c_str(), dataBuffer.length());
    database_file.close();
    i++;
    // Clear buffer after successful write
    dataBuffer.remove(0, dataBuffer.length());
    Serial.println(F("Buffer written to file and cleared"));
  }
  else {
    Serial.println(F("File not available, data buffered"));
  }
  if (i > 3) {
    
    database_file = SD.open(filename, FILE_READ);
    if (database_file) {
      while (database_file.available()) {
        Serial.write(database_file.read());
      }
      database_file.close();
    }
    else {
      Serial.println(F("Error opening file for reading"));
    }
  }
  
  delay(100);
}

