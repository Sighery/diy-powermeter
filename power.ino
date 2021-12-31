/**
   Main
   ====

  Copy binary to output (dist) folder:
  copy /Y ..\output\power.ino.zip dist
*/

#include "Adafruit_MPU6050.h"
#include "Adafruit_Sensor.h"
#include "board.h"
#include <Wire.h>
#include <bluefruit.h>
#include <SPI.h>
#include "HX711_ADC.h"


#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <Adafruit_TinyUSB.h> // for Serial

//#define DEBUG

#define DEV_NAME "Cycle Power Meter"
#define NRF52840_XXAA
#define gn 9.80665 // gravity constant

// Virtufit Etappe I: crank length, in meters
// (to be added to the calibration procedure!)
#define CRANK_RADIUS 0.1725

// Milliseconds to wait before go to sleep: 900000 = 15 minutes 
#define MILLIS_TO_SLEEP 900000 

// The pause for the loop, and based on testing the actual
// calls overhead take about 20ms themselves E.g. at 50ms delay,
// that means a 50ms delay, plus 20ms to poll. So 70ms per loop,
// will get ~14 samples/second.
#define LOOP_DELAY 70

// HX711 on-board hardware switch default is 10 Hz (alternative: 80 Hz)
#define HX711_RATE 10 

// With the default HX711_RATE of 10 Hz, we want at least 15 samples for a valid measurement
#define MIN_UPDATE_TIME 1500

// If the number of radians per seconds is less than this, we assume the user stopped pedaling
#define STAND_STILL_RPS (0.25 * PI)

// Pin-outs
#define LED_PIN LED_BUILTIN
#define SD_CS_PIN PIN_A3
#define GYRO_INT_PIN A4

// Interrupt related variables (must be volatile)
volatile long timeFirstSleepCheck=0;
volatile long Sleepy=0;
volatile int last_connection_count=0;
volatile long connectedStart=0;
volatile boolean newLoadDataReady=0;

// Bluetooth
#define PWR_MEAS_CHAR_LEN 8 // Bluetooth package length
bool show_values=false; // print raw values
int16_t test_power=0; // for testing
uint16_t test_totalCrankRev=0; // for testing
uint16_t test_totalCrankRev_inc=0; // for testing
uint8_t connection_count = 0;

#define LOAD_OFFSET_DEFAULT 8745984
#define LOAD_MULTIPLIER_DEFAULT 810.1
#define CALIBRATIONS_FILENAME    "/calibrations.txt"

typedef struct settings_struct {
  unsigned char calibrated; 

  int gyro_offset = 0;
  long load_offset = 0;
  float load_multiplier = 0;
} nvram_settings_struct;
nvram_settings_struct nvram_settings;

//HX711 pins:
#define HX711_dout A0 //mcu > HX711 dout pin (was: 4)
#define HX711_sck A1 //mcu > HX711 sck pin (was: 5)

//HX711 constructor:
HX711_ADC LoadCell(HX711_dout, HX711_sck);

//HX711 EEPROM calibration settings saving/loading
const int calVal_eepromAdress = 0;
unsigned long t = 0;

//MPU6050 constructor:
Adafruit_MPU6050 mpu;

//Internal filesystem
using namespace Adafruit_LittleFS_Namespace;
File file(InternalFS);


void setup() {
  Wire.begin();

  Serial.begin(115200);
  int cnt=0;
  while ( !Serial && (cnt++ < 300)) delay(10);   // for nrf52840 with native usb

  timeFirstSleepCheck=0;
  Sleepy = 0;

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // Setup, calibrate our other components
  gyroSetup();
  loadSetup();
  bleSetup();
  setupBattery();

  Serial.printf("Setup completed.\n\n");
  Serial.printf("Enter 'h' for help.\n\n");
  delay(200);
}

void loop() {
  // Vars for polling footspeed
  static float avgRad = 0.f;
  static float avgRad_prev = PI; // ensure a non-zero value
  
  static float rad=0.f;
  // Cadence is calculated by increasing total revolutions.
  // TODO it's possible this rolls over, about 12 hours at 90RPM for 16 bit unsigned.
  static uint16_t totalCrankRevs = 0;
  // Vars for force
  static float avgForce = 0.f;

  // We only publish every once-in-a-while.
  static long lastUpdate = millis();
  // Other things (like battery) might be on a longer update schedule for power.
  static long lastInfrequentUpdate = millis();
  // To find the average values to use, count the num of samples
  // between updates.
  static int16_t numPolls = 0;
  bool pedaling = false;

  float Zroll, Ztilt; 


  // Degrees per second
  rad = getNormalAvgVelocity();
  avgRad += rad;
  numPolls += 1;

  
//  if (Bluefruit.connected()) {
  if (connection_count > 0) {
    // We have a central connected
    long timeNow = millis();
    long timeSinceLastUpdate = timeNow - lastUpdate;
   
    // Print help text after an arbitrary wait time (to allow the user to press UART in the App)
    if(connection_count != last_connection_count) {
      if (connectedStart == 0) connectedStart = millis();
      if((millis() - connectedStart) > (1000*6))
      {
        last_connection_count = connection_count;
        connectedStart = 0;

        printHelp();
      }
    }

    // Check if we're ready for a new update
    //  - Ensure minimum 2 seconds of measurements (~ 20 samples from load-sensor)
    //  - Ensure near horizontal position
    getZtilt(&Zroll, &Ztilt);
    if ((timeSinceLastUpdate >= 2000) && (Ztilt > -5) && (Ztilt < 5) && (Zroll > -95) && (Zroll < -85)) {  
      if (avgRad_prev > STAND_STILL_RPS) {
        pedaling = true;
      }
      else {
        pedaling = false;
      }

      // Get and store the last-measured cadence to determine the next update time
      avgRad = avgRad / numPolls;
      avgRad_prev = avgRad;

      // Get the moving average force from the load cell (library)
      avgForce = getAvgForce();

      // Get the circular velocity of the rider's foot in m/s
      float mps = CRANK_RADIUS * avgRad; 

      // Multiply it all by 2, because we only have the sensor on 1/2 the cranks
      int16_t power = 2 * mps * avgForce;

      if (pedaling) {
        totalCrankRevs += 1;
      }
      
      if((test_power>0) || (test_totalCrankRev_inc>0))
      {
        test_totalCrankRev += test_totalCrankRev_inc;
        blePublishPower(test_power, test_totalCrankRev, timeNow);
        printfLog("Fake: Force=%d  Cad=%d\n", test_power, test_totalCrankRev);
      }
      else
      {
        if (show_values) {
            printfLog("%.1fN * %.2fm/s = %dW (Z=%0.1f, R=%0.1f, p=%d)\n", avgForce, mps, power, Ztilt, Zroll, numPolls);
        }
        blePublishPower(power, totalCrankRevs, timeNow);
      }

      // Reset the latest update to now.
      lastUpdate = timeNow;
      // Let the averages from this polling period just carry over.
      numPolls = 1;

      // And check the battery, don't need to do it nearly this often though.
      // 1000 ms / sec * 60 sec / min * 5 = 5 minutes
      if ((timeNow - lastInfrequentUpdate) > (1000 * 60 * 5)) {
        blePublishBatt();
        lastInfrequentUpdate = timeNow;
      }
    }
  }
  else
  {
    last_connection_count = 0;
  }


  char buf[64]={'\0'};
  GetUserInput(buf);
  if (buf[0] == 'c') calibrateLoadCell();
  if (buf[0] == 's') enterSleepMode();
  if (buf[0] == 'h') printHelp(); 
  if (buf[0] == 'f') {
    if (test_power > 0) {
      test_power = 0;
      test_totalCrankRev = 0;
      test_totalCrankRev_inc = 0;
    }
    else {
      testBT(); //test bluetooth
    }
  }
  if (buf[0] == 'm') {
    if (show_values) {
      show_values = false;
    }
    else {
      show_values = true;
    }
  }
  
  // Pass-through USB/Bluethooth (BLE) data
  //bleuart_data_transfer();

  // Should we go to sleep?
  gyroCheckSleepy(pedaling);

  delay(LOOP_DELAY);

// Request CPU to enter low-power mode until an event/interrupt occurs
//  waitForEvent();
}

/**
   Figure out how long to sleep in the loops. We'd like to tie the update interval
   to the cadence, but if they stop pedaling need some minimum.

   Return update interval, in milliseconds.
*/
float updateTime(float rad, bool *pedaling) {
  // So knowing the dps, how long for 360 degrees?
  float my_delay = min(MIN_UPDATE_TIME, 1000.f * (2*PI) / rad);
  if (rad > STAND_STILL_RPS) {
    // Let the caller know we didn't just hit the max pause,
    // the cranks are spinning.
    *pedaling = true;
  }
  else {
    // We're not spinning
    *pedaling = false;
  }

  return (my_delay);
}

void printHelp() {
  printfLog("=================\n");
  printfLog("Power Cycle Meter\n");
  printfLog("=================\n\n");

  if (nvram_settings.load_offset == LOAD_OFFSET_DEFAULT) {
    printfLog("\nLoad-cell defaults loaded:\n");
  }
  printfLog("Load offset calibration: %d\n",nvram_settings.load_offset);
  printfLog("Load multiplier calibration: %.1f\n\n",nvram_settings.load_multiplier); 
  blePublishBatt(); // Publish battery level to newly connected devices

  printfLog("Commands:\n");
  printfLog(" h : show this Help text\n");
  printfLog(" m : Monitor power & cadence\n");
  printfLog(" f : Fake power & cadence\n");
  printfLog(" c : Calibrate load sensor\n");
  printfLog(" s : enter Sleep mode\n");
  printfLog("\n");
}