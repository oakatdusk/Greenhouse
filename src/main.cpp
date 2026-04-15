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

//functions from libraries
RTC_DS3231 rtc; //for the RTC
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, SCL_PIN, SDA_PIN); //for the OLED display
OneWire oneWire(ONE_WIRE_BUS); //set up onewire instance for the temp sensors, using the defined pin
DallasTemperature sensors(&oneWire); //pass OneWire reference to DallasTemperature library


// FUNCTIONS___________________________________________________________________________________
// initialization functions
void initPins();
void initSerial();
void initRTC();

void evaluateConditions();

//sensor functions
void waterLevelCheck();
void readTempSensors();
void readSoilSensors();

// display functions
void pageStatus(DateTime t);
void drawPage(int state, DateTime t);

//watering functions
void startWatering();
void stopWatering();
void handleWatering();

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
bool blueAlreadyPressed = false; // To track if blue button was already pressed (for toggling)
bool lastBlueState = HIGH;

// --- SYSTEM STATUS & SAFETY ---
bool readyToWater = true; // True if all conditions (temp, water) are met
bool isWatering = false;  // Is the pump/relay currently ON?
bool wateredToday = false;
bool wateringStopped = false; // True if watering was stopped due to a condition

bool hasError = false;         // True if hardware fails
String statusMessage = "IDLE"; // Human-readable status (e.g., "Ready", "Soil Wet")
String lastError = "";         // Human-readable error (e.g., "RTC Lost", "Sensor A Fail")

// --- WATER BARREL (TANK) ---
bool waterLevelGood = false; // From Float Switch

//temperatures
float barrelTemp = 0.0;      // Water temperature
float soilTempA = 0.0;
float soilTempB = 0.0;


//for soil moisture sensors

int soilMoistureA = 0; // %
int soilMoistureB = 0; // %

const int DRY_VALUEA = 3326;  // Value in dry air
const int DRY_VALUEB = 3326;  // Value in dry air
const int WET_VALUEA = 1053;  // Value in a glass of water
const int WET_VALUEB = 1053;  // Value in a glass of water

uint32_t soilASum = 0; //we use a sum so that we can average readings to get a more stable value
uint32_t soilBSum = 0;
int soilSampleCount = 0;
const int maxSoilSamples = 20; // How many samples to average
unsigned long lastSoilRead = 0;
unsigned long soilReadInterval = 100; // Read soil moisture every 100 ms until we have enough samples to average

//temp sensor stuff
DeviceAddress barrelAddr = { 0x28, 0x64, 0x11, 0x25, 0x00, 0x00, 0x00, 0x41 };
DeviceAddress soilAAddr  = { 0x28, 0x8C, 0x67, 0x25, 0x00, 0x00, 0x00, 0x38 };
DeviceAddress soilBAddr  = { 0x28, 0x32, 0xB8, 0x24, 0x00, 0x00, 0x00, 0x92 };



//timing
unsigned long wateringDuration =  1 * 60 * 1000; //how long to water for
unsigned long wateringCurrentDuration = 0; //how long we've been watering so far
unsigned long lastSensorRead = 0; //when the sensors were last read
const unsigned long sensorInterval = 5000; // Read every 5 seconds

void setup()
{
  initPins(); //initalize pins for all the outputs and inputs
  initSerial(); //start serial communication for debugging
  Wire.begin(SDA_PIN, SCL_PIN, 10000); // 100 kHz I2C
  u8g2.begin(); //initialize the OLED display
  initRTC(); //initialize the RTC
  sensors.begin(); //initialize the temperature sensors
  sensors.setWaitForConversion(false); //so it does not block all code as it converts
  lastActivity = millis(); // Initialize this so the display doesn't sleep immediately
  
}

void loop()
{
  DateTime now = rtc.now();

  waterLevelCheck();
if (millis() - lastSensorRead > sensorInterval) {
  readTempSensors();
  soilSampleCount = 0; //reset soil sample count after reading temps
  lastSensorRead = millis();
}
if (soilSampleCount < maxSoilSamples && millis() - lastSoilRead > soilReadInterval) {
readSoilSensors();
  lastSoilRead = millis();
}




  evaluateConditions();



  // yellow button: cycle through display pages
  bool currentYellowState = digitalRead(YELLOW_BUTTON_PIN);
  if (currentYellowState != lastYellowState && currentYellowState == LOW && (millis() - lastYellowDebounce > debounceDelay))
  {

    lastActivity = millis(); // Reset inactivity timer
    lastYellowDebounce = millis();
    pageState++;
    if (pageState > 3) pageState = 1; // Cycle back to first page (skip OFF) 
    u8g2.setPowerSave(0);            // Wake up OLED if it was asleep
    Serial.print("Yellow pressed - page changed to: ");
    Serial.println(pageState);
  }
  lastYellowState = currentYellowState;

  // blue button: toggle watering
  bool currentBlueState = digitalRead(BLUE_BUTTON_PIN);
  if (currentBlueState != lastBlueState && currentBlueState == LOW && (millis() - lastBlueDebounce > debounceDelay))
  {
    u8g2.setPowerSave(0); 
    lastActivity = millis(); // Reset inactivity timer
    lastBlueDebounce = millis();
   
    if (pageState != 4) {
      // First press: Jump to confirmation page
      pageState = 4; 
    } 
    else {
      // Second press: Perform the action and go back to Status page
      pageState = 1; // Go back to Status page after confirming
      if (isWatering) {
        stopWatering();
        statusMessage = "Manual Stop";
        wateringStopped = true; // Mark that watering was stopped manually
        digitalWrite(YELLOW_LED_PIN, HIGH); // Light up yellow LED to indicate manual stop
      } 
      else if (readyToWater) {
        startWatering();
      }
      else
      {
        statusMessage = "Cannot Water";
        digitalWrite(YELLOW_LED_PIN, HIGH); // Light up yellow LED to indicate we can't water
      }
  
    }
}
  lastBlueState = currentBlueState;

  if (millis() - lastActivity > timeout) // turn off display after 20 seconds of inactivity
  {
    pageState = 0;        // Set state to OFF
    u8g2.setPowerSave(1); // Put OLED into low-power sleep
  }
  else {
    // Only update the display if we're on a page (not OFF)
    if (pageState != 0) {
      drawPage(pageState, now);
    }
  }
}

void initPins()
{

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

    // make sure everything, especially the relay is off
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BLUE_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
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
    u8g2.setCursor(0, 45);
    if (waterLevelGood) {
      u8g2.print("Level: Good");
    } else {
      u8g2.print("Level: Empty");
    }
    
    u8g2.setCursor(0, 60);
    u8g2.printf("Temp: %.1f", barrelTemp);
    u8g2.drawUTF8(u8g2.getCursorX(), u8g2.getCursorY(), "°C");
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
    case 4: // CONFIRMATION PAGE
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawBox(0, 15, 128, 20); // Box in the middle
    u8g2.setDrawColor(0);
    if (!isWatering) {
      u8g2.drawStr(10, 28, "START WATERING?");
    } else {
      u8g2.drawStr(15, 28, "STOP WATERING?");
    }
    u8g2.setDrawColor(1);
    u8g2.drawStr(5, 55, "Press blue to confirm");
    break;
  }
  u8g2.sendBuffer();
}

void startWatering() {

  if (isWatering) {
    return; // Already watering, do nothing
  }
isWatering = true;
wateringStopped = false; // Reset this in case it was previously stopped
digitalWrite(RELAY_PIN, HIGH); //turn on the relay and the pump
digitalWrite(BLUE_LED_PIN, HIGH); // Light up the blue button LED to indicate watering is active
digitalWrite(YELLOW_LED_PIN, LOW); // Ensure yellow LED is off when watering starts
statusMessage = "Watering";
wateringCurrentDuration = millis(); // Start the watering timer

}

void stopWatering() {
  if (!isWatering) {
    return; // Not watering, do nothing
  }
  isWatering = false;
  digitalWrite(RELAY_PIN, LOW); //turn off the relay and the pump
  digitalWrite(BLUE_LED_PIN, LOW); // Turn off the blue button LED
}

void evaluateConditions() {
    readyToWater = true; // assume yes, then disqualify

    if (!waterLevelGood) {
        readyToWater = false;
        statusMessage = "Tank Empty";
    }
    // else if (soilMoistureA > 70 && soilMoistureB > 70) {
    //     readyToWater = false;
    //     statusMessage = "Soil Wet";
    // }
    // else if (barrelTemp < 5.0) {
    //     readyToWater = false;
    //     statusMessage = "Too Cold";
    // }
    // else if (wateredToday) {
    //     readyToWater = false;
    //     statusMessage = "Done Today";
    // }
    else {
        statusMessage = "Ready";
    }
}

void handleWatering() {
  if (isWatering && (millis() - wateringCurrentDuration >= wateringDuration)) {
    stopWatering();
    wateredToday = true;
    statusMessage = "Done Today";
  }
}

void waterLevelCheck() {
  waterLevelGood = (digitalRead(FLOAT_SWITCH_PIN) == LOW);
}

void readTempSensors() {
  sensors.requestTemperatures(); // Tell all sensors on the bus to prepare data

  // Fetch temperatures by their hard-coded unique addresses
  float tBarrel = sensors.getTempC(barrelAddr);
  float tSoilA  = sensors.getTempC(soilAAddr);
  float tSoilB  = sensors.getTempC(soilBAddr);

  // Safety Check: Only update global variables if the reading is valid
  // (DS18B20 returns -127.0 if the sensor is missing/disconnected)

  if ( tBarrel== -127 || tBarrel == 85) {
    tBarrel = NAN; // returns nan aka not a number if the sensor is not responding properly
    hasError = true;
    lastError = "TANK TEMP ERROR";
  }
  else {
    barrelTemp = tBarrel;
  }

  if (tSoilA == -127 || tSoilA == 85) {
    tSoilA = NAN; // 85 tends to show right after booting up and -127 shows if it can't get a proper reading
  }
  else {
    soilTempA = tSoilA;
  }
  if (tSoilB == -127 || tSoilB == 85) {
    tSoilB = NAN; // 85 tends to show right after booting up and -127 shows if it can't get a proper reading
  }
  else {
    soilTempB = tSoilB;
  }
}

void readSoilSensors() {
  // Add current reading to the running total
  soilASum += analogRead(SOIL_A_PIN);
  soilBSum += analogRead(SOIL_B_PIN);
  soilSampleCount++;
  lastSoilRead = millis();

  // Once we hit our target number of samples, calculate the result
  if (soilSampleCount >= maxSoilSamples) {
    int avgA = soilASum / maxSoilSamples;
    int avgB = soilBSum / maxSoilSamples;

    // Apply the map and constrain logic
    soilMoistureA = constrain(map(avgA, DRY_VALUEA, WET_VALUEA, 0, 100), 0, 100);
    soilMoistureB = constrain(map(avgB, DRY_VALUEB, WET_VALUEB, 0, 100), 0, 100);

    // RESET the counters for the next batch
    soilASum = 0;
    soilBSum = 0;
   
  }
}