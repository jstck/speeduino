/*
Speeduino - Simple engine management for the Arduino Mega 2560 platform
Copyright (C) Josh Stewart

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,la
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

//**************************************************************************************************
// Config section
#define engineSquirtsPerCycle 2 //Would be 1 for a 2 stroke
//**************************************************************************************************

//https://developer.mbed.org/handbook/C-Data-Types
#include <stdint.h>
//************************************************
#include "globals.h"
#include "utils.h"
#include "table.h"
#include "scheduler.h"
#include "comms.h"
#include "cancomms.h"
#include "maths.h"
#include "corrections.h"
#include "timers.h"
//#include "display.h"
#include "decoders.h"
#include "idle.h"
#include "auxiliaries.h"
#include "sensors.h"
#include "src/PID_v1/PID_v1.h"
//#include "src/DigitalWriteFast/digitalWriteFast.h"
#include "errors.h"
#include "storage.h"
#include "scheduledIO.h"
#include <EEPROM.h>
#if defined (CORE_TEENSY)
#include <FlexCAN.h>
#endif

struct config1 configPage1;
struct config2 configPage2;
struct config3 configPage3;
struct config4 configPage4;
struct config10 configPage10;
struct config11 configPage11;

int req_fuel_uS, inj_opentime_uS;

bool ignitionOn = false; //The current state of the ignition system
bool fuelOn = false; //The current state of the ignition system
bool fuelPumpOn = false; //The current status of the fuel pump

void (*trigger)(); //Pointer for the trigger function (Gets pointed to the relevant decoder)
void (*triggerSecondary)(); //Pointer for the secondary trigger function (Gets pointed to the relevant decoder)
uint16_t (*getRPM)(); //Pointer to the getRPM function (Gets pointed to the relevant decoder)
int (*getCrankAngle)(int); //Pointer to the getCrank Angle function (Gets pointed to the relevant decoder)
void (*triggerSetEndTeeth)(); //Pointer to the triggerSetEndTeeth function of each decoder

byte cltCalibrationTable[CALIBRATION_TABLE_SIZE];
byte iatCalibrationTable[CALIBRATION_TABLE_SIZE];
byte o2CalibrationTable[CALIBRATION_TABLE_SIZE];

//These variables are used for tracking the number of running sensors values that appear to be errors. Once a threshold is reached, the sensor reading will go to default value and assume the sensor is faulty
byte mapErrorCount = 0;
byte iatErrorCount = 0;
byte cltErrorCount = 0;

unsigned long counter;
unsigned long currentLoopTime; //The time the current loop started (uS)
unsigned long previousLoopTime; //The time the previous loop started (uS)

int CRANK_ANGLE_MAX = 720;
int CRANK_ANGLE_MAX_IGN = 360;
int CRANK_ANGLE_MAX_INJ = 360; // The number of crank degrees that the system track over. 360 for wasted / timed batch and 720 for sequential

static byte coilHIGH = HIGH;
static byte coilLOW = LOW;
static byte fanHIGH = HIGH;             // Used to invert the cooling fan output
static byte fanLOW = LOW;               // Used to invert the cooling fan output

static byte vicsHIGH = HIGH;            // Used to invert the VICS output
static byte vicsLOW = LOW;              // Used to invert the VICS output

volatile int mainLoopCount;
byte deltaToothCount = 0; //The last tooth that was used with the deltaV calc
int rpmDelta;
byte ignitionCount;
uint16_t fixedCrankingOverride = 0;
int16_t lastAdvance; //Stores the previous advance figure to track changes.
bool clutchTrigger;
bool previousClutchTrigger;

unsigned long secCounter; //The next time to incremen 'runSecs' counter.
int channel1IgnDegrees; //The number of crank degrees until cylinder 1 is at TDC (This is obviously 0 for virtually ALL engines, but there's some weird ones)
int channel2IgnDegrees; //The number of crank degrees until cylinder 2 (and 5/6/7/8) is at TDC
int channel3IgnDegrees; //The number of crank degrees until cylinder 3 (and 5/6/7/8) is at TDC
int channel4IgnDegrees; //The number of crank degrees until cylinder 4 (and 5/6/7/8) is at TDC
int channel5IgnDegrees; //The number of crank degrees until cylinder 5 is at TDC
int channel1InjDegrees; //The number of crank degrees until cylinder 1 is at TDC (This is obviously 0 for virtually ALL engines, but there's some weird ones)
int channel2InjDegrees; //The number of crank degrees until cylinder 2 (and 5/6/7/8) is at TDC
int channel3InjDegrees; //The number of crank degrees until cylinder 3 (and 5/6/7/8) is at TDC
int channel4InjDegrees; //The number of crank degrees until cylinder 4 (and 5/6/7/8) is at TDC
int channel5InjDegrees; //The number of crank degrees until cylinder 5 is at TDC

//These are the functions the get called to begin and end the ignition coil charging. They are required for the various spark output modes
void (*ign1StartFunction)();
void (*ign1EndFunction)();
void (*ign2StartFunction)();
void (*ign2EndFunction)();
void (*ign3StartFunction)();
void (*ign3EndFunction)();
void (*ign4StartFunction)();
void (*ign4EndFunction)();
void (*ign5StartFunction)();
void (*ign5EndFunction)();

int timePerDegree;
byte degreesPerLoop; //The number of crank degrees that pass for each mainloop of the program
volatile bool fpPrimed = false; //Tracks whether or not the fuel pump priming has been completed yet
bool initialisationComplete = false; //Tracks whether the setup() functino has run completely

void setup()
{

  //Setup the dummy fuel and ignition tables
  //dummyFuelTable(&fuelTable);
  //dummyIgnitionTable(&ignitionTable);
  table3D_setSize(&fuelTable, 16);
  table3D_setSize(&ignitionTable, 16);
  table3D_setSize(&afrTable, 16);
  table3D_setSize(&boostTable, 8);
  table3D_setSize(&vvtTable, 8);
  table3D_setSize(&trim1Table, 6);
  table3D_setSize(&trim2Table, 6);
  table3D_setSize(&trim3Table, 6);
  table3D_setSize(&trim4Table, 6);

  loadConfig();
  doUpdates(); //Check if any data items need updating (Occurs ith firmware updates)

  Serial.begin(115200);
#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__) //ATmega2561 does not have Serial3
  if (configPage10.enable_canbus == 1) { Serial3.begin(115200); }
#elif defined(CORE_STM32)
  if (configPage10.enable_canbus == 1) { Serial2.begin(115200); }
  else if (configPage10.enable_canbus == 2)
  {
    //enable local can interface
  }
#elif defined(CORE_TEENSY)
  if (configPage10.enable_canbus == 1) { Serial2.begin(115200); }
  else if (configPage10.enable_canbus == 2)
  {
    //Teensy onboard CAN not used currently
    //enable local can interface
    //setup can interface to 250k
    //FlexCAN CANbus0(2500000, 0);
    //static CAN_message_t txmsg,rxmsg;
    //CANbus0.begin();
  }
#endif

  //Repoint the 2D table structs to the config pages that were just loaded
  taeTable.valueSize = SIZE_BYTE; //Set this table to use byte values
  taeTable.xSize = 4;
  taeTable.values = configPage2.taeValues;
  taeTable.axisX = configPage2.taeBins;
  WUETable.valueSize = SIZE_BYTE; //Set this table to use byte values
  WUETable.xSize = 10;
  WUETable.values = configPage1.wueValues;
  WUETable.axisX = configPage2.wueBins;
  crankingEnrichTable.valueSize = SIZE_BYTE;
  crankingEnrichTable.xSize = 4;
  crankingEnrichTable.values = configPage11.crankingEnrichValues;
  crankingEnrichTable.axisX = configPage11.crankingEnrichBins;

  dwellVCorrectionTable.valueSize = SIZE_BYTE;
  dwellVCorrectionTable.xSize = 6;
  dwellVCorrectionTable.values = configPage2.dwellCorrectionValues;
  dwellVCorrectionTable.axisX = configPage3.voltageCorrectionBins;
  injectorVCorrectionTable.valueSize = SIZE_BYTE;
  injectorVCorrectionTable.xSize = 6;
  injectorVCorrectionTable.values = configPage3.injVoltageCorrectionValues;
  injectorVCorrectionTable.axisX = configPage3.voltageCorrectionBins;
  IATDensityCorrectionTable.valueSize = SIZE_BYTE;
  IATDensityCorrectionTable.xSize = 9;
  IATDensityCorrectionTable.values = configPage3.airDenRates;
  IATDensityCorrectionTable.axisX = configPage3.airDenBins;
  IATRetardTable.valueSize = SIZE_BYTE;
  IATRetardTable.xSize = 6;
  IATRetardTable.values = configPage2.iatRetValues;
  IATRetardTable.axisX = configPage2.iatRetBins;

  //Setup the calibration tables
  loadCalibration();

  //Set the pin mappings
  if(configPage1.pinMapping > BOARD_NR_GPIO_PINS)
  {
    //First time running on this board
    setPinMapping(3); //Force board to v0.4
    configPage1.flexEnabled = false; //Have to disable flex. If this isn't done and the wrong flex pin is interrupt attached below, system can hang.
  }
  else { setPinMapping(configPage1.pinMapping); }

  //Need to check early on whether the coil charging is inverted. If this is not set straight away it can cause an unwanted spark at bootup
  if(configPage2.IgInv == 1) { coilHIGH = LOW, coilLOW = HIGH; }
  else { coilHIGH = HIGH, coilLOW = LOW; }
  endCoil1Charge();
  endCoil2Charge();
  endCoil3Charge();
  endCoil4Charge();
  endCoil5Charge();

  //Similar for injectors, make sure they're turned off
  closeInjector1();
  closeInjector2();
  closeInjector3();
  closeInjector4();
  closeInjector5();

  //Set the tacho output default state
  digitalWrite(pinTachOut, HIGH);

  //Perform all initialisations
  initialiseSchedulers();
  initialiseTimers();
  //initialiseDisplay();
  initialiseIdle();
  initialiseFan();
  initialiseAuxPWM();
  initialiseCorrections();
  initialiseADC();

  //Lookup the current MAP reading for barometric pressure
  instanteneousMAPReading();
  //barometric reading can be taken from either an external sensor if enabled, or simply by using the initial MAP value
  if ( configPage3.useExtBaro != 0 )
  {
    readBaro();
    EEPROM.update(EEPROM_LAST_BARO, currentStatus.baro);
  }
  else
  {
    /*
     * The highest sea-level pressure on Earth occurs in Siberia, where the Siberian High often attains a sea-level pressure above 105 kPa;
     * with record highs close to 108.5 kPa.
     * The lowest measurable sea-level pressure is found at the centers of tropical cyclones and tornadoes, with a record low of 87 kPa;
     */
    if ((currentStatus.MAP >= BARO_MIN) && (currentStatus.MAP <= BARO_MAX)) //Check if engine isn't running
    {
      currentStatus.baro = currentStatus.MAP;
      EEPROM.update(EEPROM_LAST_BARO, currentStatus.baro);
    }
    else
    {
      //Attempt to use the last known good baro reading from EEPROM
      if ((EEPROM.read(EEPROM_LAST_BARO) >= BARO_MIN) && (EEPROM.read(EEPROM_LAST_BARO) <= BARO_MAX)) //Make sure it's not invalid (Possible on first run etc)
      { currentStatus.baro = EEPROM.read(EEPROM_LAST_BARO); } //last baro correction
      else { currentStatus.baro = 100; } //Final fall back position.
    }
  }

  //Check whether the flex sensor is enabled and if so, attach an interupt for it
  if(configPage1.flexEnabled)
  {
    attachInterrupt(digitalPinToInterrupt(pinFlex), flexPulse, RISING);
    currentStatus.ethanolPct = 0;
  }

  //Once the configs have been loaded, a number of one time calculations can be completed
  req_fuel_uS = configPage1.reqFuel * 100; //Convert to uS and an int. This is the only variable to be used in calculations
  inj_opentime_uS = configPage1.injOpen * 100; //Injector open time. Comes through as ms*10 (Eg 15.5ms = 155).

  //Begin the main crank trigger interrupt pin setup
  //The interrupt numbering is a bit odd - See here for reference: http://arduino.cc/en/Reference/AttachInterrupt
  //These assignments are based on the Arduino Mega AND VARY BETWEEN BOARDS. Please confirm the board you are using and update acordingly.
  byte triggerInterrupt = 0; // By default, use the first interrupt
  byte triggerInterrupt2 = 1;
  currentStatus.RPM = 0;
  currentStatus.hasSync = false;
  currentStatus.runSecs = 0;
  currentStatus.secl = 0;
  currentStatus.startRevolutions = 0;
  currentStatus.flatShiftingHard = false;
  currentStatus.launchingHard = false;
  currentStatus.crankRPM = ((unsigned int)configPage2.crankRPM * 100); //Crank RPM limit (Saves us calculating this over and over again. It's updated once per second in timers.ino)
  triggerFilterTime = 0; //Trigger filter time is the shortest possible time (in uS) that there can be between crank teeth (ie at max RPM). Any pulses that occur faster than this time will be disgarded as noise. This is simply a default value, the actual values are set in the setup() functinos of each decoder

  #if defined(CORE_AVR)
    switch (pinTrigger) {
      //Arduino Mega 2560 mapping
      case 2:
        triggerInterrupt = 0; break;
      case 3:
        triggerInterrupt = 1; break;
      case 18:
        triggerInterrupt = 5; break;
      case 19:
        triggerInterrupt = 4; break;
      case 20:
        triggerInterrupt = 3; break;
      case 21:
        triggerInterrupt = 2; break;
      default:
        triggerInterrupt = 0; break; //This should NEVER happen
    }
  #else
    triggerInterrupt = pinTrigger;
  #endif

  #if defined(CORE_AVR)
    switch (pinTrigger2) {
      //Arduino Mega 2560 mapping
      case 2:
        triggerInterrupt2 = 0; break;
      case 3:
        triggerInterrupt2 = 1; break;
      case 18:
        triggerInterrupt2 = 5; break;
      case 19:
        triggerInterrupt2 = 4; break;
      case 20:
        triggerInterrupt2 = 3; break;
      case 21:
        triggerInterrupt2 = 2; break;
      default:
        triggerInterrupt2 = 0; break; //This should NEVER happen
    }
  #else
    triggerInterrupt2 = pinTrigger2;
  #endif
  pinMode(pinTrigger, INPUT);
  pinMode(pinTrigger2, INPUT);
  pinMode(pinTrigger3, INPUT);
  //digitalWrite(pinTrigger, HIGH);

  //Set the trigger function based on the decoder in the config
  switch (configPage2.TrigPattern)
  {
    case 0:
      //Missing tooth decoder
      triggerSetup_missingTooth();
      trigger = triggerPri_missingTooth;
      triggerSecondary = triggerSec_missingTooth;
      getRPM = getRPM_missingTooth;
      getCrankAngle = getCrankAngle_missingTooth;
      triggerSetEndTeeth = triggerSetEndTeeth_missingTooth;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
      if(configPage2.TrigEdgeSec == 0) { attachInterrupt(triggerInterrupt2, triggerSec_missingTooth, RISING); }
      else { attachInterrupt(triggerInterrupt2, triggerSec_missingTooth, FALLING); }
      break;

    case 1:
      // Basic distributor
      triggerSetup_BasicDistributor();
      trigger = triggerPri_BasicDistributor;
      getRPM = getRPM_BasicDistributor;
      getCrankAngle = getCrankAngle_BasicDistributor;
      triggerSetEndTeeth = triggerSetEndTeeth_BasicDistributor;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
      break;

    case 2:
      triggerSetup_DualWheel();
      trigger = triggerPri_DualWheel;
      getRPM = getRPM_DualWheel;
      getCrankAngle = getCrankAngle_DualWheel;
      triggerSetEndTeeth = triggerSetEndTeeth_DualWheel;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
      if(configPage2.TrigEdgeSec == 0) { attachInterrupt(triggerInterrupt2, triggerSec_DualWheel, RISING); }
      else { attachInterrupt(triggerInterrupt2, triggerSec_DualWheel, FALLING); }
      break;

    case 3:
      triggerSetup_GM7X();
      trigger = triggerPri_GM7X;
      getRPM = getRPM_GM7X;
      getCrankAngle = getCrankAngle_GM7X;
      triggerSetEndTeeth = triggerSetEndTeeth_GM7X;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
      break;

    case 4:
      triggerSetup_4G63();
      trigger = triggerPri_4G63;
      getRPM = getRPM_4G63;
      getCrankAngle = getCrankAngle_4G63;
      triggerSetEndTeeth = triggerSetEndTeeth_4G63;

      //These may both need to change, not sure
      if(configPage2.TrigEdge == 0)
      {
        attachInterrupt(triggerInterrupt, trigger, CHANGE);  // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
        attachInterrupt(triggerInterrupt2, triggerSec_4G63, FALLING); //changed
      }
      else
      {
        attachInterrupt(triggerInterrupt, trigger, CHANGE); // Primary trigger connects to
        attachInterrupt(triggerInterrupt2, triggerSec_4G63, FALLING);
      }
      break;

    case 5:
      triggerSetup_24X();
      trigger = triggerPri_24X;
      getRPM = getRPM_24X;
      getCrankAngle = getCrankAngle_24X;
      triggerSetEndTeeth = triggerSetEndTeeth_24X;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); } // Primary trigger connects to
      attachInterrupt(triggerInterrupt2, triggerSec_24X, CHANGE);
      break;

    case 6:
      triggerSetup_Jeep2000();
      trigger = triggerPri_Jeep2000;
      getRPM = getRPM_Jeep2000;
      getCrankAngle = getCrankAngle_Jeep2000;
      triggerSetEndTeeth = triggerSetEndTeeth_Jeep2000;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); } // Primary trigger connects to
      attachInterrupt(triggerInterrupt2, triggerSec_Jeep2000, CHANGE);
      break;

    case 7:
      triggerSetup_Audi135();
      trigger = triggerPri_Audi135;
      getRPM = getRPM_Audi135;
      getCrankAngle = getCrankAngle_Audi135;
      triggerSetEndTeeth = triggerSetEndTeeth_Audi135;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
      attachInterrupt(triggerInterrupt2, triggerSec_Audi135, RISING);
      break;

    case 8:
      triggerSetup_HondaD17();
      trigger = triggerPri_HondaD17;
      getRPM = getRPM_HondaD17;
      getCrankAngle = getCrankAngle_HondaD17;
      triggerSetEndTeeth = triggerSetEndTeeth_HondaD17;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); } // Primary trigger connects to
      attachInterrupt(triggerInterrupt2, triggerSec_HondaD17, CHANGE);
      break;

    case 9:
      triggerSetup_Miata9905();
      trigger = triggerPri_Miata9905;
      getRPM = getRPM_Miata9905;
      getCrankAngle = getCrankAngle_Miata9905;
      triggerSetEndTeeth = triggerSetEndTeeth_Miata9905;

      //These may both need to change, not sure
      if(configPage2.TrigEdge == 0)
      {
        attachInterrupt(triggerInterrupt, trigger, RISING);  // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
        attachInterrupt(triggerInterrupt2, triggerSec_Miata9905, FALLING); //changed
      }
      else
      {
        attachInterrupt(triggerInterrupt, trigger, FALLING); // Primary trigger connects to
        attachInterrupt(triggerInterrupt2, triggerSec_Miata9905, RISING);
      }
      break;

    case 10:
      triggerSetup_MazdaAU();
      trigger = triggerPri_MazdaAU;
      getRPM = getRPM_MazdaAU;
      getCrankAngle = getCrankAngle_MazdaAU;
      triggerSetEndTeeth = triggerSetEndTeeth_MazdaAU;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); } // Primary trigger connects to
      attachInterrupt(triggerInterrupt2, triggerSec_MazdaAU, FALLING);
      break;

    case 11:
      triggerSetup_non360();
      trigger = triggerPri_DualWheel; //Is identical to the dual wheel decoder, so that is used. Same goes for the secondary below
      getRPM = getRPM_non360;
      getCrankAngle = getCrankAngle_non360;
      triggerSetEndTeeth = triggerSetEndTeeth_Non360;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
      attachInterrupt(triggerInterrupt2, triggerSec_DualWheel, FALLING); //Note the use of the Dual Wheel trigger function here. No point in having the same code in twice.
      break;

    case 12:
        triggerSetup_Nissan360();
        trigger = triggerPri_Nissan360;
        getRPM = getRPM_Nissan360;
        getCrankAngle = getCrankAngle_Nissan360;
        triggerSetEndTeeth = triggerSetEndTeeth_Nissan360;

        if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
        else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
        attachInterrupt(triggerInterrupt2, triggerSec_Nissan360, CHANGE);
        break;

    case 13:
            triggerSetup_Subaru67();
            trigger = triggerPri_Subaru67;
            getRPM = getRPM_Subaru67;
            getCrankAngle = getCrankAngle_Subaru67;
            triggerSetEndTeeth = triggerSetEndTeeth_Subaru67;

            if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
            else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
            attachInterrupt(triggerInterrupt2, triggerSec_Subaru67, FALLING);
            break;

    case 14:
            triggerSetup_Daihatsu();
            trigger = triggerPri_Daihatsu;
            getRPM = getRPM_Daihatsu;
            getCrankAngle = getCrankAngle_Daihatsu;
            triggerSetEndTeeth = triggerSetEndTeeth_Daihatsu;

            if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
            else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
            //No secondary input required for this pattern
            break;

    default:
      trigger = triggerPri_missingTooth;
      getRPM = getRPM_missingTooth;
      getCrankAngle = getCrankAngle_missingTooth;

      if(configPage2.TrigEdge == 0) { attachInterrupt(triggerInterrupt, trigger, RISING); } // Attach the crank trigger wheel interrupt (Hall sensor drags to ground when triggering)
      else { attachInterrupt(triggerInterrupt, trigger, FALLING); }
      break;
  }

  //End crank triger interrupt attachment
  req_fuel_uS = req_fuel_uS / engineSquirtsPerCycle; //The req_fuel calculation above gives the total required fuel (At VE 100%) in the full cycle. If we're doing more than 1 squirt per cycle then we need to split the amount accordingly. (Note that in a non-sequential 4-stroke setup you cannot have less than 2 squirts as you cannot determine the stroke to make the single squirt on)

  //Initial values for loop times
  previousLoopTime = 0;
  currentLoopTime = micros();

  mainLoopCount = 0;
  ignitionCount = 0;

  //Calculate the number of degrees between cylinders
  switch (configPage1.nCylinders) {
    case 1:
      channel1IgnDegrees = 0;
      channel1InjDegrees = 0;

      channel1InjEnabled = true;
      break;

    case 2:
      channel1IgnDegrees = 0;

      if (configPage1.engineType == EVEN_FIRE )
      {
        channel2IgnDegrees = 180;
      }
      else { channel2IgnDegrees = configPage1.oddfire2; }

      //For alternating injection, the squirt occurs at different times for each channel
      if(configPage1.injLayout == INJ_SEMISEQUENTIAL)
      {
        channel1InjDegrees = 0;
        channel2InjDegrees = channel2IgnDegrees; //Set to the same as the ignition degrees (Means there's no need for another if to check for oddfire)
      }
      if (!configPage1.injTiming) { channel1InjDegrees = channel2InjDegrees = 0; } //For simultaneous, all squirts happen at the same time

      channel1InjEnabled = true;
      channel2InjEnabled = true;
      break;

    case 3:
      channel1IgnDegrees = 0;

      if (configPage1.engineType == EVEN_FIRE )
      {
        if(configPage2.sparkMode == IGN_MODE_SEQUENTIAL)
        {
          channel2IgnDegrees = 240;
          channel3IgnDegrees = 480;

          CRANK_ANGLE_MAX_IGN = 720;
        }
        else
        {
          channel2IgnDegrees = 120;
          channel3IgnDegrees = 240;
        }
      }
      else
      {
        channel2IgnDegrees = configPage1.oddfire2;
        channel3IgnDegrees = configPage1.oddfire3;
      }

      //For alternatiing injection, the squirt occurs at different times for each channel
      if(configPage1.injLayout == INJ_SEMISEQUENTIAL  || configPage1.injLayout == INJ_PAIRED)
      {
        channel1InjDegrees = 0;
        channel2InjDegrees = 120;
        channel3InjDegrees = 240;
      }
      else if (configPage1.injLayout == INJ_SEQUENTIAL)
      {
        channel1InjDegrees = 0;
        channel2InjDegrees = 240;
        channel3InjDegrees = 480;
        CRANK_ANGLE_MAX_INJ = 720;
        req_fuel_uS = req_fuel_uS * 2;
      }
      if (!configPage1.injTiming) { channel1InjDegrees = channel2InjDegrees = channel3InjDegrees = 0; } //For simultaneous, all squirts happen at the same time

      channel1InjEnabled = true;
      channel2InjEnabled = true;
      channel3InjEnabled = true;
      break;
    case 4:
      channel1IgnDegrees = 0;

      if (configPage1.engineType == EVEN_FIRE )
      {
        channel2IgnDegrees = 180;

        if(configPage2.sparkMode == IGN_MODE_SEQUENTIAL)
        {
          channel3IgnDegrees = 360;
          channel4IgnDegrees = 540;

          CRANK_ANGLE_MAX_IGN = 720;
        }
      }
      else
      {
        channel2IgnDegrees = configPage1.oddfire2;
        channel3IgnDegrees = configPage1.oddfire3;
        channel4IgnDegrees = configPage1.oddfire4;
      }

      //For alternatiing injection, the squirt occurs at different times for each channel
      if(configPage1.injLayout == INJ_SEMISEQUENTIAL || configPage1.injLayout == INJ_PAIRED)
      {
        channel1InjDegrees = 0;
        channel2InjDegrees = 180;
      }
      else if (configPage1.injLayout == INJ_SEQUENTIAL)
      {
        channel1InjDegrees = 0;
        channel2InjDegrees = 180;
        channel3InjDegrees = 360;
        channel4InjDegrees = 540;

        channel3InjEnabled = true;
        channel4InjEnabled = true;

        CRANK_ANGLE_MAX_INJ = 720;
        req_fuel_uS = req_fuel_uS * 2;
      }
      if (!configPage1.injTiming) { channel1InjDegrees = channel2InjDegrees = 0; } //For simultaneous, all squirts happen at the same time

      channel1InjEnabled = true;
      channel2InjEnabled = true;
      break;
    case 5:
      channel1IgnDegrees = 0;
      channel2IgnDegrees = 72;
      channel3IgnDegrees = 144;
      channel4IgnDegrees = 216;
      channel5IgnDegrees = 288;

      if(configPage2.sparkMode == IGN_MODE_SEQUENTIAL)
      {
        channel2IgnDegrees = 144;
        channel3IgnDegrees = 288;
        channel4IgnDegrees = 432;
        channel5IgnDegrees = 576;

        CRANK_ANGLE_MAX_IGN = 720;
      }

      //For alternatiing injection, the squirt occurs at different times for each channel
      if(configPage1.injLayout == INJ_SEMISEQUENTIAL || configPage1.injLayout == INJ_PAIRED)
      {
        channel1InjDegrees = 0;
        channel2InjDegrees = 72;
        channel3InjDegrees = 144;
        channel4InjDegrees = 216;
        channel5InjDegrees = 288;
      }
      else if (configPage1.injLayout == INJ_SEQUENTIAL)
      {
        channel1InjDegrees = 0;
        channel2InjDegrees = 144;
        channel3InjDegrees = 288;
        channel4InjDegrees = 432;
        channel5InjDegrees = 576;

        CRANK_ANGLE_MAX_INJ = 720;
      }
      if (!configPage1.injTiming) { channel1InjDegrees = channel2InjDegrees = channel3InjDegrees = channel4InjDegrees = channel5InjDegrees = 0; } //For simultaneous, all squirts happen at the same time

      channel1InjEnabled = true;
      channel2InjEnabled = true;
      channel3InjEnabled = false; //this is disabled as injector 5 function calls 3 & 5 together
      channel4InjEnabled = true;
      channel5InjEnabled = true;
      break;
    case 6:
      channel1IgnDegrees = 0;
      channel1InjDegrees = 0;
      channel2IgnDegrees = 120;
      channel2InjDegrees = 120;
      channel3IgnDegrees = 240;
      channel3InjDegrees = 240;

      if (!configPage1.injTiming) { channel1InjDegrees = channel2InjDegrees = channel3InjDegrees = 0; } //For simultaneous, all squirts happen at the same time

      configPage1.injLayout = 0; //This is a failsafe. We can never run semi-sequential with more than 4 cylinders

      channel1InjEnabled = true;
      channel2InjEnabled = true;
      channel3InjEnabled = true;
      break;
    case 8:
      channel1IgnDegrees = channel1InjDegrees = 0;
      channel2IgnDegrees = channel2InjDegrees = 90;
      channel3IgnDegrees = channel3InjDegrees = 180;
      channel4IgnDegrees = channel4InjDegrees = 270;

      if (!configPage1.injTiming)  { channel1InjDegrees = channel2InjDegrees = channel3InjDegrees = channel4InjDegrees = 0; } //For simultaneous, all squirts happen at the same time

      configPage1.injLayout = 0; //This is a failsafe. We can never run semi-sequential with more than 4 cylinders

      channel1InjEnabled = true;
      channel2InjEnabled = true;
      channel3InjEnabled = true;
      channel4InjEnabled = true;
      break;
    default: //Handle this better!!!
      channel1InjDegrees = 0;
      channel2InjDegrees = 180;
      break;
  }

  switch(configPage2.sparkMode)
  {
    case IGN_MODE_WASTED:
      //Wasted Spark (Normal mode)
      ign1StartFunction = beginCoil1Charge;
      ign1EndFunction = endCoil1Charge;
      ign2StartFunction = beginCoil2Charge;
      ign2EndFunction = endCoil2Charge;
      ign3StartFunction = beginCoil3Charge;
      ign3EndFunction = endCoil3Charge;
      ign4StartFunction = beginCoil4Charge;
      ign4EndFunction = endCoil4Charge;
      ign5StartFunction = beginCoil5Charge;
      ign5EndFunction = endCoil5Charge;
      break;

    case IGN_MODE_SINGLE:
      //Single channel mode. All ignition pulses are on channel 1
      ign1StartFunction = beginCoil1Charge;
      ign1EndFunction = endCoil1Charge;
      ign2StartFunction = beginCoil1Charge;
      ign2EndFunction = endCoil1Charge;
      ign3StartFunction = beginCoil1Charge;
      ign3EndFunction = endCoil1Charge;
      ign4StartFunction = beginCoil1Charge;
      ign4EndFunction = endCoil1Charge;
      ign5StartFunction = beginCoil1Charge;
      ign5EndFunction = endCoil1Charge;
      break;

    case IGN_MODE_WASTEDCOP:
      //Wasted COP mode. Ignition channels 1&3 and 2&4 are paired together
      //This is not a valid mode for >4 cylinders
      if( configPage1.nCylinders <= 4 )
      {
        ign1StartFunction = beginCoil1and3Charge;
        ign1EndFunction = endCoil1and3Charge;
        ign2StartFunction = beginCoil2and4Charge;
        ign2EndFunction = endCoil2and4Charge;

        ign3StartFunction = nullCallback;
        ign3EndFunction = nullCallback;
        ign4StartFunction = nullCallback;
        ign4EndFunction = nullCallback;
      }
      else
      {
        //If the person has inadvertantly selected this when running more than 4 cylinders, just use standard Wasted spark mode
        ign1StartFunction = beginCoil1Charge;
        ign1EndFunction = endCoil1Charge;
        ign2StartFunction = beginCoil2Charge;
        ign2EndFunction = endCoil2Charge;
        ign3StartFunction = beginCoil3Charge;
        ign3EndFunction = endCoil3Charge;
        ign4StartFunction = beginCoil4Charge;
        ign4EndFunction = endCoil4Charge;
        ign5StartFunction = beginCoil5Charge;
        ign5EndFunction = endCoil5Charge;
      }
      break;

    case IGN_MODE_SEQUENTIAL:
      ign1StartFunction = beginCoil1Charge;
      ign1EndFunction = endCoil1Charge;
      ign2StartFunction = beginCoil2Charge;
      ign2EndFunction = endCoil2Charge;
      ign3StartFunction = beginCoil3Charge;
      ign3EndFunction = endCoil3Charge;
      ign4StartFunction = beginCoil4Charge;
      ign4EndFunction = endCoil4Charge;
      ign5StartFunction = beginCoil5Charge;
      ign5EndFunction = endCoil5Charge;
      break;

    default:
      //Wasted spark (Shouldn't ever happen anyway)
      ign1StartFunction = beginCoil1Charge;
      ign1EndFunction = endCoil1Charge;
      ign2StartFunction = beginCoil2Charge;
      ign2EndFunction = endCoil2Charge;
      ign3StartFunction = beginCoil3Charge;
      ign3EndFunction = endCoil3Charge;
      ign4StartFunction = beginCoil4Charge;
      ign4EndFunction = endCoil4Charge;
      ign5StartFunction = beginCoil5Charge;
      ign5EndFunction = endCoil5Charge;
      break;
  }

  //Begin priming the fuel pump. This is turned off in the low resolution, 1s interrupt in timers.ino
  digitalWrite(pinFuelPump, HIGH);
  fuelPumpOn = true;
  //Perform the priming pulses. Set these to run at an arbitrary time in the future (100us). The prime pulse value is in ms*10, so need to multiple by 100 to get to uS
  setFuelSchedule1(100, (unsigned long)(configPage1.primePulse * 100));
  setFuelSchedule2(100, (unsigned long)(configPage1.primePulse * 100));
  setFuelSchedule3(100, (unsigned long)(configPage1.primePulse * 100));
  setFuelSchedule4(100, (unsigned long)(configPage1.primePulse * 100));

  initialisationComplete = true;
}

void loop()
{
      mainLoopCount++;
      //Check for any requets from serial. Serial operations are checked under 2 scenarios:
      // 1) Every 64 loops (64 Is more than fast enough for TunerStudio). This function is equivalent to ((loopCount % 64) == 1) but is considerably faster due to not using the mod or division operations
      // 2) If the amount of data in the serial buffer is greater than a set threhold (See globals.h). This is to avoid serial buffer overflow when large amounts of data is being sent
      if ( ((mainLoopCount & 31) == 1) or (Serial.available() > SERIAL_BUFFER_THRESHOLD) )
      {
        if (Serial.available() > 0)
        {
          command();
        }
      }

#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__) //ATmega2561 does not have Serial3
      //if serial3 interface is enabled then check for serial3 requests.
      if (configPage10.enable_canbus == 1)
          {
            if ( ((mainLoopCount & 31) == 1) or (CANSerial.available() > SERIAL_BUFFER_THRESHOLD) )
                {
                  if (CANSerial.available() > 0)
                    {
                    canCommand();
                    }
                }
          }

#elif defined(CORE_STM32)
      //if can or secondary serial interface is enabled then check for requests.
      if (configPage10.enable_canbus == 1)  //secondary serial interface enabled
          {
            if ( ((mainLoopCount & 31) == 1) or (Serial2.available() > SERIAL_BUFFER_THRESHOLD) )
                {
                  if (Serial2.available() > 0)
                    {
                    canCommand();
                    }
                }
          }
      else if (configPage10.enable_canbus == 2) // can module enabled
          {
            //check local can module
          }
#elif defined(CORE_TEENSY)
      //if can or secondary serial interface is enabled then check for requests.
      if (configPage10.enable_canbus == 1)  //secondary serial interface enabled
          {
            if ( ((mainLoopCount & 31) == 1) or (Serial2.available() > SERIAL_BUFFER_THRESHOLD) )
                {
                  if (Serial2.available() > 0)
                    {
                    canCommand();
                    }
                }
          }
      else if (configPage10.enable_canbus == 2) // can module enabled
          {
            //check local can module
            // if ( ((mainLoopCount & 31) == 1) or (CANbus0.available())
            //    {
            //      CANbus0.read(rx_msg);
            //    }
          }
#endif

    //Displays currently disabled
    // if (configPage1.displayType && (mainLoopCount & 255) == 1) { updateDisplay();}

    previousLoopTime = currentLoopTime;
    currentLoopTime = micros();
    unsigned long timeToLastTooth = (currentLoopTime - toothLastToothTime);
    if ( (timeToLastTooth < MAX_STALL_TIME) || (toothLastToothTime > currentLoopTime) ) //Check how long ago the last tooth was seen compared to now. If it was more than half a second ago then the engine is probably stopped. toothLastToothTime can be greater than currentLoopTime if a pulse occurs between getting the lastest time and doing the comparison
    {
      currentStatus.RPM = currentStatus.longRPM = getRPM(); //Long RPM is included here
      FUEL_PUMP_ON();
      fuelPumpOn = true; //Not sure if this is needed.
    }
    else
    {
      //We reach here if the time between teeth is too great. This VERY likely means the engine has stopped
      currentStatus.RPM = 0;
      currentStatus.PW1 = 0;
      currentStatus.VE = 0;
      toothLastToothTime = 0;
      //toothLastMinusOneToothTime = 0;
      currentStatus.hasSync = false;
      currentStatus.runSecs = 0; //Reset the counter for number of seconds running.
      secCounter = 0; //Reset our seconds counter.
      currentStatus.startRevolutions = 0;
      toothSystemCount = 0;
      MAPcurRev = 0;
      MAPcount = 0;
      currentStatus.rpmDOT = 0;
      ignitionOn = false;
      fuelOn = false;
      if (fpPrimed) { digitalWrite(pinFuelPump, LOW); } //Turn off the fuel pump, but only if the priming is complete
      fuelPumpOn = false;
      disableIdle(); //Turn off the idle PWM
      BIT_CLEAR(currentStatus.engine, BIT_ENGINE_CRANK); //Clear cranking bit (Can otherwise get stuck 'on' even with 0 rpm)
      BIT_CLEAR(currentStatus.engine, BIT_ENGINE_WARMUP); //Same as above except for WUE
    }

    //Uncomment the following for testing
    /*
    currentStatus.hasSync = true;
    currentStatus.RPM = 500;
    */

    //***Perform sensor reads***
    //-----------------------------------------------------------------------------------------------------
    readMAP();

    if ((mainLoopCount & 31) == 1) //Every 32 loops
    {
      readTPS(); //TPS reading to be performed every 32 loops (any faster and it can upset the TPSdot sampling time)

      //Check for launching/flat shift (clutch) can be done around here too
      previousClutchTrigger = clutchTrigger;
      if(configPage3.launchHiLo) { clutchTrigger = digitalRead(pinLaunch); }
      else { clutchTrigger = !digitalRead(pinLaunch); }

      if(previousClutchTrigger != clutchTrigger) { currentStatus.clutchEngagedRPM = currentStatus.RPM; }

      if (configPage3.launchEnabled && clutchTrigger && (currentStatus.clutchEngagedRPM < ((unsigned int)(configPage3.flatSArm) * 100)) && (currentStatus.RPM > ((unsigned int)(configPage3.lnchHardLim) * 100)) ) { currentStatus.launchingHard = true; BIT_SET(currentStatus.spark, BIT_SPARK_HLAUNCH); } //HardCut rev limit for 2-step launch control.
      else { currentStatus.launchingHard = false; BIT_CLEAR(currentStatus.spark, BIT_SPARK_HLAUNCH); }

      if(configPage3.flatSEnable && clutchTrigger && (currentStatus.RPM > ((unsigned int)(configPage3.flatSArm) * 100)) && (currentStatus.RPM > currentStatus.clutchEngagedRPM) ) { currentStatus.flatShiftingHard = true; }
      else { currentStatus.flatShiftingHard = false; }

      //Boost cutoff is very similar to launchControl, but with a check against MAP rather than a switch
      if(configPage3.boostCutType && currentStatus.MAP > (configPage3.boostLimit * 2) ) //The boost limit is divided by 2 to allow a limit up to 511kPa
      {
        switch(configPage3.boostCutType)
        {
          case 1:
            BIT_SET(currentStatus.spark, BIT_SPARK_BOOSTCUT);
            BIT_CLEAR(currentStatus.squirt, BIT_SQUIRT_BOOSTCUT);
            break;
          case 2:
            BIT_SET(currentStatus.squirt, BIT_SQUIRT_BOOSTCUT);
            BIT_CLEAR(currentStatus.spark, BIT_SPARK_BOOSTCUT);
            break;
          case 3:
            BIT_SET(currentStatus.spark, BIT_SPARK_BOOSTCUT);
            BIT_SET(currentStatus.squirt, BIT_SQUIRT_BOOSTCUT);
            break;
        }
      }
      else
      {
        BIT_CLEAR(currentStatus.spark, BIT_SPARK_BOOSTCUT);
        BIT_CLEAR(currentStatus.squirt, BIT_SQUIRT_BOOSTCUT);
      }

      //And check whether the tooth log buffer is ready
      if(toothHistoryIndex > TOOTH_LOG_SIZE) { BIT_SET(currentStatus.squirt, BIT_SQUIRT_TOOTHLOG1READY); }

      //Most boost tends to run at about 30Hz, so placing it here ensures a new target time is fetched frequently enough
      boostControl();
    }
    if( (mainLoopCount & 63) == 1) //Every 64 loops
    {
      //Nothing here currently
    }
    //The IAT and CLT readings can be done less frequently. This still runs about 4 times per second
    if ((mainLoopCount & 255) == 1) //Every 256 loops
    {
       readCLT();
       readIAT();
       readO2();
       readBat();
#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__) //ATmega2561 does not have Serial3
      //if Can interface is enabled then check for serial3 requests.
      if (configPage10.enable_canbus == 1)  // megas only support can via secondary serial
          {
            if (configPage10.enable_candata_in)
              {
                if (BIT_CHECK(configPage10.caninput_sel,currentStatus.current_caninchannel))  //if current input channel bit is enabled
                  {
                    sendCancommand(2,0,currentStatus.current_caninchannel,0,((configPage10.caninput_param_group[currentStatus.current_caninchannel]&2047)+256));    //send an R command for data from paramgroup[currentStatus.current_caninchannel]
                  }
                else
                  {
                    if (currentStatus.current_caninchannel < 15)
                        {
                          currentStatus.current_caninchannel++;   //step to next input channel if under 15
                        }
                    else
                        {
                          currentStatus.current_caninchannel = 0;   //reset input channel back to 1
                        }
                  }
              }
          }
#elif defined(CORE_STM32) || defined(CORE_TEENSY)
      //if serial3io is enabled then check for serial3 requests.
            if (configPage10.enable_candata_in)
              {
                if (BIT_CHECK(configPage10.caninput_sel,currentStatus.current_caninchannel))  //if current input channel is enabled
                  {
                    if (configPage10.enable_canbus == 1)  //can via secondary serial
                    {
                      sendCancommand(2,0,currentStatus.current_caninchannel,0,((configPage10.caninput_param_group[currentStatus.current_caninchannel]&2047)+256));    //send an R command for data from paramgroup[currentStatus.current_caninchannel]
                    }
                    else if (configPage10.enable_canbus == 2) // can via internal can module
                    {
                      sendCancommand(3,configPage10.speeduino_tsCanId,currentStatus.current_caninchannel,0,configPage10.caninput_param_group[currentStatus.current_caninchannel]);    //send via localcanbus the command for data from paramgroup[currentStatus.current_caninchannel]
                    }
                  }
                else
                  {
                    if (currentStatus.current_caninchannel < 15)
                        {
                          currentStatus.current_caninchannel++;   //step to next input channel if under 15
                        }
                    else
                        {
                          currentStatus.current_caninchannel = 0;   //reset input channel back to 0
                        }
                  }
              }

#endif
       vvtControl();
       idleControl(); //Perform any idle related actions. Even at higher frequencies, running 4x per second is sufficient.
    }
    if ((mainLoopCount & 1023) == 1) //Every 1024 loops (Approx. 1 per second)
    {
      //Approx. once per second
      readBaro();
    }

    if(configPage4.iacAlgorithm == IAC_ALGORITHM_STEP_OL || configPage4.iacAlgorithm == IAC_ALGORITHM_STEP_CL) { idleControl(); } //Run idlecontrol every loop for stepper idle.

    //Always check for sync
    //Main loop runs within this clause
    if (currentStatus.hasSync && (currentStatus.RPM > 0))
    {
        if(currentStatus.startRevolutions >= configPage2.StgCycles)  { ignitionOn = true; fuelOn = true; } //Enable the fuel and ignition, assuming staging revolutions are complete
        //If it is, check is we're running or cranking
        if(currentStatus.RPM > currentStatus.crankRPM) //Crank RPM stored in byte as RPM / 100
        {
          BIT_SET(currentStatus.engine, BIT_ENGINE_RUN); //Sets the engine running bit
          //Only need to do anything if we're transitioning from cranking to running
          if( BIT_CHECK(currentStatus.engine, BIT_ENGINE_CRANK) )
          {
            BIT_CLEAR(currentStatus.engine, BIT_ENGINE_CRANK); //clears the engine cranking bit
            if(configPage2.ignBypassEnabled) { digitalWrite(pinIgnBypass, HIGH); }
          }
        }
        else
        {  //Sets the engine cranking bit, clears the engine running bit
          BIT_SET(currentStatus.engine, BIT_ENGINE_CRANK);
          BIT_CLEAR(currentStatus.engine, BIT_ENGINE_RUN);
          currentStatus.runSecs = 0; //We're cranking (hopefully), so reset the engine run time to prompt ASE.
          if(configPage2.ignBypassEnabled) { digitalWrite(pinIgnBypass, LOW); }
        }
      //END SETTING STATUSES
      //-----------------------------------------------------------------------------------------------------

      //Begin the fuel calculation
      //Calculate an injector pulsewidth from the VE
      currentStatus.corrections = correctionsFuel();
      lastAdvance = currentStatus.advance; //Store the previous advance value
      if (configPage1.algorithm == 0) //Check which fuelling algorithm is being used
      {
        //Speed Density
        currentStatus.VE = get3DTableValue(&fuelTable, currentStatus.MAP, currentStatus.RPM); //Perform lookup into fuel map for RPM vs MAP value
        currentStatus.PW1 = PW_SD(req_fuel_uS, currentStatus.VE, currentStatus.MAP, currentStatus.corrections, inj_opentime_uS);
        currentStatus.advance = get3DTableValue(&ignitionTable, currentStatus.MAP, currentStatus.RPM) - OFFSET_IGNITION; //As above, but for ignition advance
      }
      else
      {
        //Alpha-N
        currentStatus.VE = get3DTableValue(&fuelTable, currentStatus.TPS, currentStatus.RPM); //Perform lookup into fuel map for RPM vs TPS value
        currentStatus.PW1 = PW_AN(req_fuel_uS, currentStatus.VE, currentStatus.TPS, currentStatus.corrections, inj_opentime_uS); //Calculate pulsewidth using the Alpha-N algorithm (in uS)
        currentStatus.advance = get3DTableValue(&ignitionTable, currentStatus.TPS, currentStatus.RPM) - OFFSET_IGNITION; //As above, but for ignition advance
      }

      currentStatus.advance = correctionsIgn(currentStatus.advance);

      int injector1StartAngle = 0;
      int injector2StartAngle = 0;
      int injector3StartAngle = 0; //Currently used for 3 cylinder only
      int injector4StartAngle = 0; //Not used until sequential gets written
      int injector5StartAngle = 0; //For 5 cylinder testing
      int ignition1StartAngle = 0;
      int ignition2StartAngle = 0;
      int ignition3StartAngle = 0; //Currently used for 3 cylinder only
      int ignition4StartAngle = 0; //Not used until sequential or 4+ cylinders support gets written
      int ignition5StartAngle = 0; //Not used until sequential or 4+ cylinders support gets written
      //These are used for comparisons on channels above 1 where the starting angle (for injectors or ignition) can be less than a single loop time
      //(Don't ask why this is needed, it will break your head)
      int tempCrankAngle;
      int tempStartAngle;

      //********************************************************
      //How fast are we going? Need to know how long (uS) it will take to get from one tooth to the next. We then use that to estimate how far we are between the last tooth and the next one
      //We use a 1st Deriv accleration prediction, but only when there is an even spacing between primary sensor teeth
      //Any decoder that has uneven spacing has its triggerToothAngle set to 0
      if(secondDerivEnabled && toothHistoryIndex >= 3 && currentStatus.RPM < 2000) //toothHistoryIndex must be greater than or equal to 3 as we need the last 3 entries. Currently this mode only runs below 3000 rpm
      //if(true)
      {
        //Only recalculate deltaV if the tooth has changed since last time (DeltaV stays the same until the next tooth)
        //if (deltaToothCount != toothCurrentCount)
        {
          deltaToothCount = toothCurrentCount;
          int angle1, angle2; //These represent the crank angles that are travelled for the last 2 pulses
          if(configPage2.TrigPattern == 4)
          {
            //Special case for 70/110 pattern on 4g63
            angle2 = triggerToothAngle; //Angle 2 is the most recent
            if (angle2 == 70) { angle1 = 110; }
            else { angle1 = 70; }
          }
          else if(configPage2.TrigPattern == 0)
          {
            //Special case for missing tooth decoder where the missing tooth was one of the last 2 seen
            if(toothCurrentCount == 1) { angle2 = 2*triggerToothAngle; angle1 = triggerToothAngle; }
            else if(toothCurrentCount == 2) { angle1 = 2*triggerToothAngle; angle2 = triggerToothAngle; }
            else { angle1 = angle2 = triggerToothAngle; }
          }
          else { angle1 = angle2 = triggerToothAngle; }

          long toothDeltaV = (1000000L * angle2 / toothHistory[toothHistoryIndex]) - (1000000L * angle1 / toothHistory[toothHistoryIndex-1]);
          long toothDeltaT = toothHistory[toothHistoryIndex];
          //long timeToLastTooth = micros() - toothLastToothTime;

          rpmDelta = (toothDeltaV << 10) / (6 * toothDeltaT);
        }


        timePerDegree = ldiv( 166666L, (currentStatus.RPM + rpmDelta)).quot; //There is a small amount of rounding in this calculation, however it is less than 0.001 of a uS (Faster as ldiv than / )
      }
      else
      {
        long rpm_adjust = ((long)(micros() - toothOneTime) * (long)currentStatus.rpmDOT) / 1000000; //Take into account any likely accleration that has occurred since the last full revolution completed

        //timePerDegree = DIV_ROUND_CLOSEST(166666L, (currentStatus.RPM + rpm_adjust));
        timePerDegree = ldiv( 166666L, currentStatus.RPM + rpm_adjust).quot; //There is a small amount of rounding in this calculation, however it is less than 0.001 of a uS (Faster as ldiv than / )
      }

      //Check that the duty cycle of the chosen pulsewidth isn't too high. This is disabled at cranking
      if( !BIT_CHECK(currentStatus.engine, BIT_ENGINE_CRANK) )
      {
        unsigned long pwLimit = percentage(configPage1.dutyLim, revolutionTime); //The pulsewidth limit is determined to be the duty cycle limit (Eg 85%) by the total time it takes to perform 1 revolution
        if (CRANK_ANGLE_MAX_INJ == 720) { pwLimit = pwLimit * 2; } //For sequential, the maximum pulse time is double (2 revolutions). Wouldn't work for 2 stroke...
        if (currentStatus.PW1 > pwLimit) { currentStatus.PW1 = pwLimit; }
      }


      //***********************************************************************************************
      //BEGIN INJECTION TIMING
      //Determine next firing angles
      currentStatus.PW2 = currentStatus.PW3 = currentStatus.PW4 = currentStatus.PW1; // Initial state is for all pulsewidths to be the same (This gets changed below)
      if(!configPage1.indInjAng) {configPage1.inj4Ang = configPage1.inj3Ang = configPage1.inj2Ang = configPage1.inj1Ang;} //Forcing all injector close angles to be the same.
      int PWdivTimerPerDegree = div(currentStatus.PW1, timePerDegree).quot; //How many crank degrees the calculated PW will take at the current speed
      injector1StartAngle = configPage1.inj1Ang - ( PWdivTimerPerDegree ); //This is a little primitive, but is based on the idea that all fuel needs to be delivered before the inlet valve opens. See http://www.extraefi.co.uk/sequential_fuel.html for more detail
      if(injector1StartAngle < 0) {injector1StartAngle += CRANK_ANGLE_MAX_INJ;}

      //Repeat the above for each cylinder
      switch (configPage1.nCylinders)
      {
        //2 cylinders
        case 2:
          injector2StartAngle = (configPage1.inj2Ang + channel2InjDegrees - ( PWdivTimerPerDegree ));
          if(injector2StartAngle > CRANK_ANGLE_MAX_INJ) {injector2StartAngle -= CRANK_ANGLE_MAX_INJ;}
          break;
        //3 cylinders
        case 3:
          injector2StartAngle = (configPage1.inj2Ang + channel2InjDegrees - ( PWdivTimerPerDegree ));
          if(injector2StartAngle > CRANK_ANGLE_MAX_INJ) {injector2StartAngle -= CRANK_ANGLE_MAX_INJ;}
          injector3StartAngle = (configPage1.inj3Ang + channel3InjDegrees - ( PWdivTimerPerDegree ));
          if(injector3StartAngle > CRANK_ANGLE_MAX_INJ) {injector3StartAngle -= CRANK_ANGLE_MAX_INJ;}
          break;
        //4 cylinders
        case 4:
          injector2StartAngle = (configPage1.inj2Ang + channel2InjDegrees - ( PWdivTimerPerDegree ));
          if(injector2StartAngle > CRANK_ANGLE_MAX_INJ) {injector2StartAngle -= CRANK_ANGLE_MAX_INJ;}

          if(configPage1.injLayout == INJ_SEQUENTIAL)
          {
            injector3StartAngle = (configPage1.inj3Ang + channel3InjDegrees - ( PWdivTimerPerDegree ));
            if(injector3StartAngle > CRANK_ANGLE_MAX_INJ) {injector3StartAngle -= CRANK_ANGLE_MAX_INJ;}
            injector4StartAngle = (configPage1.inj4Ang + channel4InjDegrees - ( PWdivTimerPerDegree ));
            if(injector4StartAngle > CRANK_ANGLE_MAX_INJ) {injector4StartAngle -= CRANK_ANGLE_MAX_INJ;}

            if(configPage3.fuelTrimEnabled)
            {
              unsigned long pw1percent = 100 + (byte)get3DTableValue(&trim1Table, currentStatus.MAP, currentStatus.RPM) - OFFSET_FUELTRIM;
              unsigned long pw2percent = 100 + (byte)get3DTableValue(&trim2Table, currentStatus.MAP, currentStatus.RPM) - OFFSET_FUELTRIM;
              unsigned long pw3percent = 100 + (byte)get3DTableValue(&trim3Table, currentStatus.MAP, currentStatus.RPM) - OFFSET_FUELTRIM;
              unsigned long pw4percent = 100 + (byte)get3DTableValue(&trim4Table, currentStatus.MAP, currentStatus.RPM) - OFFSET_FUELTRIM;

              if (pw1percent != 100) { currentStatus.PW1 = (pw1percent * currentStatus.PW1) / 100; }
              if (pw2percent != 100) { currentStatus.PW2 = (pw2percent * currentStatus.PW2) / 100; }
              if (pw3percent != 100) { currentStatus.PW3 = (pw3percent * currentStatus.PW3) / 100; }
              if (pw4percent != 100) { currentStatus.PW4 = (pw4percent * currentStatus.PW4) / 100; }
            }
          }
          break;
        //5 cylinders
        case 5:
          injector2StartAngle = (configPage1.inj2Ang + channel2InjDegrees - ( PWdivTimerPerDegree ));
          if(injector2StartAngle > CRANK_ANGLE_MAX_INJ) {injector2StartAngle -= CRANK_ANGLE_MAX_INJ;}
          injector3StartAngle = (configPage1.inj3Ang + channel3InjDegrees - ( PWdivTimerPerDegree ));
          if(injector3StartAngle > CRANK_ANGLE_MAX_INJ) {injector3StartAngle -= CRANK_ANGLE_MAX_INJ;}
          injector4StartAngle = (configPage1.inj4Ang + channel4InjDegrees - ( PWdivTimerPerDegree ));
          if(injector4StartAngle > CRANK_ANGLE_MAX_INJ) {injector4StartAngle -= CRANK_ANGLE_MAX_INJ;}
          injector5StartAngle = (configPage1.inj1Ang + channel5InjDegrees - ( PWdivTimerPerDegree ));
          if(injector5StartAngle > CRANK_ANGLE_MAX_INJ) {injector5StartAngle -= CRANK_ANGLE_MAX_INJ;}
          break;
        //6 cylinders
        case 6:
          injector2StartAngle = (configPage1.inj2Ang + channel2InjDegrees - ( PWdivTimerPerDegree ));
          if(injector2StartAngle > CRANK_ANGLE_MAX_INJ) {injector2StartAngle -= CRANK_ANGLE_MAX_INJ;}
          injector3StartAngle = (configPage1.inj3Ang + channel3InjDegrees - ( PWdivTimerPerDegree ));
          if(injector3StartAngle > CRANK_ANGLE_MAX_INJ) {injector3StartAngle -= CRANK_ANGLE_MAX_INJ;}
          break;
        //8 cylinders
        case 8:
          injector2StartAngle = (configPage1.inj2Ang + channel2InjDegrees - ( PWdivTimerPerDegree ));
          if(injector2StartAngle > CRANK_ANGLE_MAX_INJ) {injector2StartAngle -= CRANK_ANGLE_MAX_INJ;}
          injector3StartAngle = (configPage1.inj3Ang + channel3InjDegrees - ( PWdivTimerPerDegree ));
          if(injector3StartAngle > CRANK_ANGLE_MAX_INJ) {injector3StartAngle -= CRANK_ANGLE_MAX_INJ;}
          injector4StartAngle = (configPage1.inj4Ang + channel4InjDegrees - ( PWdivTimerPerDegree ));
          if(injector4StartAngle > CRANK_ANGLE_MAX_INJ) {injector4StartAngle -= CRANK_ANGLE_MAX_INJ;}
          break;
        //Will hit the default case on 1 cylinder or >8 cylinders. Do nothing in these cases
        default:
          break;
      }

      //***********************************************************************************************
      //| BEGIN IGNITION CALCULATIONS
      BIT_CLEAR(currentStatus.spark, BIT_SPARK_HRDLIM);
      if (currentStatus.RPM > ((unsigned int)(configPage2.HardRevLim) * 100) ) { BIT_SET(currentStatus.spark, BIT_SPARK_HRDLIM); } //Hardcut RPM limit


      //Set dwell
       //Dwell is stored as ms * 10. ie Dwell of 4.3ms would be 43 in configPage2. This number therefore needs to be multiplied by 100 to get dwell in uS
      if ( BIT_CHECK(currentStatus.engine, BIT_ENGINE_CRANK) ) { currentStatus.dwell =  (configPage2.dwellCrank * 100); }
      else { currentStatus.dwell =  (configPage2.dwellRun * 100); }
      currentStatus.dwell = correctionsDwell(currentStatus.dwell);

      int dwellAngle = (div(currentStatus.dwell, timePerDegree).quot ); //Convert the dwell time to dwell angle based on the current engine speed

      //Calculate start angle for each channel
      //1 cylinder (Everyone gets this)
      ignition1EndAngle = CRANK_ANGLE_MAX_IGN - currentStatus.advance;
      ignition1StartAngle = ignition1EndAngle - dwellAngle; // 360 - desired advance angle - number of degrees the dwell will take
      if(ignition1StartAngle < 0) {ignition1StartAngle += CRANK_ANGLE_MAX_IGN;}

      //This test for more cylinders and do the same thing
      switch (configPage1.nCylinders)
      {
        //2 cylinders
        case 2:
          ignition2EndAngle = channel2IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition2StartAngle = ignition2EndAngle - dwellAngle;
          if(ignition2StartAngle > CRANK_ANGLE_MAX_IGN) {ignition2StartAngle -= CRANK_ANGLE_MAX_IGN;}
          break;
        //3 cylinders
        case 3:
          ignition2EndAngle = channel2IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition2StartAngle = ignition2EndAngle - dwellAngle;
          if(ignition2StartAngle > CRANK_ANGLE_MAX_IGN) {ignition2StartAngle -= CRANK_ANGLE_MAX_IGN;}
          ignition3EndAngle = channel3IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition3StartAngle = channel3IgnDegrees + 360 - currentStatus.advance - dwellAngle;
          if(ignition3StartAngle > CRANK_ANGLE_MAX_IGN) {ignition3StartAngle -= CRANK_ANGLE_MAX_IGN;}
          break;
        //4 cylinders
        case 4:
          ignition2EndAngle = channel2IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition2StartAngle = ignition2EndAngle - dwellAngle;
          if(ignition2StartAngle > CRANK_ANGLE_MAX_IGN) {ignition2StartAngle -= CRANK_ANGLE_MAX_IGN;}
          if(ignition2StartAngle < 0) {ignition2StartAngle += CRANK_ANGLE_MAX_IGN;}

          if(configPage2.sparkMode == IGN_MODE_SEQUENTIAL)
          {
            ignition3EndAngle = channel3IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
            ignition3StartAngle = ignition3EndAngle - dwellAngle;
            if(ignition3StartAngle > CRANK_ANGLE_MAX_IGN) {ignition3StartAngle -= CRANK_ANGLE_MAX_IGN;}

            ignition4EndAngle = channel4IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
            ignition4StartAngle = ignition4EndAngle - dwellAngle;
            if(ignition4StartAngle > CRANK_ANGLE_MAX_IGN) {ignition4StartAngle -= CRANK_ANGLE_MAX_IGN;}
          }
          break;
        //5 cylinders
        case 5:
          ignition2EndAngle = channel2IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition2StartAngle = ignition2EndAngle - dwellAngle;
          if(ignition2StartAngle > CRANK_ANGLE_MAX_IGN) {ignition2StartAngle -= CRANK_ANGLE_MAX_IGN;}
          if(ignition2StartAngle < 0) {ignition2StartAngle += CRANK_ANGLE_MAX_IGN;}

          ignition3EndAngle = channel3IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition3StartAngle = ignition3EndAngle - dwellAngle;
          if(ignition3StartAngle > CRANK_ANGLE_MAX_IGN) {ignition3StartAngle -= CRANK_ANGLE_MAX_IGN;}

          ignition4EndAngle = channel4IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition4StartAngle = ignition4EndAngle - dwellAngle;
          if(ignition4StartAngle > CRANK_ANGLE_MAX_IGN) {ignition4StartAngle -= CRANK_ANGLE_MAX_IGN;}

          ignition5StartAngle = channel5IgnDegrees + CRANK_ANGLE_MAX - currentStatus.advance - dwellAngle;
          if(ignition5StartAngle > CRANK_ANGLE_MAX_IGN) {ignition5StartAngle -= CRANK_ANGLE_MAX_IGN;}

          break;
        //6 cylinders
        case 6:
          ignition2EndAngle = channel2IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition2StartAngle = ignition2EndAngle - dwellAngle;
          if(ignition2StartAngle > CRANK_ANGLE_MAX_IGN) {ignition2StartAngle -= CRANK_ANGLE_MAX_IGN;}

          ignition3EndAngle = channel3IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition3StartAngle = ignition3EndAngle - dwellAngle;
          if(ignition3StartAngle > CRANK_ANGLE_MAX_IGN) {ignition3StartAngle -= CRANK_ANGLE_MAX_IGN;}
          break;
        //8 cylinders
        case 8:
          ignition2EndAngle = channel2IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition2StartAngle = ignition2EndAngle - dwellAngle;
          if(ignition2StartAngle > CRANK_ANGLE_MAX_IGN) {ignition2StartAngle -= CRANK_ANGLE_MAX_IGN;}

          ignition3EndAngle = channel3IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition3StartAngle = ignition3EndAngle - dwellAngle;
          if(ignition3StartAngle > CRANK_ANGLE_MAX_IGN) {ignition3StartAngle -= CRANK_ANGLE_MAX_IGN;}

          ignition4EndAngle = channel4IgnDegrees + CRANK_ANGLE_MAX_IGN - currentStatus.advance;
          ignition4StartAngle = ignition4EndAngle - dwellAngle;
          if(ignition4StartAngle > CRANK_ANGLE_MAX_IGN) {ignition4StartAngle -= CRANK_ANGLE_MAX_IGN;}
          break;

        //Will hit the default case on 1 cylinder or >8 cylinders. Do nothing in these cases
        default:
          break;
      }
      //If ignition timing is being tracked per tooth, perform the calcs to get the end teeth
      //This only needs to be run if the advance figure has changed, otherwise the end teeth will still be the same
      if( (configPage1.perToothIgn == true) && (lastAdvance != currentStatus.advance) ) { triggerSetEndTeeth(); }

      //***********************************************************************************************
      //| BEGIN FUEL SCHEDULES
      //Finally calculate the time (uS) until we reach the firing angles and set the schedules
      //We only need to set the shcedule if we're BEFORE the open angle
      //This may potentially be called a number of times as we get closer and closer to the opening time

      //Determine the current crank angle
      int crankAngle = getCrankAngle(timePerDegree);
      if (crankAngle > CRANK_ANGLE_MAX_INJ ) { crankAngle -= 360; }

      if (fuelOn && currentStatus.PW1 > 0 && !BIT_CHECK(currentStatus.squirt, BIT_SQUIRT_BOOSTCUT))
      {
        if ( (injector1StartAngle <= crankAngle) && (fuelSchedule1.Status == RUNNING) ) { injector1StartAngle += CRANK_ANGLE_MAX_INJ; }
        if (injector1StartAngle > crankAngle)
        {
          setFuelSchedule1(
                    ((unsigned long)(injector1StartAngle - crankAngle) * (unsigned long)timePerDegree),
                    (unsigned long)currentStatus.PW1
                    );
        }

        /*-----------------------------------------------------------------------------------------
        | A Note on tempCrankAngle and tempStartAngle:
        |   The use of tempCrankAngle/tempStartAngle is described below. It is then used in the same way for channels 2, 3 and 4 on both injectors and ignition
        |   Essentially, these 2 variables are used to realign the current crank angle and the desired start angle around 0 degrees for the given cylinder/output
        |   Eg: If cylinder 2 TDC is 180 degrees after cylinder 1 (Eg a standard 4 cylidner engine), then tempCrankAngle is 180* less than the current crank angle and
        |       tempStartAngle is the desired open time less 180*. Thus the cylinder is being treated relative to its own TDC, regardless of its offset
        |
        |   This is done to avoid problems with very short of very long times until tempStartAngle.
        |   This will very likely need to be rewritten when sequential is enabled
        |------------------------------------------------------------------------------------------
        */
        if(channel2InjEnabled)
        {
          tempCrankAngle = crankAngle - channel2InjDegrees;
          if( tempCrankAngle < 0) { tempCrankAngle += CRANK_ANGLE_MAX_INJ; }
          tempStartAngle = injector2StartAngle - channel2InjDegrees;
          if ( tempStartAngle < 0) { tempStartAngle += CRANK_ANGLE_MAX_INJ; }
          if ( (tempStartAngle <= tempCrankAngle) && (fuelSchedule2.Status == RUNNING) ) { tempStartAngle += CRANK_ANGLE_MAX_INJ; }
          if ( tempStartAngle > tempCrankAngle )
          {
            setFuelSchedule2(
                      ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree),
                      (unsigned long)currentStatus.PW2
                      );
          }
        }

        if(channel3InjEnabled)
        {
          tempCrankAngle = crankAngle - channel3InjDegrees;
          if( tempCrankAngle < 0) { tempCrankAngle += CRANK_ANGLE_MAX_INJ; }
          tempStartAngle = injector3StartAngle - channel3InjDegrees;
          if ( tempStartAngle < 0) { tempStartAngle += CRANK_ANGLE_MAX_INJ; }
          if ( (tempStartAngle <= tempCrankAngle) && (fuelSchedule3.Status == RUNNING) ) { tempStartAngle += CRANK_ANGLE_MAX_INJ; }
          if ( tempStartAngle > tempCrankAngle )
          {
            setFuelSchedule3(
                      ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree),
                      (unsigned long)currentStatus.PW3
                      );
          }
        }

        if(channel4InjEnabled)
        {
          tempCrankAngle = crankAngle - channel4InjDegrees;
          if( tempCrankAngle < 0) { tempCrankAngle += CRANK_ANGLE_MAX_INJ; }
          tempStartAngle = injector4StartAngle - channel4InjDegrees;
          if ( tempStartAngle < 0) { tempStartAngle += CRANK_ANGLE_MAX_INJ; }
          if ( (tempStartAngle <= tempCrankAngle) && (fuelSchedule4.Status == RUNNING) ) { tempStartAngle += CRANK_ANGLE_MAX_INJ; }
          if ( tempStartAngle > tempCrankAngle )
          {
            setFuelSchedule4(
                      ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree),
                      (unsigned long)currentStatus.PW4
                      );
          }
        }

        if(channel5InjEnabled)
        {
          tempCrankAngle = crankAngle - channel5InjDegrees;
          if( tempCrankAngle < 0) { tempCrankAngle += CRANK_ANGLE_MAX_INJ; }
          tempStartAngle = injector5StartAngle - channel5InjDegrees;
          if ( tempStartAngle < 0) { tempStartAngle += CRANK_ANGLE_MAX_INJ; }
          if (tempStartAngle <= tempCrankAngle && fuelSchedule5.schedulesSet == 0) { tempStartAngle += CRANK_ANGLE_MAX_INJ; }
          if ( tempStartAngle > tempCrankAngle )
          {
            //Note the hacky use of fuel schedule 3 below
            /*
            setFuelSchedule3(openInjector3and5,
                      ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree),
                      (unsigned long)currentStatus.PW1,
                      closeInjector3and5
                    );*/
            setFuelSchedule3(
                      ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree),
                      (unsigned long)currentStatus.PW1
                      );
          }
        }
      }
      //***********************************************************************************************
      //| BEGIN IGNITION SCHEDULES
      //Likewise for the ignition

      //fixedCrankingOverride is used to extend the dwell during cranking so that the decoder can trigger the spark upon seeing a certain tooth. Currently only available on the basic distributor and 4g63 decoders.
      if ( configPage2.ignCranklock && BIT_CHECK(currentStatus.engine, BIT_ENGINE_CRANK)) { fixedCrankingOverride = currentStatus.dwell * 3; }
      else { fixedCrankingOverride = 0; }

      //Perform an initial check to see if the ignition is turned on (Ignition only turns on after a preset number of cranking revolutions and:
      //Check for hard cut rev limit (If we're above the hardcut limit, we simply don't set a spark schedule)
      if(ignitionOn && !currentStatus.launchingHard && !BIT_CHECK(currentStatus.spark, BIT_SPARK_BOOSTCUT) && !BIT_CHECK(currentStatus.spark, BIT_SPARK_HRDLIM) && !currentStatus.flatShiftingHard)
      {

        //Refresh the current crank angle info
        //ignition1StartAngle = 335;
        crankAngle = getCrankAngle(timePerDegree); //Refresh with the latest crank angle
        if (crankAngle > CRANK_ANGLE_MAX_IGN ) { crankAngle -= 360; }

        if (ignition1StartAngle > crankAngle)
        {
            /*
            long some_time = ((unsigned long)(ignition1StartAngle - crankAngle) * (unsigned long)timePerDegree);
            long newRPM = (long)(some_time * currentStatus.rpmDOT) / 1000000L;
            newRPM = currentStatus.RPM + (newRPM/2);
            unsigned long timePerDegree_1 = ldiv( 166666L, newRPM).quot;
            unsigned long timeout = (unsigned long)(ignition1StartAngle - crankAngle) * 282UL;
            */
            setIgnitionSchedule1(ign1StartFunction,
                      ((unsigned long)(ignition1StartAngle - crankAngle) * (unsigned long)timePerDegree), //(timeout/10),
                      currentStatus.dwell + fixedCrankingOverride, //((unsigned long)((unsigned long)currentStatus.dwell* currentStatus.RPM) / newRPM) + fixedCrankingOverride,
                      ign1EndFunction
                      );
        }

        tempCrankAngle = crankAngle - channel2IgnDegrees;
        if( tempCrankAngle < 0) { tempCrankAngle += CRANK_ANGLE_MAX_IGN; }
        tempStartAngle = ignition2StartAngle - channel2IgnDegrees;
        if ( tempStartAngle < 0) { tempStartAngle += CRANK_ANGLE_MAX_IGN; }
        {
            unsigned long ignition2StartTime = 0;
            if(tempStartAngle > tempCrankAngle) { ignition2StartTime = ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree); }
            //else if (tempStartAngle < tempCrankAngle) { ignition2StartTime = ((long)(360 - tempCrankAngle + tempStartAngle) * (long)timePerDegree); }
            else { ignition2StartTime = 0; }

            if(ignition2StartTime > 0) {
            setIgnitionSchedule2(ign2StartFunction,
                      ignition2StartTime,
                      currentStatus.dwell + fixedCrankingOverride,
                      ign2EndFunction
                      );
            }
        }

        tempCrankAngle = crankAngle - channel3IgnDegrees;
        if( tempCrankAngle < 0) { tempCrankAngle += CRANK_ANGLE_MAX_IGN; }
        tempStartAngle = ignition3StartAngle - channel3IgnDegrees;
        if ( tempStartAngle < 0) { tempStartAngle += CRANK_ANGLE_MAX_IGN; }
        //if (tempStartAngle > tempCrankAngle)
        {
            long ignition3StartTime = 0;
            if(tempStartAngle > tempCrankAngle) { ignition3StartTime = ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree); }
            //else if (tempStartAngle < tempCrankAngle) { ignition4StartTime = ((long)(360 - tempCrankAngle + tempStartAngle) * (long)timePerDegree); }
            else { ignition3StartTime = 0; }

            if(ignition3StartTime > 0) {
            setIgnitionSchedule3(ign3StartFunction,
                      ignition3StartTime,
                      currentStatus.dwell + fixedCrankingOverride,
                      ign3EndFunction
                      );
            }
        }

        tempCrankAngle = crankAngle - channel4IgnDegrees;
        if( tempCrankAngle < 0) { tempCrankAngle += CRANK_ANGLE_MAX_IGN; }
        tempStartAngle = ignition4StartAngle - channel4IgnDegrees;
        if ( tempStartAngle < 0) { tempStartAngle += CRANK_ANGLE_MAX_IGN; }
        //if (tempStartAngle > tempCrankAngle)
        {

            long ignition4StartTime = 0;
            if(tempStartAngle > tempCrankAngle) { ignition4StartTime = ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree); }
            //else if (tempStartAngle < tempCrankAngle) { ignition4StartTime = ((long)(360 - tempCrankAngle + tempStartAngle) * (long)timePerDegree); }
            else { ignition4StartTime = 0; }

            if(ignition4StartTime > 0) {
            setIgnitionSchedule4(ign4StartFunction,
                      ignition4StartTime,
                      currentStatus.dwell + fixedCrankingOverride,
                      ign4EndFunction
                      );
            }
        }

        tempCrankAngle = crankAngle - channel5IgnDegrees;
        if( tempCrankAngle < 0) { tempCrankAngle += CRANK_ANGLE_MAX_IGN; }
        tempStartAngle = ignition5StartAngle - channel5IgnDegrees;
        if ( tempStartAngle < 0) { tempStartAngle += CRANK_ANGLE_MAX_IGN; }
        //if (tempStartAngle > tempCrankAngle)
        {

            long ignition5StartTime = 0;
            if(tempStartAngle > tempCrankAngle) { ignition5StartTime = ((unsigned long)(tempStartAngle - tempCrankAngle) * (unsigned long)timePerDegree); }
            //else if (tempStartAngle < tempCrankAngle) { ignition4StartTime = ((long)(360 - tempCrankAngle + tempStartAngle) * (long)timePerDegree); }
            else { ignition5StartTime = 0; }

            if(ignition5StartTime > 0) {
            setIgnitionSchedule5(ign5StartFunction,
                      ignition5StartTime,
                      currentStatus.dwell + fixedCrankingOverride,
                      ign5EndFunction
                      );
            }
        }
      } //Ignition schedules on
    } //Has sync and RPM
} //loop()
