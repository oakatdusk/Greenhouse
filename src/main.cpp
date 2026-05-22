// LIBRARIES_________________________________________________________________________________
#include <Arduino.h>           //kinda always needed
#include <U8g2lib.h>           //for OLED
#include <Wire.h>              //for I2C to work
#include "RTClib.h"            //to make RTC work
#include "OneWire.h"           //allows us to have several temp sensors on one wire
#include "DallasTemperature.h" //to work with DS18B20 temp sensors

// PIN DEFINITIONS____________________________________________________________________________

// Sensors
#define SOIL_A_PIN 34       // Soil moisture sensor A (analog input)
#define SOIL_B_PIN 35       // Soil moisture sensor B (analog input)
#define ONE_WIRE_BUS 4      // Temperature sensors (digital)
#define FLOAT_SWITCH_PIN 13 // Float switch (digital input)

// Buttons
#define BLUE_BUTTON_PIN 26 // blue button, to manually turn watering on or off
#define BLUE_LED_PIN 25    // Blue button LED, to show watering status (digital output)

#define YELLOW_BUTTON_PIN 27 // yellow button, to turn on and switch between OLED screens
#define YELLOW_LED_PIN 14    // Yellow button LED, to show overall status (digital output)

// outputs

#define RELAY_PIN 23   // Relay for the pump (digital output)
#define BUILT_IN_LED 2 // the built in LED

// I2C
#define SDA_PIN 21
#define SCL_PIN 22
#define RTC_SQW_PIN 19

// functions from libraries
RTC_DS3231 rtc;                                                     // for the RTC
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R1, SCL_PIN, SDA_PIN); // constructor line, setting up for the oled display
OneWire oneWire(ONE_WIRE_BUS);                                      // set up onewire instance for the temp sensors, using the defined pin
DallasTemperature sensors(&oneWire);

// VARIABLES___________________________________________________________________________________

// ERRORS___________________________________________________________________________________

enum ErrorCode
{
  ERR_RTC,
  ERR_I2C,
  ERR_PUMP_TIMEOUT,
  ERR_TANK_TEMP,
  ERR_SOILA_M,
  ERR_SOILB_M,
  ERR_SOILA_TEMP,
  ERR_SOILB_TEMP,
  ERR_COUNT
};

struct Error
{
  ErrorCode code;
  bool stopsWatering;
  bool active;
  const char *message;
};

Error errors[] = {
    {ERR_RTC, true, false, "RTC"},
    {ERR_I2C, true, false, "I2C"},
    {ERR_PUMP_TIMEOUT, true, false, "PUMP FAIL"},
    {ERR_TANK_TEMP, true, true, "TANK TEMP"},
    {ERR_SOILA_M, false, true, "SOIL A"},
    {ERR_SOILB_M, false, true, "SOIL B"},
    {ERR_SOILA_TEMP, false, true, "TEMP A"},
    {ERR_SOILB_TEMP, false, true, "TEMP B"},
};

enum SystemState
{
  STATE_IDLE,
  STATE_READY,
  STATE_ACTIVE,
  STATE_MANUALLY_STOPPED, // replaces manuallyStopped flag
  STATE_MANUALLY_STARTED, // replaces manuallyStarted flag
  STATE_DONE,
  STATE_ERROR,
  STATE_DELAY,
  STATE_SLEEP
};

struct StateInfo
{
  SystemState state;
  const char *message;
};

StateInfo states[] = {
    {STATE_IDLE, "IDLE"},
    {STATE_READY, "READY"},
    {STATE_ACTIVE, "ACTIVE"},
    {STATE_MANUALLY_STOPPED, "MAN. STOP"},
    {STATE_MANUALLY_STARTED, "MAN. START"},
    {STATE_DONE, "DONE"},
    {STATE_ERROR, "ERROR!"},
    {STATE_DELAY, "DELAY"},
    {STATE_SLEEP, "SLEEP"}};

SystemState currentState = STATE_IDLE;  // what the system is currently doing, for display and logic purposes
SystemState preErrorState = STATE_IDLE; // what we were doing before the error

// for display
int pageState = 0;              // 0=OFF, 1=Status, 2=Barrel, 3=Soil, 4=stop?, 5=stopped, 6=start?, 7=started, 8=moisture?
unsigned long lastActivity = 0; // Tracks time in milliseconds
const long timeout = 60 * 1000; // 60 seconds before turning off

const char *daysOfTheWeek[7] = { // so the display can show the day of the week
    "Sun",
    "Mon",
    "Tue",
    "Wed",
    "Thu",
    "Fri",
    "Sat"};

// for button debounce
const int debounceDelay = 50;

// --- SYSTEM STATUS & SAFETY ---
bool readyToWater = false; // True if all conditions (temp, water) are met
bool wateredToday = false;
bool timeToWater = false; // True if it's the scheduled time to water based on RTC

// --- WATER BARREL (TANK) ---
bool waterLevelGood = false; // From Float Switch = false;

// temperatures
float barrelTemp = 0.0;        // Water temperature
float barrelTempTarget = 20.0; // Minimum water temperature for watering
float soilTempA = 0.0;
float soilTempB = 0.0;

// for soil moisture sensors

int soilMoistureA = 0;       // %
int soilMoistureB = 0;       // %
int soilMoistureTarget = 50; //% target soil moisture percentage to stop watering
int newSoilMoistureTarget = 50;
bool settingMoistureTarget = false;

bool readingSoilMoisture = true; // flag to indicate if we're currently taking soil moisture readings
bool readingTemp = true;         // flag to indicate if we're currently taking temp readings

const int DRY_VALUEA = 3326; // Value in dry air
const int DRY_VALUEB = 3326; // Value in dry air
const int WET_VALUEA = 1053; // Value in a glass of water
const int WET_VALUEB = 1053; // Value in a glass of water

uint32_t soilASum = 0; // we use a sum so that we can average readings to get a more stable value
uint32_t soilBSum = 0;
int soilSampleCount = 0;
const int maxSoilSamples = 20; // How many samples to average
unsigned long lastSoilRead = 0;
unsigned long soilReadInterval = 100; // Read soil moisture every 100 ms until we have enough samples to average

// temp sensor addresses so we know which is which
DeviceAddress barrelAddr = {0x28, 0x64, 0x11, 0x25, 0x00, 0x00, 0x00, 0x41};
DeviceAddress soilAAddr = {0x28, 0x8C, 0x67, 0x25, 0x00, 0x00, 0x00, 0x38};
DeviceAddress soilBAddr = {0x28, 0x32, 0xB8, 0x24, 0x00, 0x00, 0x00, 0x92};

// watering timing
const unsigned long wateringDuration = 1 * 60 * 1000; // how long to water for (in ms)
unsigned long wateringStartTime = 0;                  // when we started watering
unsigned long wateringElapsed = 0;                    // total ms watered across all sessions
unsigned long totalElapsed = 0;                       // total elapsed time for watering, including current session
int wateringProgress = 0;                             // percentage of watering completed, for display
const uint wateringStartHour = 13;                    // what hour to start watering (24 hr format)

// sensor timing
unsigned long lastSensorRead = 0;          // when the sensors were last read
const unsigned long sensorInterval = 5000; // Read temp and moisture every 5 seconds

// FUNCTIONS___________________________________________________________________________________

// initialization functions
void initPins();
void initSerial();
void initRTC();

// logic functions
void evaluateConditions(); // check conditions to determine if ready to water
void checkMidnightReset(DateTime now);
void timeToWaterCheck(DateTime now); // check RTC to see if it's time to water based on the schedule

Error *getActiveError();
int countActiveErrors();
void setState(SystemState newState);
const char *getStateMessage(SystemState state);
bool hasStoppingError();

// sensor functions
bool waterLevelCheck();
void readTempSensors();
void readSoilSensors(); // this is used in kinda an async way, where we set a flag to start reading the soil moisture sensors in the main loop since it takes a while to get a stable reading and we don't want to block the whole loop while we're doing it

// display functions
void updateDisplay(DateTime t);
void drawCenteredText(const char *text, int y);
void displayTimeout();

// button functions
bool yellowButtonPress();
bool blueButtonPress();

void updateLEDs();

// watering functions
void handleWatering();
void automaticStart();

void manualStop();
void manualStart();

void setup()
{
  initPins();                           // initalize pins for all the outputs and inputs
  initSerial();                         // start serial communication for debugging
  Wire.begin(SDA_PIN, SCL_PIN, 100000); // 100 kHz I2C
  u8g2.begin();                         // initialize the OLED display
  initRTC();                            // initialize the RTC
  sensors.begin();                      // initialize the temperature sensors
  sensors.setWaitForConversion(false);  // so it does not block all code as it converts
  lastActivity = millis();              // Initialize this so the display doesn't sleep immediately
}

void loop()
{
  DateTime now = rtc.now();

  // reading all sensors
  waterLevelCheck();
  if (millis() - lastSensorRead > sensorInterval) // is it time to read the sensors again?
  {
    Serial.println("Current State: " + String(getStateMessage(currentState)));
    Serial.printf("Current Errors: %d\n", countActiveErrors());
    Serial.println("Active Errors:");
    for (int i = 0; i < ERR_COUNT; i++)
    {
      if (errors[i].active)
      {
        Serial.println("- " + String(errors[i].message));
      }
    }
    readingTemp = true;         // set flag to start reading temp sensors in the main loop (since it takes a while and we want to do it asynchronously)
    readingSoilMoisture = true; // set flag to start reading soil moisture in the main loop (since it takes a while and we want to do it asynchronously)
    lastSensorRead = millis();
  }
  readTempSensors();
  readSoilSensors();

  // buttons
  yellowButtonPress();
  blueButtonPress();

  // evaluate logic
  checkMidnightReset(now);
  timeToWaterCheck(now);
  evaluateConditions();

  // results of the logic
  automaticStart(); // starts watering if time is right and conditions are met and no stopping errors
  handleWatering(); // checks watering progress and turns off automatically when done

  // update outputs
  displayTimeout();
  if (pageState != 0)
  {
    updateDisplay(now);
  }
  updateLEDs();
}

void initPins()
{

  // Inputs
  pinMode(FLOAT_SWITCH_PIN, INPUT_PULLUP);
  pinMode(BLUE_BUTTON_PIN, INPUT);
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
    errors[ERR_RTC].active = true;
  }

  if (rtc.lostPower())
  {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

// checks for a button press and updates the page
bool yellowButtonPress()
{
  // yellow button: cycle through display pages
  static bool lastYellowState = HIGH;
  static unsigned long lastYellowDebounce = 0;
  bool currentYellowState = digitalRead(YELLOW_BUTTON_PIN);

  bool pressed =
      currentYellowState != lastYellowState &&
      currentYellowState == LOW &&
      millis() - lastYellowDebounce > debounceDelay;

  if (pressed)
  {
    lastActivity = millis(); // resets display inactivity timer

    lastYellowDebounce = millis();
    if (settingMoistureTarget)
    {
      soilMoistureTarget = newSoilMoistureTarget;
      pageState = 5;
      settingMoistureTarget = false;
    }
    else
    {
      pageState++;

      if (pageState > 4)
      {
        pageState = 1;
      }
    }
  }

  lastYellowState = currentYellowState;

  return pressed;
}

// checks for a blue button press which is used for manual start or stop
bool blueButtonPress()
{
  // blue button: toggle watering
  static bool lastBlueState = HIGH;
  static unsigned long lastBlueDebounce = 0;
  static bool blueAlreadyPressed = false; // To track if blue button was already pressed (for toggling)
  bool currentBlueState = digitalRead(BLUE_BUTTON_PIN);

  bool pressed =
      currentBlueState != lastBlueState &&
      currentBlueState == LOW &&
      millis() - lastBlueDebounce > debounceDelay;

  if (pressed)
  {
    lastActivity = millis(); // Reset inactivity timer
    lastBlueDebounce = millis();
    if (pageState == 4)
    {
      settingMoistureTarget = true;
      lastActivity = millis(); // Reset inactivity timer
      lastBlueDebounce = millis();
      newSoilMoistureTarget += 10;
      if (newSoilMoistureTarget > 100)
        newSoilMoistureTarget = 0;
    }
    else
    {
      if (!blueAlreadyPressed)
      {
        if (currentState == STATE_ACTIVE || currentState == STATE_MANUALLY_STARTED)
        {
          pageState = 6; // go to confirmation page for stopping
        }
        else
        {
          pageState = 8; // go to confirmation page for starting
        }
        blueAlreadyPressed = true; // Mark as pressed
      }
      else
      {
        // If already pressed, this is a confirmation press
        if (currentState == STATE_ACTIVE || currentState == STATE_MANUALLY_STARTED)
        {
          manualStop();
        }
        else
        {
          manualStart();
        }
        blueAlreadyPressed = false; // Reset for next toggle
      }
    }
  }

  lastBlueState = currentBlueState;

  return pressed;
}

void manualStop()
{
  setState(STATE_MANUALLY_STOPPED);
  pageState = 7; // go to manually stopped page
}

void manualStart()
{
  if (hasStoppingError())
  {
    setState(STATE_ERROR);
    pageState = 10; // go to manual start failed page
    return;         // If there is an error that should stop watering, do not allow manual start
  }
  else
  {
    setState(STATE_MANUALLY_STARTED);
    pageState = 8; // go to manually started page
  }
}

void setState(SystemState newState)
{
  if (newState == currentState)
    return; // if nothing has changed do nothing

  if (newState == STATE_ERROR)
  {
    preErrorState = currentState; // remember what we were doing before error
    digitalWrite(RELAY_PIN, LOW); // immediately stop watering if we hit an error, this is here just in case
  }

  // exit actions for old state
  if (currentState == STATE_ACTIVE || currentState == STATE_MANUALLY_STARTED)
  {
    digitalWrite(RELAY_PIN, LOW);
    totalElapsed = wateringElapsed + (millis() - wateringStartTime);
    wateringProgress = constrain(map(totalElapsed, 0, wateringDuration, 0, 100), 0, 100);
  }
  if (currentState == STATE_SLEEP)
  {
    // change update intervals back to normal
  }

  // entry actions for new state
  if (newState == STATE_ACTIVE || newState == STATE_MANUALLY_STARTED)
  {
    digitalWrite(RELAY_PIN, HIGH);
    wateringStartTime = millis();
  }

  if (newState == STATE_DONE)
  {
    wateringProgress = 100;
    wateredToday = true;
    timeToWater = false; // reset the time to water flag in case we hit done because of the schedule, so that it doesn't keep trying to water every loop
  }

  if (newState == STATE_SLEEP)
  {
    pageState = 0; // turn off display
    // add in something to slow down sensor readings and updates

    if (newState == STATE_IDLE)
    {
      wateringElapsed = 0;
      totalElapsed = 0;
      wateringProgress = 0;
    }
  }

  currentState = newState;
}

void evaluateConditions()
{
  // error appeared — save state and bail
  if (hasStoppingError())
  {
    if (currentState != STATE_ERROR)
      setState(STATE_ERROR);
    return;
  }
  // error cleared — recover
  if (currentState == STATE_ERROR && !hasStoppingError())
  {
    setState(STATE_IDLE);
  }

  // derive readyToWater from current conditions
  readyToWater = waterLevelCheck() && barrelTemp >= barrelTempTarget && !wateredToday && currentState != STATE_MANUALLY_STOPPED;
}

// checks watering duration and moisture and automatically turns watering off
void handleWatering()
{
  if (currentState != STATE_ACTIVE && currentState != STATE_MANUALLY_STARTED)
    return;

  totalElapsed = wateringElapsed + (millis() - wateringStartTime);
  wateringProgress = constrain(map(totalElapsed, 0, wateringDuration, 0, 100), 0, 100);

  if (totalElapsed >= wateringDuration ||
      (soilMoistureA >= soilMoistureTarget && soilMoistureB >= soilMoistureTarget && currentState == STATE_ACTIVE))
  {
    wateringProgress = 100;
    setState(STATE_DONE);
  }
}

void automaticStart()
{
  if (currentState == STATE_MANUALLY_STARTED || currentState == STATE_MANUALLY_STOPPED || currentState == STATE_ACTIVE || currentState == STATE_ERROR)
    return; // in these conditions we do not want automatic start
  if (timeToWater && readyToWater)
    setState(STATE_ACTIVE);
  else if (timeToWater && !readyToWater)
    setState(STATE_DELAY);
  else if (currentState == STATE_DELAY && readyToWater)
    setState(STATE_READY);
}

bool waterLevelCheck()
{
  waterLevelGood = (digitalRead(FLOAT_SWITCH_PIN) == HIGH);
  return waterLevelGood;
}

void readTempSensors()
{
  if (!readingTemp)
    return; // not time to read temp sensors yet
  static bool waitingForConversion = false;
  static unsigned long lastTempRequest = 0;
  if (!waitingForConversion)
  {
    sensors.requestTemperatures(); // Tell all sensors on the bus to prepare data
    waitingForConversion = true;
    lastTempRequest = millis();
  }
  if (millis() - lastTempRequest >= 750 && waitingForConversion) // DS18B20 max conversion time is 750 ms, so we wait at least that long before trying to read the data
  {
    lastTempRequest = millis();
    waitingForConversion = false;
    readingTemp = false;
    // Fetch temperatures by their hard-coded unique addresses
    float tBarrel = sensors.getTempC(barrelAddr);
    float tSoilA = sensors.getTempC(soilAAddr);
    float tSoilB = sensors.getTempC(soilBAddr);

    // Safety Check: Only update global variables if the reading is valid
    // (DS18B20 returns -127.0 if the sensor is missing/disconnected)

    errors[ERR_TANK_TEMP].active = (tBarrel == -127 || tBarrel == 85);
    if (!errors[ERR_TANK_TEMP].active)
      barrelTemp = tBarrel;

    errors[ERR_SOILA_TEMP].active = (tSoilA == -127 || tSoilA == 85);
    if (!errors[ERR_SOILA_TEMP].active)
      soilTempA = tSoilA;

    errors[ERR_SOILB_TEMP].active = (tSoilB == -127 || tSoilB == 85);
    if (!errors[ERR_SOILB_TEMP].active)
      soilTempB = tSoilB;
  }
}

void readSoilSensors()
{
  if (!(readingSoilMoisture && millis() - lastSoilRead > soilReadInterval))
  {
    return; // Not time to read soil moisture yet
  }

  // Add current reading to the running total
  soilASum += analogRead(SOIL_A_PIN);
  soilBSum += analogRead(SOIL_B_PIN);
  soilSampleCount++;
  lastSoilRead = millis();

  // Once we hit our target number of samples, calculate the result
  if (soilSampleCount >= maxSoilSamples)
  {
    int avgA = soilASum / maxSoilSamples;
    int avgB = soilBSum / maxSoilSamples;

    // check for weird readings BEFORE mapping
    errors[ERR_SOILA_M].active = (avgA < (WET_VALUEA - 200) || avgA > (DRY_VALUEA + 200));
    errors[ERR_SOILB_M].active = (avgB < (WET_VALUEB - 200) || avgB > (DRY_VALUEB + 200));

    // only update moisture if reading looks valid
    if (!errors[ERR_SOILA_M].active)
    {
      soilMoistureA = constrain(map(avgA, DRY_VALUEA, WET_VALUEA, 0, 100), 0, 100);
    }
    if (!errors[ERR_SOILB_M].active)
    {
      soilMoistureB = constrain(map(avgB, DRY_VALUEB, WET_VALUEB, 0, 100), 0, 100);
    }

    // RESET the counters for the next batch
    soilASum = 0;
    soilBSum = 0;
    soilSampleCount = 0;
    readingSoilMoisture = false; // done reading soil moisture for now
    Serial.printf("A raw: %d\n", avgA);
    Serial.printf("B raw: %d\n", avgB);
  }
}

const char *getStateMessage(SystemState state)
{
  for (int i = 0; i < sizeof(states) / sizeof(states[0]); i++)
  {
    if (states[i].state == state)
    {
      return states[i].message;
    }
  }

  return "UNKNOWN";
}

// returns the number of active errors
int countActiveErrors()
{
  int n = 0;
  for (int i = 0; i < ERR_COUNT; i++)
    if (errors[i].active)
      n++;
  return n;
}

// returns the first active error, or nullptr if there are no active errors
Error *getActiveError()
{
  for (int i = 0; i < ERR_COUNT; i++)
    if (errors[i].active)
      return &errors[i];
  return nullptr;
}

// returns true if any error that stops watering is active
bool hasStoppingError()
{
  for (int i = 0; i < ERR_COUNT; i++)
  {
    if (errors[i].active && errors[i].stopsWatering)
      return true;
  }
  return false;
}

// updates the yellow and blue LEDs as system indicators
void updateLEDs()
{ // yellow LED turns on if there is an active system error
  // blue LED turns on if watering is active
  // blue LED blinks if watering gets delayed

  if (getActiveError() != nullptr)
  {
    digitalWrite(YELLOW_LED_PIN, HIGH);
  }
  else
  {
    digitalWrite(YELLOW_LED_PIN, LOW);
  }
  if (currentState == STATE_ACTIVE || currentState == STATE_MANUALLY_STARTED)
  {
    digitalWrite(BLUE_LED_PIN, HIGH);
  }
  else
  {
    if (timeToWater && currentState != STATE_ACTIVE && currentState != STATE_MANUALLY_STARTED)
    {
      // If it is time to water but cannot water due to conditions or errors the blue led blinks
      int blinkInterval = 500; // Blink every 500 ms
      static unsigned long lastBlinkTime = 0;
      static bool blueLedState = false;
      if (millis() - lastBlinkTime >= blinkInterval)
      {
        blueLedState = !blueLedState;
        digitalWrite(BLUE_LED_PIN, blueLedState); // Toggle the blue LED using cached state
        lastBlinkTime = millis();
      }
    }
    else
    {
      digitalWrite(BLUE_LED_PIN, LOW);
    }
  }
}

// checks for midnight and resets daily flags
void checkMidnightReset(DateTime t)
{

  static int lastHour = -1;
  int currentHour = t.hour();

  if (currentHour != lastHour)
  {
    lastHour = currentHour;
    if (currentHour == 0)
    { // if it is has hit midnight
      // clear all the daily flags at midnight
      setState(STATE_IDLE);
      digitalWrite(RELAY_PIN, LOW); // make sure watering is off at midnight reset
      wateringProgress = 0;
      wateringElapsed = 0;
      totalElapsed = 0;
      timeToWater = false;
      wateredToday = false;
    }
  }
}

void timeToWaterCheck(DateTime t)
{
  if (wateredToday || currentState == STATE_MANUALLY_STARTED)
  {
    return; // if watering was manually started, ignore the schedule until the next manual action or midnight reset
  }
  if (t.hour() == (wateringStartHour - 1) && currentState == STATE_IDLE)
  {
    setState(STATE_READY); // update state to ready one hour before watering time if conditions are met so the user has a chance to see it on the display and intervene if they want to
    return;
  }
  // Check if it's the scheduled time to water (e.g., 13 = 1 PM) and set the flag
  if (t.hour() == wateringStartHour)
  {
    timeToWater = true;
  }
}

// draws text centered on the display at a given y coordinate
void drawCenteredText(const char *text, int y)
{
  int width = u8g2.getStrWidth(text);
  int x = (64 - width) / 2;
  u8g2.drawStr(x, y, text);
}

// turns on display and shows appropriate page
void updateDisplay(DateTime t)
{
  u8g2.clearBuffer();
  u8g2.setPowerSave(0); // Wake up OLED if it was asleep

  if (pageState < 5)
  { // because only the first few pages get the status bar
    // status bar
    u8g2.setFont(u8g2_font_Terminal_te);
    u8g2.drawBox(0, 0, 64, 20);
    u8g2.setDrawColor(0);
    u8g2.drawRFrame(1, 1, 62, 18, 2); //(x, y, width, height, radius)
    drawCenteredText(getStateMessage(currentState), 15);
    u8g2.setDrawColor(1);
    // time
    u8g2.drawHLine(0, 116, 64); // Divider line under the header

    // time and day
    u8g2.setFont(u8g2_font_siji_t_6x10);
    u8g2.setCursor(5, 128);
    // u8g2.printf("%02d:%02d", now.hour(), now.minute());
    u8g2.printf(
        "%s %02d:%02d",
        daysOfTheWeek[t.dayOfTheWeek()],
        t.hour(),
        t.minute());
  }
  switch (pageState)
  {
  case 1: // status page
  {
    u8g2.setFont(u8g2_font_streamline_ecology_t);
    u8g2.drawGlyph(5, 60, 61); // plant icon
    uint8_t progressHeight = map(wateringProgress, 0, 100, 0, 36);
    uint8_t progressY = 26 + (36 - progressHeight);
    u8g2.drawBox(36, progressY, 20, progressHeight);
    u8g2.drawFrame(34, 24, 24, 40);

    u8g2.setFont(u8g2_font_6x10_tf);

    int activeErrorCount = countActiveErrors();
    if (activeErrorCount > 0)
    {
      Error *err = getActiveError();

      u8g2.drawBox(0, 70, 64, 30);
      u8g2.setDrawColor(0);
      u8g2.drawRFrame(-1, 69, 66, 32, 4); //(x, y, width, height, radius)
      u8g2.drawRFrame(1, 71, 62, 28, 4);  //(x, y, width, height, radius)
      drawCenteredText("ERROR:", 83);
      drawCenteredText(err->message, 95);
      u8g2.setDrawColor(1);

      u8g2.setCursor(0, 110);
      u8g2.printf("errors:%d", activeErrorCount);
    }
    else
    {
      u8g2.drawStr(0, 110, "errors:0 :)");
    }
  }
  break;
  case 2: // Rainwater Tank page
    u8g2.setFont(u8g2_font_6x12_tf);
    drawCenteredText("RAIN TANK", 35);
    u8g2.drawHLine(0, 42, 64); // Divider line
    u8g2.setCursor(0, 60);
    if (errors[ERR_TANK_TEMP].active)
    {
      u8g2.printf("Temp:fail");
    }
    else
    {
      u8g2.printf("Temp:%.1f", barrelTemp);
      u8g2.drawUTF8(u8g2.getCursorX(), u8g2.getCursorY(), "°C");
    }
    if (waterLevelGood)
    {
      u8g2.drawStr(0, 75, "Tank:Full");
    }
    else
      u8g2.drawStr(0, 75, "Tank:Empty");
    u8g2.setFont(u8g2_font_open_iconic_weather_4x_t);
    u8g2.drawGlyph(32, 117, 67); // rain icon
    u8g2.drawHLine(0, 116, 64);  // Divider line under the header
    break;

  case 3: // Soil sensor page
    u8g2.setFont(u8g2_font_6x12_tf);
    drawCenteredText("MOISTURE", 45);
    u8g2.drawRFrame(3, 34, 58, 16, 2); //(x, y, width, height, radius)

    u8g2.setCursor(9, 62);
    if (!errors[ERR_SOILA_M].active) u8g2.printf("%d%%", soilMoistureA);
    else u8g2.printf("fail");

    u8g2.setCursor(41, 62);
    if (!errors[ERR_SOILB_M].active) u8g2.printf("%d%%", soilMoistureB);
    else u8g2.printf("fail");

    drawCenteredText("SOIL TEMP", 83);
    u8g2.drawRFrame(3, 72, 58, 16, 2); //(x, y, width, height, radius)

    u8g2.setCursor(2, 100);
    if (!errors[ERR_SOILA_TEMP].active)
    {
      u8g2.printf("%.1f", soilTempA);
      u8g2.drawUTF8(u8g2.getCursorX(), u8g2.getCursorY(), "°");
    }
    else u8g2.printf("fail");
    
    u8g2.setCursor(34, 100);
    if (!errors[ERR_SOILB_TEMP].active)
    {
      u8g2.printf("%.1f", soilTempB);
      u8g2.drawUTF8(u8g2.getCursorX(), u8g2.getCursorY(), "°");
    }
    else u8g2.printf("fail");

    break;
  case 4:
  {
    u8g2.setFont(u8g2_font_6x12_tf);
    drawCenteredText("TARGET", 30);
    drawCenteredText("MOISTURE", 40);

    u8g2.setFont(u8g2_font_ncenB14_tr);
    char buf[10];
    int val = settingMoistureTarget ? newSoilMoistureTarget : soilMoistureTarget;
    sprintf(buf, "%d%%", val);
    drawCenteredText(buf, 55);

    u8g2.setFont(u8g2_font_5x7_tf);
    drawCenteredText("press blue", 65);
    drawCenteredText("to change", 80);
    drawCenteredText("yellow", 95);
    drawCenteredText("to confirm", 110);
    
  }
  break;
  case 5: // target moisture change confirmed
  {
    u8g2.setFont(u8g2_font_6x12_tf);
    drawCenteredText("MOISTURE SET", 30);

    u8g2.setFont(u8g2_font_ncenB14_tr);
    char buf[10];
    sprintf(buf, "%d%%", soilMoistureTarget);
    drawCenteredText(buf, 60);

    u8g2.setFont(u8g2_font_5x7_tf);
    drawCenteredText("Saved", 85);
  }
  break;

  case 6: // ask for manual stop

    u8g2.drawBox(0, 15, 64, 25); // Box in the middle
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_ncenB12_tr);
    drawCenteredText(" STOP?", 35);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawRFrame(0, 55, 64, 30, 2); //(x, y, width, height, radius)
    drawCenteredText("Press blue", 65);
    drawCenteredText("to STOP", 80);

    u8g2.setFont(u8g2_font_open_iconic_check_4x_t);
    u8g2.drawGlyph(16, 128, 66); // stop icon

    break;

  case 7: // manually stopped
    u8g2.drawBox(0, 15, 64, 38);
    u8g2.setDrawColor(0);
    u8g2.drawRFrame(1, 16, 62, 36, 2); //(x, y, width, height, radius)
    u8g2.setFont(u8g2_font_Terminal_tr);
    drawCenteredText("manually", 30);
    u8g2.setFont(u8g2_font_Terminal_te);
    drawCenteredText("STOPPED", 45);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawRFrame(0, 57, 64, 50, 2); //(x, y, width, height, radius)
    drawCenteredText("auto start", 70);
    drawCenteredText("disabled", 85);
    drawCenteredText("for today", 100);

    u8g2.setFont(u8g2_font_open_iconic_embedded_2x_t);
    u8g2.drawGlyph(24, 128, 71); // ! icon

    break;

  case 8: // ask for manual start
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB12_tr);
    // u8g2.setFont(u8g2_font_Pixellari_tf );

    u8g2.drawBox(0, 15, 64, 25); // Box in the middle
    u8g2.setDrawColor(0);
    drawCenteredText("START", 35);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawRFrame(0, 55, 64, 30, 2); //(x, y, width, height, radius)
    drawCenteredText("Press blue", 65);
    drawCenteredText("to START", 80);

    u8g2.setFont(u8g2_font_open_iconic_text_4x_t);
    u8g2.drawGlyph(16, 128, 89); // question mark icon

    break;

  case 9: // manually started
    u8g2.drawBox(0, 15, 64, 38);
    u8g2.setDrawColor(0);
    u8g2.drawRFrame(1, 16, 62, 36, 2); //(x, y, width, height, radius)
    u8g2.setFont(u8g2_font_Terminal_tr);
    drawCenteredText("manually", 30);
    u8g2.setFont(u8g2_font_Terminal_te);
    drawCenteredText("STARTED", 45);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawRFrame(0, 57, 64, 50, 2); //(x, y, width, height, radius)

    drawCenteredText("watering", 70);
    drawCenteredText("stops", 85);
    drawCenteredText("after 1h", 100);

    u8g2.setFont(u8g2_font_open_iconic_weather_2x_t);
    u8g2.drawGlyph(24, 128, 67); // rain icon

    break;

  case 10: // page for failed manual start

    // Top banner
    u8g2.drawBox(0, 15, 64, 38);
    u8g2.setDrawColor(0);
    u8g2.drawRFrame(1, 16, 62, 36, 2);

    u8g2.setFont(u8g2_font_Terminal_tr);
    drawCenteredText("START", 30);
    u8g2.setFont(u8g2_font_Terminal_te);
    drawCenteredText("FAILED", 45);

    u8g2.setDrawColor(1);

    // Info box
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawRFrame(0, 57, 64, 50, 2);

    drawCenteredText("manual", 70);
    drawCenteredText("start", 85);
    drawCenteredText("BLOCKED", 100);

    // Warning icon
    u8g2.setFont(u8g2_font_open_iconic_embedded_2x_t);
    u8g2.drawGlyph(24, 128, 71); // ! icon

    break;
  }
  u8g2.sendBuffer();
}

// checks for inactivity and turns off display after timeout
void displayTimeout()
{
  if (millis() - lastActivity > timeout) // turn off display after 60 seconds of inactivity
  {
    pageState = 0;        // Set state to OFF
    u8g2.setPowerSave(1); // Put OLED into low-power sleep
  }
}
//
