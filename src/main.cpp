// LIBRARIES_________________________________________________________________________________
#include <Arduino.h> //kinda always needed
#include <U8g2lib.h> //for OLED
#include <U8g2lib.h>
#include <Wire.h>              //for I2C to work
#include "RTClib.h"            //to make RTC work
#include "OneWire.h"           //allows us to have several temp sensors on one wire
#include "DallasTemperature.h" //to work with DS18B20 temp sensors

// PIN DEFINITIONS____________________________________________________________________________

// Sensors
#define SOIL_A_PIN 34       // Soil moisture sensor A (analog input)
#define SOIL_B_PIN 35       // Soil moisture sensor B (analog input)
#define ONE_WIRE_BUS 4      // Temperature sensors (digital)
#define FLOAT_SWITCH_PIN 27 // Float switch (digital input)

// Buttons
#define BLUE_BUTTON_PIN 25   // blue button, to manually turn watering on or off
#define YELLOW_BUTTON_PIN 26 // yellow button, to turn on and switch between OLED screens

// outputs
#define BLUE_LED_PIN 13   // Blue button LED, to show watering status (digital output)
#define YELLOW_LED_PIN 14 // Yellow button LED, to show overall status (digital output)
#define RELAY_PIN 18      // Relay for the pump (digital output)
#define BUILT_IN_LED 2    // the built in LED

// I2C
#define SDA_PIN 21
#define SCL_PIN 22
#define RTC_SQW_PIN 32

RTC_DS3231 rtc;

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, SCL_PIN, SDA_PIN);

// FUNCTIONS___________________________________________________________________________________
void initPins();

// VARIABLES___________________________________________________________________________________

void setup()
{
  initPins();
  //  serial monitor setup
  Serial.begin(115200);
  delay(1000);
  Serial.println("Serial monitor set up.");
  Wire.begin(SDA_PIN, SCL_PIN, 100000); // 100 kHz I2C
  u8g2.begin();
  // Check if RTC is connected
  if (!rtc.begin())
  {
    Serial.println("Couldn't find RTC! Check your SDA/SCL wiring.");
    while (1)
      delay(10);
  }

  if (rtc.lostPower())
  {
    Serial.println("RTC lost power, setting the time...");
    // This sets the RTC to the date & time this code was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  rtc.writeSqwPinMode(DS3231_SquareWave1Hz);
  Serial.println("RTC Test Initialized!");
}

void loop()
{
  DateTime now = rtc.now();
  // Print current time
  Serial.printf("Date: %02d/%02d/%04d | Time: %02d:%02d:%02d\n",
                now.day(), now.month(), now.year(),
                now.hour(), now.minute(), now.second());

  // Check the physical SQW signal on Pin 32
  //If the wire is correct, the built-in LED will blink every second
  int sqwState = digitalRead(RTC_SQW_PIN);
  digitalWrite(BUILT_IN_LED, !sqwState);

  delay(500);
}

void initPins()
{
  // make sure everything, especially the relay is off
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BLUE_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);

  // Inputs
  pinMode(FLOAT_SWITCH_PIN, INPUT_PULLUP);
  pinMode(BLUE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(YELLOW_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RTC_SQW_PIN, INPUT_PULLUP);

  // Outputs
  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUILT_IN_LED, OUTPUT);
}