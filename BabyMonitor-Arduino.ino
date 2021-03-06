/* ============================================
BGLib Arduino interface library code is placed under the MIT license
Copyright (c) 2014 Jeff Rowberg

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
===============================================
*/

#include <SoftwareSerial.h>
#include "BGLib.h"
#include "LowPower.h"

#include <OneWire.h>
#include <DallasTemperature.h>

// accelerometer includes
#include <Wire.h> // Must include Wire library for I2C
#include <SFE_MMA8452Q.h> // Includes the SFE_MMA8452Q library
MMA8452Q accel;
float accelData[3];

// uncomment the following line for debug serial output
#define DEBUG

// ================================================================
// BLE STATE TRACKING (UNIVERSAL TO JUST ABOUT ANY BLE PROJECT)
// ================================================================

// BLE state machine definitions
#define BLE_STATE_STANDBY           0
#define BLE_STATE_SCANNING          1
#define BLE_STATE_ADVERTISING       2
#define BLE_STATE_CONNECTING        3
#define BLE_STATE_CONNECTED_MASTER  4
#define BLE_STATE_CONNECTED_SLAVE   5

// BLE state/link status tracker
uint8_t ble_state = BLE_STATE_STANDBY;
uint8_t ble_encrypted = 0;  // 0 = not encrypted, otherwise = encrypted
uint8_t ble_bonding = 0xFF; // 0xFF = no bonding, otherwise = bonding handle

// ================================================================
// HARDWARE CONNECTIONS AND GATT STRUCTURE SETUP
// ================================================================

// NOTE: this assumes you are using one of the following firmwares:
//  - BGLib_U1A1P_38400_noflow
//  - BGLib_U1A1P_38400_noflow_wake16
//  - BGLib_U1A1P_38400_noflow_wake16_hwake15
// If not, then you may need to change the pin assignments and/or
// GATT handles to match your firmware.

#define LED_PIN         13  // Arduino Uno LED pin
#define BLE_WAKEUP_PIN  5   // BLE wake-up pin
#define BLE_RESET_PIN   6   // BLE reset pin (active-low)

#define GATT_HANDLE_ACCELEROMETER        17
#define GATT_HANDLE_ORAL_THERMOMETER     21
#define GATT_HANDLE_SURFACE_THERMOMETER  25

#define ONE_WIRE_BUS 3
// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);
// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);

#define SURFACE_THERMOMETER_PIN  A1

// use SoftwareSerial on pins D2/D3 for RX/TX (Arduino side)
SoftwareSerial bleSerialPort(4, 7);

// create BGLib object:
//  - use SoftwareSerial port for module comms
//  - use nothing for passthrough comms (0 = null pointer)
//  - enable packet mode on API protocol since flow control is unavailable
BGLib ble112((HardwareSerial *)&bleSerialPort, 0, 1);
//BGLib ble112((HardwareSerial *)&Serial, 0, 1);

//#define BGAPI_GET_RESPONSE(v, dType) dType *v = (dType *)ble112.getLastRXPayload()

boolean booted = false;
float reading1;
float reading2;
uint16_t slice;
unsigned long wakeUpTime = millis();

// ================================================================
// ARDUINO APPLICATION SETUP AND LOOP FUNCTIONS
// ================================================================

// initialization sequence
void setup() {
    // initialize status LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // initialize BLE wake-up pin to allow (not force) sleep mode (assumes active-high)
    pinMode(BLE_WAKEUP_PIN, OUTPUT);
    digitalWrite(BLE_WAKEUP_PIN, LOW);

    // set up internal status handlers
    ble112.onTimeout = onTimeout;

    // ONLY enable these if you are using the <wakeup_pin> parameter in your firmware's hardware.xml file
    // BLE module must be woken up before sending any UART data
    ble112.onBeforeTXCommand = onBeforeTXCommand;
    ble112.onTXCommandComplete = onTXCommandComplete;

    // set up BGLib event handlers
    ble112.ble_evt_system_boot = my_ble_evt_system_boot;
    ble112.ble_evt_connection_status = my_ble_evt_connection_status;
    ble112.ble_evt_connection_disconnected = my_ble_evt_connection_disconnect;
    ble112.ble_evt_attributes_value = my_ble_evt_attributes_value;
    ble112.ble_evt_attributes_user_read_request = my_ble_evt_attributes_user_read_request;

    // open Arduino USB serial
    #ifdef DEBUG
        Serial.begin(19200);
    #endif

    // open BLE software serial port
    bleSerialPort.begin(19200);
    
    // initialize BLE reset pin (active-low)
    pinMode(BLE_RESET_PIN, OUTPUT);
    bleReset();

    pinMode(8, OUTPUT);
    digitalWrite(8, LOW);
    
    pinMode(12, OUTPUT);
    
    // configure wakeup pin
    pinMode(2, INPUT_PULLUP);
    
    accel.init();
    
    sensors.begin();
}

// main application loop
void loop() {
    // keep polling for new data from BLE
    ble112.checkActivity();
    
    if (booted && millis() > wakeUpTime + 50) sleep();
}

void sleep() {
    #ifdef DEBUG
        Serial.println("Going to sleep");
        Serial.println();
        // finish sending any queued serial messages
        Serial.flush();
    #endif
    
    // wait for interrupt pin to reset to normal state
    while (digitalRead(2) == LOW);
    
    // power down components
    digitalWrite(LED_PIN, LOW);
    
    // set flow control CTS -> HIGH to prevent incoming data
    digitalWrite(8, HIGH);
    
    // power down with wakeup interrupt
    attachInterrupt(0, wakeUp, LOW);
    LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
}

void wakeUp() {
    #ifdef DEBUG
        Serial.println("Waking up");
    #endif
    
    // remove interrupt
    detachInterrupt(0);
    
    // set CTS -> LOW to accept incoming data
    digitalWrite(8, LOW);
    
    // set time reference
    wakeUpTime = millis();
}


// Toggle reset pin to reset BLE112 chip
void bleReset() {
    digitalWrite(BLE_RESET_PIN, LOW);
    delay(5); // wait 5ms
    digitalWrite(BLE_RESET_PIN, HIGH);
}

void onTimeout() {
    #ifdef DEBUG
        Serial.println("!!!\tTimeout occurred!");
    #endif
}

// called immediately before beginning UART TX of a command
void onBeforeTXCommand() {
    // wake module up
    digitalWrite(BLE_WAKEUP_PIN, HIGH);

    // wait for "hardware_io_port_status" event to come through, and parse it (and otherwise ignore it)
    uint8_t *last;
    while (1) {
        ble112.checkActivity();
        last = ble112.getLastEvent();
        if (last[0] == 0x07 && last[1] == 0x00) break;
    }

    // give a bit of a gap between parsing the wake-up event and allowing the command to go out
    delayMicroseconds(50000); // increased to 50ms to make more reliable
}

// called immediately after finishing UART TX
void onTXCommandComplete() {
    // allow module to return to sleep
    digitalWrite(BLE_WAKEUP_PIN, LOW);
}



// ================================================================
// APPLICATION EVENT HANDLER FUNCTIONS
// ================================================================

void my_ble_evt_system_boot(const ble_msg_system_boot_evt_t *msg) {
    #ifdef DEBUG
        Serial.print("###\tsystem_boot: { ");
        Serial.print("major: "); Serial.print(msg -> major, HEX);
        Serial.print(", minor: "); Serial.print(msg -> minor, HEX);
        Serial.print(", patch: "); Serial.print(msg -> patch, HEX);
        Serial.print(", build: "); Serial.print(msg -> build, HEX);
        Serial.print(", ll_version: "); Serial.print(msg -> ll_version, HEX);
        Serial.print(", protocol_version: "); Serial.print(msg -> protocol_version, HEX);
        Serial.print(", hw: "); Serial.print(msg -> hw, HEX);
        Serial.println(" }");
    #endif

    // system boot means module is in standby state
    //ble_state = BLE_STATE_STANDBY;
    // ^^^ skip above since we're going right back into advertising below

    // set advertisement interval to 200-300ms, use all advertisement channels
    // (note min/max parameters are in units of 625 uSec)
    ble112.ble_cmd_gap_set_adv_parameters(320, 480, 7);
    while (ble112.checkActivity(1000));

    // USE THE FOLLOWING TO LET THE BLE STACK HANDLE YOUR ADVERTISEMENT PACKETS
    // ========================================================================
    // start advertising general discoverable / undirected connectable
    ble112.ble_cmd_gap_set_mode(BGLIB_GAP_GENERAL_DISCOVERABLE, BGLIB_GAP_UNDIRECTED_CONNECTABLE);
    while (ble112.checkActivity(1000));
}

void my_ble_evt_connection_status(const ble_msg_connection_status_evt_t *msg) {
    #ifdef DEBUG
        Serial.print("###\tconnection_status: { ");
        Serial.print("connection: "); Serial.print(msg -> connection, HEX);
        Serial.print(", flags: "); Serial.print(msg -> flags, HEX);
        Serial.print(", address: ");
        // this is a "bd_addr" data type, which is a 6-byte uint8_t array
        for (uint8_t i = 0; i < 6; i++) {
            if (msg -> address.addr[i] < 16) Serial.write('0');
            Serial.print(msg -> address.addr[i], HEX);
        }
        Serial.print(", address_type: "); Serial.print(msg -> address_type, HEX);
        Serial.print(", conn_interval: "); Serial.print(msg -> conn_interval, HEX);
        Serial.print(", timeout: "); Serial.print(msg -> timeout, HEX);
        Serial.print(", latency: "); Serial.print(msg -> latency, HEX);
        Serial.print(", bonding: "); Serial.print(msg -> bonding, HEX);
        Serial.println(" }");
    #endif

    // "flags" bit description:
    //  - bit 0: connection_connected
    //           Indicates the connection exists to a remote device.
    //  - bit 1: connection_encrypted
    //           Indicates the connection is encrypted.
    //  - bit 2: connection_completed
    //           Indicates that a new connection has been created.
    //  - bit 3; connection_parameters_change
    //           Indicates that connection parameters have changed, and is set
    //           when parameters change due to a link layer operation.

    // check for new connection established
    if ((msg -> flags & 0x05) == 0x05) {
        // track state change based on last known state, since we can connect two ways
        if (ble_state == BLE_STATE_ADVERTISING) {
            ble_state = BLE_STATE_CONNECTED_SLAVE;
        } else {
            ble_state = BLE_STATE_CONNECTED_MASTER;
        }
    }

    // update "encrypted" status
    ble_encrypted = msg -> flags & 0x02;
    
    // update "bonded" status
    ble_bonding = msg -> bonding;
}

void my_ble_evt_connection_disconnect(const struct ble_msg_connection_disconnected_evt_t *msg) {
    #ifdef DEBUG
        Serial.print("###\tconnection_disconnect: { ");
        Serial.print("connection: "); Serial.print(msg -> connection, HEX);
        Serial.print(", reason: "); Serial.print(msg -> reason, HEX);
        Serial.println(" }");
    #endif

    // set state to DISCONNECTED
    //ble_state = BLE_STATE_DISCONNECTED;
    // ^^^ skip above since we're going right back into advertising below

    // after disconnection, resume advertising as discoverable/connectable
    //ble112.ble_cmd_gap_set_mode(BGLIB_GAP_GENERAL_DISCOVERABLE, BGLIB_GAP_UNDIRECTED_CONNECTABLE);
    //while (ble112.checkActivity(1000));

    // after disconnection, resume advertising as discoverable/connectable (with user-defined advertisement data)
    ble112.ble_cmd_gap_set_mode(BGLIB_GAP_USER_DATA, BGLIB_GAP_UNDIRECTED_CONNECTABLE);
    while (ble112.checkActivity(1000));

    // set state to ADVERTISING
    ble_state = BLE_STATE_ADVERTISING;

    // clear "encrypted" and "bonding" info
    ble_encrypted = 0;
    ble_bonding = 0xFF;
}

void my_ble_evt_attributes_value(const struct ble_msg_attributes_value_evt_t *msg) {
    #ifdef DEBUG
        Serial.print("###\tattributes_value: { ");
        Serial.print("connection: "); Serial.print(msg -> connection, HEX);
        Serial.print(", reason: "); Serial.print(msg -> reason, HEX);
        Serial.print(", handle: "); Serial.print(msg -> handle, HEX);
        Serial.print(", offset: "); Serial.print(msg -> offset, HEX);
        Serial.print(", value_len: "); Serial.print(msg -> value.len, HEX);
        Serial.print(", value_data: ");
        // this is a "uint8array" data type, which is a length byte and a uint8_t* pointer
        for (uint8_t i = 0; i < msg -> value.len; i++) {
            if (msg -> value.data[i] < 16) Serial.write('0');
            Serial.print(msg -> value.data[i], HEX);
        }
        Serial.println(" }");
    #endif
}

void my_ble_evt_attributes_user_read_request(const struct ble_msg_attributes_user_read_request_evt_t *msg) {
    switch (msg -> handle) {
        case GATT_HANDLE_ACCELEROMETER:
            if (accel.available()) {
                accel.read();
                accelData[0] = accel.cx;
                accelData[1] = accel.cy;
                accelData[2] = accel.cz;
            }
            #ifdef DEBUG
                Serial.print("Sending acceleration data...");
            #endif
            ble112.ble_cmd_attributes_user_read_response(msg -> connection, 0, 12, (uint8*) accelData);
            break;
        case GATT_HANDLE_ORAL_THERMOMETER:
            sensors.requestTemperatures();
            reading1 = sensors.getTempCByIndex(0);
            #ifdef DEBUG
                Serial.print("Sending oral thermometer data...");
            #endif
            ble112.ble_cmd_attributes_user_read_response(msg -> connection, 0, 4, (uint8*) &reading1);
            break;
        case GATT_HANDLE_SURFACE_THERMOMETER:
            reading2 = analogRead(SURFACE_THERMOMETER_PIN) / 1024.0;
            #ifdef DEBUG
                Serial.print("Sending surface thermometer data...");
            #endif
            ble112.ble_cmd_attributes_user_read_response(msg -> connection, 0, 4, (uint8*) &reading2);
            break;
    }
    while (ble112.checkActivity(1000));
    #ifdef DEBUG
        Serial.println("done");
    #endif
}

