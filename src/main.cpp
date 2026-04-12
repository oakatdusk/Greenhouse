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
const long timeout = 20000;     // 20 seconds before turning off

// for button debounce
const int debounceDelay = 50;

unsigned long lastYellowDebounce = 0;
bool lastYellowState = HIGH;

// For Blue Button
unsigned long lastBlueDebounce = 0;
bool lastBlueState = HIGH;

// --- SYSTEM STATUS & SAFETY ---
bool readyToWater = true; // True if all conditions (temp, water) are met
bool isWatering = false;   // Is the pump/relay currently ON?
bool wateredToday = false;

bool hasError = false;         // True if hardware fails
String statusMessage = "IDLE"; // Human-readable status (e.g., "Ready", "Soil Wet")
String lastError = "";         // Human-readable error (e.g., "RTC Lost", "Sensor A Fail")

// --- WATER BARREL (TANK) ---
bool waterLevelGood = false; // From Float Switch
float barrelTemp = 0.0;      // Water temperature

// --- SOIL MONITORING (ZONE A & B) ---
float soilTempA = 0.0;
int soilMoistureA = 0; // %
float soilTempB = 0.0;
int soilMoistureB = 0; // %

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

  // yellow button: cycle through display pages
  bool currentYellowState = digitalRead(YELLOW_BUTTON_PIN);
  if (currentYellowState != lastYellowState && currentYellowState == LOW && (millis() - lastYellowDebounce > debounceDelay))
  {

    lastActivity = millis(); // Reset inactivity timer
    lastYellowDebounce = millis();
    pageState = (pageState % 3) + 1; // this cycles 1, 2, 3 automatically
    u8g2.setPowerSave(0);            // Wake up OLED if it was asleep
    drawPage(pageState, now);
    Serial.print("Yellow pressed - page changed to: ");
    Serial.println(pageState);
  }
  lastYellowState = currentYellowState;

  // blue button: toggle watering
  bool currentBlueState = digitalRead(BLUE_BUTTON_PIN);
  if (currentBlueState != lastBlueState && currentBlueState == LOW && (millis() - lastBlueDebounce > debounceDelay))
  {
    lastBlueDebounce = millis();

    if (isWatering)
    {
      isWatering = false;
      statusMessage = "Watering Stopped";
    }
    else
    {
      // Only allow starting watering if conditions are met
      if (readyToWater)
      {
        isWatering = true;
        statusMessage = "Watering Started";
      }
      else
      {
        statusMessage = "Cannot Water";
      }
    }
    pageState = 1;
    drawPage(pageState, now);
    u8g2.setPowerSave(0); // Wake up OLED if it was asleep
    lastActivity = millis(); // Reset inactivity timer
    digitalWrite(RELAY_PIN, isWatering); // Physically turn pump on/off
    digitalWrite(BLUE_LED_PIN, isWatering); // Light up button
    Serial.print("Blue pressed - status changed to: ");
    Serial.println(statusMessage);

  }
  lastBlueState = currentBlueState;

  

  if (millis() - lastActivity > timeout) //turn off display after 20 seconds of inactivity
  {
    pageState = 0;        // Set state to OFF
    u8g2.setPowerSave(1); // Put OLED into low-power sleep
  }
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
    lastError = "RTC ERROR";
    hasError = true;
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
  // --- PART A: THE SAFETY HEADER (Shows on every page) ---
  if (hasError)
  {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawBox(0, 0, 128, 11); // Draw a black bar at the top
    u8g2.setDrawColor(0);        // Switch to "white text on black"
    u8g2.drawStr(2, 9, "ERR:");
    u8g2.drawStr(30, 9, lastError.c_str());
    u8g2.setDrawColor(1); // Switch back to normal
  }
  else
  {
    // Show normal system status if no errors
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(0, 9);
    u8g2.print(statusMessage);
  }

  u8g2.drawHLine(0, 12, 128); // Divider line under the header
  u8g2.setFont(u8g2_font_6x10_tf);
  switch (state)
  {
  case 1: // STATUS PAGE
    u8g2.drawStr(0, 30, "STATUS PAGE");
    u8g2.setCursor(0, 45); // Move down to the next "slot"
    u8g2.printf("%02d:%02d", t.hour(), t.minute());
    break;
  case 2: // TANK PAGE
    u8g2.drawStr(0, 30, "RAIN BARREL");
    // Add your float switch logic here
    break;
  case 3:                  // TEMP PAGE
    u8g2.setCursor(0, 30); // Move down to the next "slot"
    u8g2.printf("A | M: %d%% ", soilMoistureA);
    u8g2.printf("T: %.1f", soilTempA);
    u8g2.drawUTF8(u8g2.getCursorX(), u8g2.getCursorY(), "°C");

    u8g2.setCursor(0, 45); // Move down again
    u8g2.printf("B | M: %d%% ", soilMoistureB);
    u8g2.printf("T: %.1f", soilTempB);
    u8g2.drawUTF8(u8g2.getCursorX(), u8g2.getCursorY(), "°C");
    break;
  }
  u8g2.sendBuffer();
}