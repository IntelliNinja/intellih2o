/*********************************************************************
// BLuetooth Low Energy for IntelliH2O by Christopher Lee
//  IntelliH2O
//
//  Created by Christopher Lee on 08-04-15.
//  This App requires the accompanying Low-Energy Bluetooth hardware as described on:
//  http://www.intellininja.net
//  
// Based on: nRFManager democode avail on GitHub
// It also requires the accompanying iPhone/iPad app: IntelliH2O,
// available on the Apple AppStore (pending Apple Review)
// Copyright (c) 2015 IntelliNinja Productions. All rights reserved.
*********************************************************************/

// This version uses the internal data queing so you can treat it like Serial (kinda)!

#include <SPI.h>                  // CSL:Original EchoDemo Includes
#include "Adafruit_BLE_UART.h"    // CSL:Original EchoDemo Includes
#include <Servo.h>                // CSL:Add'l StandardFirmata Includes for I/O pins
#include <Wire.h>                 // CSL:Add'l StandardFirmata Includes for I/O pins
#include <Adafruit_BLE_Firmata.h>c

#define AUTO_INPUT_PULLUPS true   // CSL:Add'l StandardFirmata Includes for I/O pins

// Connect CLK/MISO/MOSI to hardware SPI
// e.g. On UNO & compatible: CLK = 13, MISO = 12, MOSI = 11
#define ADAFRUITBLE_REQ 10
#define ADAFRUITBLE_RDY 2     // This should be an interrupt pin, on Uno thats #2 or #3
#define ADAFRUITBLE_RST 9

// so we have digital 3-8 and analog 0-6

// CSL:CHANGED ALL Adafruit_BLE_URART Object name "BTLEserial" to "BLEserial"  

Adafruit_BLE_UART BLEserial = Adafruit_BLE_UART(ADAFRUITBLE_REQ, ADAFRUITBLE_RDY, ADAFRUITBLE_RST);

// make one instance for the user to use
Adafruit_BLE_FirmataClass BLE_Firmata(BLEserial);  // CSL:Add'l StandardFirmata Includes for I/O pins

/*==============================================================================
 * GLOBAL VARIABLES  -  CSL:Add'l StandardFirmata Includes for I/O pins
 *============================================================================*/

/* analog inputs */
int analogInputsToReport = 0; // bitwise array to store pin reporting
int lastAnalogReads[NUM_ANALOG_INPUTS];
int oldValue = 0;

/* digital input ports */
byte reportPINs[TOTAL_PORTS];       // 1 = report this port, 0 = silence
byte previousPINs[TOTAL_PORTS];     // previous 8 bits sent

/* pins configuration */
byte pinConfig[TOTAL_PINS];         // configuration of every pin
byte portConfigInputs[TOTAL_PORTS]; // each bit: 1 = pin in INPUT, 0 = anything else
int pinState[TOTAL_PINS];           // any value that has been written

/* timer variables */
unsigned long currentMillis;        // store the current value from millis()
unsigned long previousMillis;       // for comparison with currentMillis
int samplingInterval = 200;          // how often to run the main loop (in ms)
#define MINIMUM_SAMPLE_DELAY 150
#define ANALOG_SAMPLE_DELAY 50


/* i2c data */
struct i2c_device_info {
  byte addr;
  byte reg;
  byte bytes;
};

/* for i2c read continuous more */
i2c_device_info query[MAX_QUERIES];

byte i2cRxData[32];
boolean isI2CEnabled = false;
signed char queryIndex = -1;
unsigned int i2cReadDelayTime = 0;  // default delay time between i2c read request and Wire.requestFrom()

Servo servos[MAX_SERVOS];

/*==============================================================================
 * FUNCTIONS -  CSL:Add'l StandardFirmata Includes for I/O pins
 *============================================================================*/

void readAndReportData(byte address, int theRegister, byte numBytes) {
  // allow I2C requests that don't require a register read
  // for example, some devices using an interrupt pin to signify new data available
  // do not always require the register read so upon interrupt you call Wire.requestFrom()  
  if (theRegister != REGISTER_NOT_SPECIFIED) {
    Wire.beginTransmission(address);
    #if ARDUINO >= 100
    Wire.write((byte)theRegister);
    #else
    Wire.send((byte)theRegister);
    #endif
    Wire.endTransmission();
    delayMicroseconds(i2cReadDelayTime);  // delay is necessary for some devices such as WiiNunchuck
  } else {
    theRegister = 0;  // fill the register with a dummy value
  }

  Wire.requestFrom(address, numBytes);  // all bytes are returned in requestFrom

  // check to be sure correct number of bytes were returned by slave
  if(numBytes == Wire.available()) {
    i2cRxData[0] = address;
    i2cRxData[1] = theRegister;
    for (int i = 0; i < numBytes; i++) {
      #if ARDUINO >= 100
      i2cRxData[2 + i] = Wire.read();
      #else
      i2cRxData[2 + i] = Wire.receive();
      #endif
    }
  }
  else {
    if(numBytes > Wire.available()) {
      BLE_Firmata.sendString("I2C Read Error: Too many bytes received");
    } else {
      BLE_Firmata.sendString("I2C Read Error: Too few bytes received"); 
    }
  }

  // send slave address, register and received bytes
  BLE_Firmata.sendSysex(SYSEX_I2C_REPLY, numBytes + 2, i2cRxData);
}

void outputPort(byte portNumber, byte portValue, byte forceSend)
{
  // pins not configured as INPUT are cleared to zeros
  portValue = portValue & portConfigInputs[portNumber];
  // only send if the value is different than previously sent
  if(forceSend || previousPINs[portNumber] != portValue) {
    Serial.print(F("Sending update for port ")); Serial.print(portNumber); Serial.print(" = 0x"); Serial.println(portValue, HEX);
    BLE_Firmata.sendDigitalPort(portNumber, portValue);
    previousPINs[portNumber] = portValue;
  }
}

/* -----------------------------------------------------------------------------
 * check all the active digital inputs for change of state, then add any events
 * to the Serial output queue using Serial.print() */
void checkDigitalInputs(boolean forceSend = false)
{
  /* Using non-looping code allows constants to be given to readPort().
   * The compiler will apply substantial optimizations if the inputs
   * to readPort() are compile-time constants. */
  for (uint8_t i=0; i<TOTAL_PORTS; i++) {
    if (reportPINs[i]) {
     // Serial.print("Reporting on port "); Serial.print(i); Serial.print(" mask 0x"); Serial.println(portConfigInputs[i], HEX);
      uint8_t x = readPort(i, portConfigInputs[i]);
     // Serial.print("Read 0x"); Serial.println(x, HEX);
      outputPort(i, x, forceSend);
    }
  }
}

// -----------------------------------------------------------------------------
/* sets the pin mode to the correct state and sets the relevant bits in the
 * two bit-arrays that track Digital I/O and PWM status
 */
void setPinModeCallback(byte pin, int mode)
{
  if ((pinConfig[pin] == I2C) && (isI2CEnabled) && (mode != I2C)) {
    // disable i2c so pins can be used for other functions
    // the following if statements should reconfigure the pins properly
    disableI2CPins();
  }
  if (IS_PIN_SERVO(pin) && mode != SERVO && servos[PIN_TO_SERVO(pin)].attached()) {
    servos[PIN_TO_SERVO(pin)].detach();
  }
  if (IS_PIN_ANALOG(pin)) {
    reportAnalogCallback(PIN_TO_ANALOG(pin), mode == ANALOG ? 1 : 0); // turn on/off reporting
  }
  if (IS_PIN_DIGITAL(pin)) {
    if (mode == INPUT) {
      portConfigInputs[pin/8] |= (1 << (pin & 7));
    } else {
      portConfigInputs[pin/8] &= ~(1 << (pin & 7));
    }
   // Serial.print(F("Setting pin #")); Serial.print(pin); Serial.print(F(" port config mask to = 0x")); 
   // Serial.println(portConfigInputs[pin/8], HEX);
  }
  pinState[pin] = 0;
  switch(mode) {
  case ANALOG:
    if (IS_PIN_ANALOG(pin)) {
      Serial.print(F("Set pin #")); Serial.print(pin); Serial.println(F(" to analog"));
      if (IS_PIN_DIGITAL(pin)) {
        pinMode(PIN_TO_DIGITAL(pin), INPUT); // disable output driver
        digitalWrite(PIN_TO_DIGITAL(pin), LOW); // disable internal pull-ups
      }
      pinConfig[pin] = ANALOG;
      lastAnalogReads[PIN_TO_ANALOG(pin)] = -1;
    }
    break;
  case INPUT:
    if (IS_PIN_DIGITAL(pin)) {
      Serial.print(F("Set pin #")); Serial.print(pin); Serial.println(F(" to input"));
      pinMode(PIN_TO_DIGITAL(pin), INPUT); // disable output driver
      if (AUTO_INPUT_PULLUPS) {
        digitalWrite(PIN_TO_DIGITAL(pin), HIGH); // enable internal pull-ups
      } else {
        digitalWrite(PIN_TO_DIGITAL(pin), LOW); // disable internal pull-ups
      }
      pinConfig[pin] = INPUT;
      
      // force sending state immediately
      //delay(10);
      //checkDigitalInputs(true);  
    }
    break;
  case OUTPUT:
    if (IS_PIN_DIGITAL(pin)) {
      Serial.print(F("Set pin #")); Serial.print(pin); Serial.println(F(" to output"));
      digitalWrite(PIN_TO_DIGITAL(pin), LOW); // disable PWM
      pinMode(PIN_TO_DIGITAL(pin), OUTPUT);
      pinConfig[pin] = OUTPUT;
    }
    break;
  case PWM:
    if (IS_PIN_PWM(pin)) {
      pinMode(PIN_TO_PWM(pin), OUTPUT);
      analogWrite(PIN_TO_PWM(pin), 0);
      pinConfig[pin] = PWM;
    }
    break;
  case SERVO:
    if (IS_PIN_SERVO(pin)) {
      pinConfig[pin] = SERVO;
      if (!servos[PIN_TO_SERVO(pin)].attached()) {
          servos[PIN_TO_SERVO(pin)].attach(PIN_TO_DIGITAL(pin));
      }
    }
    break;
  case I2C:
    if (IS_PIN_I2C(pin)) {
      // mark the pin as i2c
      // the user must call I2C_CONFIG to enable I2C for a device
      pinConfig[pin] = I2C;
    }
    break;
  default:
    Serial.print(F("Unknown pin mode")); // TODO: put error msgs in EEPROM
  }
  // TODO: save status to EEPROM here, if changed
}

void analogWriteCallback(byte pin, int value)
{
  if (pin < TOTAL_PINS) {
    switch(pinConfig[pin]) {
    case SERVO:
      if (IS_PIN_SERVO(pin))
        servos[PIN_TO_SERVO(pin)].write(value);
        pinState[pin] = value;
      break;
    case PWM:
      if (IS_PIN_PWM(pin))
        analogWrite(PIN_TO_PWM(pin), value);
        pinState[pin] = value;
      break;
    }
  }
}

void digitalWriteCallback(byte port, int value)
{
  byte pin, lastPin, mask=1, pinWriteMask=0;

  if (port < TOTAL_PORTS) {
    // create a mask of the pins on this port that are writable.
    lastPin = port*8+8;
    if (lastPin > TOTAL_PINS) lastPin = TOTAL_PINS;
    for (pin=port*8; pin < lastPin; pin++) {
      // do not disturb non-digital pins (eg, Rx & Tx)
      if (IS_PIN_DIGITAL(pin)) {
        // only write to OUTPUT and INPUT (enables pullup)
        // do not touch pins in PWM, ANALOG, SERVO or other modes
        if (pinConfig[pin] == OUTPUT || pinConfig[pin] == INPUT) {
          pinWriteMask |= mask;
          pinState[pin] = ((byte)value & mask) ? 1 : 0;
          
          if (AUTO_INPUT_PULLUPS && ( pinConfig[pin] == INPUT)) {
            value |= mask;
          }
        }
      }
      mask = mask << 1;
    }
    Serial.print(F("Write digital port #")); Serial.print(port); 
    Serial.print(F(" = 0x")); Serial.print(value, HEX);
    Serial.print(F(" mask = 0x")); Serial.println(pinWriteMask, HEX);
    writePort(port, (byte)value, pinWriteMask);
  }
}


// -----------------------------------------------------------------------------
/* sets bits in a bit array (int) to toggle the reporting of the analogIns
 */
//void FirmataClass::setAnalogPinReporting(byte pin, byte state) {
//}
void reportAnalogCallback(byte analogPin, int value)
{
  if (analogPin < TOTAL_ANALOG_PINS) {
    if(value == 0) {
      analogInputsToReport = analogInputsToReport &~ (1 << analogPin);
      Serial.print(F("Stop reporting analog pin #")); Serial.println(analogPin);
    } else {
      analogInputsToReport |= (1 << analogPin);
      Serial.print(F("Will report analog pin #")); Serial.println(analogPin);
    }
  }
  // TODO: save status to EEPROM here, if changed
}

void reportDigitalCallback(byte port, int value)
{
  if (port < TOTAL_PORTS) {
    Serial.print(F("Will report 0x")); Serial.print(value, HEX); Serial.print(F(" digital mask on port ")); Serial.println(port);
    reportPINs[port] = (byte)value;
  }
  // do not disable analog reporting on these 8 pins, to allow some
  // pins used for digital, others analog.  Instead, allow both types
  // of reporting to be enabled, but check if the pin is configured
  // as analog when sampling the analog inputs.  Likewise, while
  // scanning digital pins, portConfigInputs will mask off values from any
  // pins configured as analog
}

/*==============================================================================
 * SYSEX-BASED commands
 *============================================================================*/

void sysexCallback(byte command, byte argc, byte *argv)
{
  byte mode;
  byte slaveAddress;
  byte slaveRegister;
  byte data;
  unsigned int delayTime; 
  
  switch(command) {
  case I2C_REQUEST:
    mode = argv[1] & I2C_READ_WRITE_MODE_MASK;
    if (argv[1] & I2C_10BIT_ADDRESS_MODE_MASK) {
      //BLE_Firmata.sendString("10-bit addressing mode is not yet supported");
      Serial.println(F("10-bit addressing mode is not yet supported"));
      return;
    }
    else {
      slaveAddress = argv[0];
    }

    switch(mode) {
    case I2C_WRITE:
      Wire.beginTransmission(slaveAddress);
      for (byte i = 2; i < argc; i += 2) {
        data = argv[i] + (argv[i + 1] << 7);
        #if ARDUINO >= 100
        Wire.write(data);
        #else
        Wire.send(data);
        #endif
      }
      Wire.endTransmission();
      delayMicroseconds(70);
      break;
    case I2C_READ:
      if (argc == 6) {
        // a slave register is specified
        slaveRegister = argv[2] + (argv[3] << 7);
        data = argv[4] + (argv[5] << 7);  // bytes to read
        readAndReportData(slaveAddress, (int)slaveRegister, data);
      }
      else {
        // a slave register is NOT specified
        data = argv[2] + (argv[3] << 7);  // bytes to read
        readAndReportData(slaveAddress, (int)REGISTER_NOT_SPECIFIED, data);
      }
      break;
    case I2C_READ_CONTINUOUSLY:
      if ((queryIndex + 1) >= MAX_QUERIES) {
        // too many queries, just ignore
        BLE_Firmata.sendString("too many queries");
        break;
      }
      queryIndex++;
      query[queryIndex].addr = slaveAddress;
      query[queryIndex].reg = argv[2] + (argv[3] << 7);
      query[queryIndex].bytes = argv[4] + (argv[5] << 7);
      break;
    case I2C_STOP_READING:
	  byte queryIndexToSkip;      
      // if read continuous mode is enabled for only 1 i2c device, disable
      // read continuous reporting for that device
      if (queryIndex <= 0) {
        queryIndex = -1;        
      } else {
        // if read continuous mode is enabled for multiple devices,
        // determine which device to stop reading and remove it's data from
        // the array, shifiting other array data to fill the space
        for (byte i = 0; i < queryIndex + 1; i++) {
          if (query[i].addr = slaveAddress) {
            queryIndexToSkip = i;
            break;
          }
        }
        
        for (byte i = queryIndexToSkip; i<queryIndex + 1; i++) {
          if (i < MAX_QUERIES) {
            query[i].addr = query[i+1].addr;
            query[i].reg = query[i+1].addr;
            query[i].bytes = query[i+1].bytes; 
          }
        }
        queryIndex--;
      }
      break;
    default:
      break;
    }
    break;
  case I2C_CONFIG:
    delayTime = (argv[0] + (argv[1] << 7));

    if(delayTime > 0) {
      i2cReadDelayTime = delayTime;
    }

    if (!isI2CEnabled) {
      enableI2CPins();
    }
    
    break;
  case SERVO_CONFIG:
    if(argc > 4) {
      // these vars are here for clarity, they'll optimized away by the compiler
      byte pin = argv[0];
      int minPulse = argv[1] + (argv[2] << 7);
      int maxPulse = argv[3] + (argv[4] << 7);

      if (IS_PIN_SERVO(pin)) {
        if (servos[PIN_TO_SERVO(pin)].attached())
          servos[PIN_TO_SERVO(pin)].detach();
        servos[PIN_TO_SERVO(pin)].attach(PIN_TO_DIGITAL(pin), minPulse, maxPulse);
        setPinModeCallback(pin, SERVO);
      }
    }
    break;
  case SAMPLING_INTERVAL:
    if (argc > 1) {
      samplingInterval = argv[0] + (argv[1] << 7);
      if (samplingInterval < MINIMUM_SAMPLING_INTERVAL) {
        samplingInterval = MINIMUM_SAMPLING_INTERVAL;
      }      
    } else {
      //BLE_Firmata.sendString("Not enough data");
    }
    break;
  case EXTENDED_ANALOG:
    if (argc > 1) {
      int val = argv[1];
      if (argc > 2) val |= (argv[2] << 7);
      if (argc > 3) val |= (argv[3] << 14);
      analogWriteCallback(argv[0], val);
    }
    break;
  case CAPABILITY_QUERY:
    Serial.write(START_SYSEX);
    Serial.write(CAPABILITY_RESPONSE);
    for (byte pin=0; pin < TOTAL_PINS; pin++) {
      if (IS_PIN_DIGITAL(pin)) {
        Serial.write((byte)INPUT);
        Serial.write(1);
        Serial.write((byte)OUTPUT);
        Serial.write(1);
      }
      if (IS_PIN_ANALOG(pin)) {
        Serial.write(ANALOG);
        Serial.write(10);
      }
      if (IS_PIN_PWM(pin)) {
        Serial.write(PWM);
        Serial.write(8);
      }
      if (IS_PIN_SERVO(pin)) {
        Serial.write(SERVO);
        Serial.write(14);
      }
      if (IS_PIN_I2C(pin)) {
        Serial.write(I2C);
        Serial.write(1);  // to do: determine appropriate value 
      }
      Serial.write(127);
    }
    Serial.write(END_SYSEX);
    break;
  case PIN_STATE_QUERY:
    if (argc > 0) {
      byte pin=argv[0];
      Serial.write(START_SYSEX);
      Serial.write(PIN_STATE_RESPONSE);
      Serial.write(pin);
      if (pin < TOTAL_PINS) {
        Serial.write((byte)pinConfig[pin]);
	Serial.write((byte)pinState[pin] & 0x7F);
	if (pinState[pin] & 0xFF80) Serial.write((byte)(pinState[pin] >> 7) & 0x7F);
	if (pinState[pin] & 0xC000) Serial.write((byte)(pinState[pin] >> 14) & 0x7F);
      }
      Serial.write(END_SYSEX);
    }
    break;
  case ANALOG_MAPPING_QUERY:
    Serial.write(START_SYSEX);
    Serial.write(ANALOG_MAPPING_RESPONSE);
    for (byte pin=0; pin < TOTAL_PINS; pin++) {
      Serial.write(IS_PIN_ANALOG(pin) ? PIN_TO_ANALOG(pin) : 127);
    }
    Serial.write(END_SYSEX);
    break;
  }
}

void enableI2CPins()
{
  byte i;
  // is there a faster way to do this? would probaby require importing 
  // Arduino.h to get SCL and SDA pins
  for (i=0; i < TOTAL_PINS; i++) {
    if(IS_PIN_I2C(i)) {
      // mark pins as i2c so they are ignore in non i2c data requests
      setPinModeCallback(i, I2C);
    } 
  }
   
  isI2CEnabled = true; 
  
  // is there enough time before the first I2C request to call this here?
  Wire.begin();
}

/* disable the i2c pins so they can be used for other functions */
void disableI2CPins() {
    isI2CEnabled = false;
    // disable read continuous mode for all devices
    queryIndex = -1;
    // uncomment the following if or when the end() method is added to Wire library
    // Wire.end();
}

/*==============================================================================
 * SETUP()
 *============================================================================*/

void systemResetCallback()
{
  // initialize a defalt state
  Serial.println(F("***RESET***"));
  // TODO: option to load config from EEPROM instead of default
  if (isI2CEnabled) {
  	disableI2CPins();
  }
  for (byte i=0; i < TOTAL_PORTS; i++) {
    reportPINs[i] = false;      // by default, reporting off
    portConfigInputs[i] = 0;	// until activated
    previousPINs[i] = 0;
  }
  // pins with analog capability default to analog input
  // otherwise, pins default to digital output
  for (byte i=0; i < TOTAL_PINS; i++) {
    if (IS_PIN_ANALOG(i)) {
      // turns off pullup, configures everything
      setPinModeCallback(i, ANALOG);
    } else {
      // sets the output to 0, configures portConfigInputs
      setPinModeCallback(i, INPUT);
    }
  }
  // by default, do not report any analog inputs
  analogInputsToReport = 0;

  /* send digital inputs to set the initial state on the host computer,
   * since once in the loop(), this firmware will only send on change */
  /*
  TODO: this can never execute, since no pins default to digital input
        but it will be needed when/if we support EEPROM stored config
  for (byte i=0; i < TOTAL_PORTS; i++) {
    outputPort(i, readPort(i, portConfigInputs[i]), true);
  }
  */
}


void firmataInit() {
  Serial.println(F("Init firmata"));
  //BLE_Firmata.setFirmwareVersion(FIRMATA_MAJOR_VERSION, FIRMATA_MINOR_VERSION);
  //Serial.println(F("firmata analog"));
  BLE_Firmata.attach(ANALOG_MESSAGE, analogWriteCallback);
  //Serial.println(F("firmata digital"));
  BLE_Firmata.attach(DIGITAL_MESSAGE, digitalWriteCallback);
  //Serial.println(F("firmata analog report"));
  BLE_Firmata.attach(REPORT_ANALOG, reportAnalogCallback);
  //Serial.println(F("firmata digital report"));
  BLE_Firmata.attach(REPORT_DIGITAL, reportDigitalCallback);
  //Serial.println(F("firmata pinmode"));
  BLE_Firmata.attach(SET_PIN_MODE, setPinModeCallback);
  //Serial.println(F("firmata sysex"));
  BLE_Firmata.attach(START_SYSEX, sysexCallback);
  //Serial.println(F("firmata reset"));
  BLE_Firmata.attach(SYSTEM_RESET, systemResetCallback);

  Serial.println(F("Begin firmata"));
  BLE_Firmata.begin();
  systemResetCallback();  // reset to default config 
}

aci_evt_opcode_t lastBTLEstatus, BTLEstatus;

//  -  CSL:ACTUAL BEGIN OF PGM LOGIC!! 

/**************************************************************************/
/*!
    Configure the Arduino and start advertising with the radio
*/
/**************************************************************************/
void setup(void)
{ 
  Serial.begin(9600);
  while(!Serial); // Leonardo/Micro should wait for serial init
  Serial.println(F("Adafruit Bluefruit Low Energy nRF8001 Print BLEanalog5"));

  // BLEserial.setDeviceName("NEWNAME"); /* 7 characters max! */

  BLEserial.begin();
  
  BTLEstatus = lastBTLEstatus = ACI_EVT_DISCONNECTED;   // CSL:Add'l StandardFirmata Includes for I/O pins
  
}

/**************************************************************************/
/*!
    Constantly checks for new events on the nRF8001
*/
/**************************************************************************/
aci_evt_opcode_t laststatus = ACI_EVT_DISCONNECTED;

/*==============================================================================
 * LOOP()  CSL:Add'l StandardFirmata Includes for I/O pins here/there as documented
 *============================================================================*/

void loop()
{
  // Tell the nRF8001 to do whatever it should be working on.
  BLEserial.pollACI();

  // Ask what is our current status
  aci_evt_opcode_t status = BLEserial.getState();
  // If the status changed....
  if (status != laststatus) {
    // print it out!
    if (status == ACI_EVT_DEVICE_STARTED) {
        Serial.println(F("* Advertising started"));
    }
    if (status == ACI_EVT_CONNECTED) {
        Serial.println(F("* Connected!"));
    }
    if (status == ACI_EVT_DISCONNECTED) {
        Serial.println(F("* Disconnected or advertising timed out"));
    }
    // OK set the last status change to this one
    laststatus = status;
  } 

  if (status == ACI_EVT_CONNECTED) {
    // Lets see if there's any data for us!
    if (BLEserial.available()) {
      Serial.print("* "); Serial.print(BLEserial.available()); Serial.println(F(" bytes available from BTLE"));
    }
    // OK while we still have something to read, get a character and print it out
    while (BLEserial.available()) {
      char c = BLEserial.read();
      Serial.print(c);
    }

    // Next up, see if we have any data to get from the Serial console

    if (Serial.available()) {
      // Read a line from Serial
      Serial.setTimeout(100); // 100 millisecond timeout
      String s = Serial.readString();

      // We need to convert the line to bytes, no more than 20 at this time
      uint8_t sendbuffer[20];
      s.getBytes(sendbuffer, 20);
      char sendbuffersize = min(20, s.length());

      Serial.print(F("\n* Sending -> \"")); Serial.print((char *)sendbuffer); Serial.println("\"");

      // write the data
      BLEserial.write(sendbuffer, sendbuffersize);
    }
    
    // CSL: MAIN LOOP part of I/O port monitoring code starts HERE!!
    
    // Onto the Firmata main loop
  // make the sampling interval longer if we have more analog inputs!
  uint8_t analogreportnums = 0;
  for(uint8_t a=0; a<8; a++) {
    if (analogInputsToReport & (1 << a)) {
      analogreportnums++;
    }
  }

  samplingInterval = (uint16_t)MINIMUM_SAMPLE_DELAY  + (uint16_t)ANALOG_SAMPLE_DELAY * (1+analogreportnums); 
  
  currentMillis = millis();
  if (currentMillis - previousMillis > samplingInterval) {
      previousMillis += samplingInterval;
      /* ANALOGREAD - do all analogReads() at the configured sampling interval */
    
      byte analogPin;      // CSL: These refer to the absolute I/O 'pin' value (0-19) and the equiv.'analogPin' (0-5) only I/O port numbers
    
      // CSL: Just look for analogPin = 5 (equates to absolute I/O 'pin = 19')
 
      analogPin = PIN_TO_ANALOG(19);
      int currentRead = analogRead(analogPin);                // CSL: Get Analog Value for AnalogPin
      if (currentRead != oldValue) {                          // CSL: If Value didn't change, no need to report same value again..
        String currentAnalog= String(currentRead);            // CSL: Convert (int) currentRead to a String so it can be passed back to Bluetooth Serial
    
        // We need to convert the number to byte characters, no more than 20 at this time.  CSL: Same must be done for the analogPin Value...
        uint8_t sendbufferAnalogData[20];
        currentAnalog.getBytes(sendbufferAnalogData, 20);
    
        char sendbufferAnalogSize = min(20, currentAnalog.length()); // CSL: Calculate/truncate answer to 20 chars or less

//      Serial.print(F("currentRead=")); Serial.println(currentRead); 
//      Serial.print(F("currentAnalog=")); Serial.println(currentAnalog); 
//      Serial.print(F("sendbufferAnalogData=")); Serial.println((char *)sendbufferAnalogData);

        Serial.print((char *)sendbufferAnalogData); Serial.print(F(" = ")); Serial.print(F("Char Size=")); Serial.println(sendbufferAnalogSize); 

        BLEserial.write(sendbufferAnalogData, sendbufferAnalogSize);
      
        oldValue = currentRead;                               // CSL: Save oldValue as currentRead so no need to resend if value didn't change
      }
//      else
//          Serial.print(F(" No Change. ")); Serial.println(currentRead); 
    }
  }
}
