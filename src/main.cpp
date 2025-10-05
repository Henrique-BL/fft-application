/* 
| This is simple way to read the accelarometer, Gyroscope, and temperature sensor data
| The data are just read on the serial monitor and dispaly on the OLED
| For the VS Code of this little simulation: https://github.com/Bamamou/MPU6050_ESP32
*/

#include <Adafruit_MPU6050.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

// I2C pins for ESP32 (adjust if using different pins)
#define SDA_PIN 21
#define SCL_PIN 22

Adafruit_MPU6050 mpu;
Adafruit_SSD1306 display = Adafruit_SSD1306(128, 64, &Wire);

void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial time to initialize
  
  Serial.println("MPU6050 OLED demo");
  
  // Initialize I2C with explicit pins
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000); // Set I2C clock to 100kHz for better compatibility
  
  Serial.println("I2C initialized");
  Serial.println("Scanning for I2C devices...");
  
  // I2C scanner to help debug connectivity
  byte error, address;
  int nDevices = 0;
  
  for(address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      nDevices++;
    }
  }
  
  if (nDevices == 0) {
    Serial.println("No I2C devices found. Check wiring!");
  } else {
    Serial.print("Found ");
    Serial.print(nDevices);
    Serial.println(" I2C device(s)");
  }
  
  Serial.println("Attempting to initialize MPU6050...");
  if (!mpu.begin()) {
    Serial.println("Sensor init failed");
    Serial.println("Check:");
    Serial.println("- Wiring connections (SDA to pin 21, SCL to pin 22)");
    Serial.println("- Power supply (3.3V or 5V depending on module)");
    Serial.println("- Pull-up resistors on SDA/SCL lines");
    while (1)
      yield();
  }
  Serial.println("Found a MPU-6050 sensor");

  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for 128x32
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ; // Don't proceed, loop forever
  }
  display.display();
  delay(500); // Pause for 2 seconds
  display.setTextSize(1.5);
  display.setTextColor(WHITE);
  display.setRotation(0);
}

void loop() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  display.clearDisplay();
  display.setCursor(0, 0);

  Serial.print("Accelerometer ");
  Serial.print("X: ");
  Serial.print(a.acceleration.x, 1);
  Serial.print(" m/s^2, ");
  Serial.print("Y: ");
  Serial.print(a.acceleration.y, 1);
  Serial.print(" m/s^2, ");
  Serial.print("Z: ");
  Serial.print(a.acceleration.z, 1);
  Serial.println(" m/s^2");

  display.println("Accelerometer - m/s^2");
  display.setCursor(0, 10);
  display.print(a.acceleration.x, 1);
  display.print(", ");
  display.print(a.acceleration.y, 1);
  display.print(", ");
  display.print(a.acceleration.z, 1);
  display.println("");

  Serial.print("Gyroscope ");
  Serial.print("X: ");
  Serial.print(g.gyro.x, 1);
  Serial.print(" rps, ");
  Serial.print("Y: ");
  Serial.print(g.gyro.y, 1);
  Serial.print(" rps, ");
  Serial.print("Z: ");
  Serial.print(g.gyro.z, 1);
  Serial.println(" rps");

  Serial.print("Temperature: ");
  Serial.print(temp.temperature);
  Serial.print(" degC");
  Serial.println(" ");
  display.setCursor(0, 26);
  display.println("Gyroscope - rps");
  display.setCursor(0, 38);
  display.print(g.gyro.x, 1);
  display.print(", ");
  display.print(g.gyro.y, 1);
  display.print(", ");
  display.print(g.gyro.z, 1);
  display.println("");
  display.setCursor(0, 55);
  display.print("Temp: ");
  display.print(temp.temperature);
  display.print(" degC");

  display.display();
  delay(100);
}