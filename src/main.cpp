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
// initialization functions
void initPins();
void initSerial();
void initRTC();

// display functions
void pageStatus(DateTime t);
void drawPage(int state, DateTime t);

// VARIABLES___________________________________________________________________________________

// for display
int pageState = 0;              // 0=OFF, 1=Status, 2=Barrel, 3=Soil
unsigned long lastActivity = 0; // Tracks time in milliseconds
const long timeout = 5000;     // 10 seconds before turning off
unsigned long x = 0;

void setup()
{
  initPins();
  initSerial();
  Wire.begin(SDA_PIN, SCL_PIN, 10000); // 100 kHz I2C
  u8g2.begin();
  initRTC();
  lastActivity = millis(); // Initialize this so it doesn't sleep immediately
}

void loop()
{
  DateTime now = rtc.now();

  // 1. CHECK THE BUTTON
  // if (digitalRead(YELLOW_BUTTON_PIN) == LOW || Serial.available() > 0) {

  if (Serial.available() > 0)
  {
    while(Serial.available() > 0) {
      Serial.read(); 
    }
    lastActivity = millis(); // Reset inactivity timer

    pageState++; // Move to next page
    if (pageState > 3)
    { // If past last page, go back to Page 1
      pageState = 1;
    }

    u8g2.setPowerSave(0); // Wake up OLED if it was asleep
    Serial.print("Page changed to: ");
    Serial.println(pageState);
  }

  // 2. CHECK FOR INACTIVITY
  if (millis() - lastActivity > timeout)
  {
    pageState = 0;        // Set state to OFF
    u8g2.setPowerSave(1); // Put OLED into low-power sleep
  }

  // 3. DISPLAY THE CORRECT PAGE
  if (pageState > 0)
  {
    drawPage(pageState, now); // Custom function to draw based on state
  }
  // Print current time
  // Serial.printf("Date: %02d/%02d/%04d | Time: %02d:%02d:%02d\n",
  //               now.day(), now.month(), now.year(),
  //               now.hour(), now.minute(), now.second());
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

void initSerial()
{
  Serial.begin(115200);
  while (!Serial)
    delay(10); // wait for serial port to connect. Needed for native USB
  Serial.println("Serial communication initialized.");
}

void initRTC()
{
  if (!rtc.begin())
  {
    Serial.println("Couldn't find RTC! Check your SDA/SCL wiring.");
    while (1)
      delay(10);
  }

  if (rtc.lostPower())
  {
    Serial.println("RTC lost power, setting the time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
}

void drawPage(int state, DateTime t)
{
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  switch (state)
  {
  case 1: // SOIL PAGE
    u8g2.drawStr(0, 10, "SOIL MOISTURE 1");
    // Add your soil sensor logic here
    break;
  case 2: // TANK PAGE
    u8g2.drawStr(0, 10, "WATER LEVEL 2");
    // Add your float switch logic here
    break;
  case 3: // TEMP PAGE
    u8g2.drawStr(0, 10, "AIR TEMP 3");
    u8g2.setCursor(0, 30);
    u8g2.print(t.hour()); // Example of using our DateTime "now"
    break;
  }
  u8g2.sendBuffer();
}