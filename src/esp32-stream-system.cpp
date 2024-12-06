/**
 * esp32-stream-system.cpp
 * jul-sep 2024
 * 
 * AetherStream - Stream From the Beyond
 * Copyright (C)2024, Steven R Stuart
 * 
 * This program outputs icy/mp3 internet stream to max98357a i2c audio device(s).
 * Turn the knob to set volume. Press the button to change station.
 * Set volume to 0 then press button to set timer. Do again to cancel.
 * When device is powered up, an initial timer is set. (1 hour)
 * Two clicks within 3 seconds with zero volume halts stream.
 * Hold PORTAL_PIN low on reset to launch wifi configuration portal.
 * Hold STREAM_PIN low on reset to load and store default stream data.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "AudioTools.h" 
#include "AudioTools/AudioCodecs/CodecMP3Helix.h" 
#include "AiEsp32RotaryEncoder.h"
#include <Wire.h>
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"
#include <nvs_flash.h>

// Function prototypes
void saveParamsCallback(void);
void metadataCallback(MetaDataType, const char*, int);
void runSleepTimer(bool);
void oledStatusDisplay(void);
void StreamPortalMessage(void);
void WifiPortalMessage(void);
String timerTimeLeft(void);
void menuDisplay(int);
bool checkProtocol(int);
int settingGet(const char*);
void settingPut(const char*, int);
void populateStreams(void);
void populatePrefs(void);
void streamsPut(int, const char*, const char*);
char* streamsGetTag(int);
char* streamsGetUrl(int);
void initializeStreams(void);
String Version(void);
void WipeNVS(void);

// display I/O
#define I2C_ADDRESS 0x3C        // ssd1306 oled
#define SDA_PIN 18              // i2c data
#define SCL_PIN 19              // i2c clock

// user control inputs
#define NVS_CLR_PIN 17          // clear non-volatile memory when low on reset
#define STREAM_PIN 16           // set default streams when low on reset
#define PORTAL_PIN 15           // enable wifi portal when pulled low      

// portal states
#define PORTAL_DOWN 0           // portal is off
#define PORTAL_UP 1             // portal is running
#define PORTAL_SAVE 2           // save pending
#define PORTAL_IDLE 4           // waiting for switch reset

// timer settings
#define OLED_TIMER 5000         // display timeout in milliseconds
#define SLEEP_TIMER 3600000     // one hour in milliseconds

// System is hard coded to 25 streams (TOTAL_ITEMS)
// Each item in the streamsX array contains a text name and a url,
// the name and url elements are each STREAM_ELEMENT_SIZE in length
#define STREAM_ITEM_SIZE 100          // line item width
#define STREAM_ELEMENT_SIZE 50        // max length of name, url (49 char + nul)
#define TOTAL_ITEMS 36                // number of line items in streamsX
int currentIndex = 0;                 // stream pointer 
char streamsX[TOTAL_ITEMS * STREAM_ITEM_SIZE]; // names & urls of stations

// Preferences database
#define PREF_RO true                  // pref read-only
#define PREF_RW false                 // pref read/write
const char* portalName = "AetherStreamer"; // portal ssid
const char* settings = "settings";    // general purpose namespace in prefs
const char* listened = "listened";    // settings key of last listened to stream
const char* audiovol = "volume";      // settings key of audio level
const char* initPref = "initPref";    // key for initilization

// user control I/O
#define ROTARY_ENCODER_A_PIN 33       // clk
#define ROTARY_ENCODER_B_PIN 32       // dt  
#define ROTARY_ENCODER_BUTTON_PIN 34  // sw
#define ROTARY_ENCODER_STEPS 2        // 1, 2 or 4

AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(
    ROTARY_ENCODER_A_PIN,             // ky040 clk
    ROTARY_ENCODER_B_PIN,             //   "   dt
    ROTARY_ENCODER_BUTTON_PIN,        //   "   sw
    -1,                               // Vcc pin -1 = not used
    ROTARY_ENCODER_STEPS         
);

void IRAM_ATTR readEncoderISR() {     // interrupt service routine
    rotaryEncoder.readEncoder_ISR();
}

// Instatiate the objects
URLStream urlstream;                  // Use ICYStream if metadata is desired
//ICYStream urlstream;                // Use URLStream when metadata is not needed
I2SStream i2s;
VolumeStream volume(i2s);
EncodedAudioStream mp3decode(&volume, new MP3DecoderHelix()); // Decoder stream
StreamCopy copier(mp3decode, urlstream); // copy urlstream to decoder
SSD1306AsciiWire oled;
Preferences prefs;                    // persistent data store
WiFiManager wifiMan;                  // instatiate a wifi object

char stream_item[TOTAL_ITEMS][2] = {  // prefs stream item names
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", 
  "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", 
  "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"
};
char stream_type[2][4] = {"tag", "url"}; // prefs key names
#define TYPE_TAG 0                    // prefs type references
#define TYPE_URL 1  

// Must have enough unique element names for TOTAL_ITEMS
const char* tagElement[] = {          // portal html element tags
  "t0",  "t1",  "t2",  "t3",  "t4", 
  "t5",  "t6",  "t7",  "t8",  "t9", 
  "t10", "t11", "t12", "t13", "t14", 
  "t15", "t16", "t17", "t18", "t19",
  "t20", "t21", "t22", "t23", "t24",
  "t25", "t26", "t27", "t28", "t29", 
  "t30", "t31", "t32", "t33", "t34", 
  "t35", "t36"
};
const char* nameElement[] = {         // portal html tag field titles
  "Name 1",  "Name 2",  "Name 3",  "Name 4",  "Name 5",  
  "Name 6",  "Name 7",  "Name 8",  "Name 9",  "Name 10", 
  "Name 11", "Name 12", "Name 13", "Name 14", "Name 15", 
  "Name 16", "Name 17", "Name 18", "Name 19", "Name 20", 
  "Name 21", "Name 22", "Name 23", "Name 24", "Name 25",
  "Name 26", "Name 27", "Name 28", "Name 29", "Name 30",  
  "Name 31", "Name 32", "Name 33", "Name 34", "Name 35", 
  "Name 36"
};
const char* urlElement[] = {          // portal url element tags/titles
  "URL_1",  "URL_2",  "URL_3",  "URL_4",  "URL_5",  
  "URL_6",  "URL_7",  "URL_8",  "URL_9",  "URL_10", 
  "URL_11", "URL_12", "URL_13", "URL_14", "URL_15", 
  "URL_16", "URL_17", "URL_18", "URL_19", "URL_20", 
  "URL_21", "URL_22", "URL_23", "URL_24", "URL_25",
  "URL_26", "URL_27", "URL_28", "URL_29", "URL_30",  
  "URL_31", "URL_32", "URL_33", "URL_34", "URL_35", 
  "URL_36"
};

// globals
long volLevel; // audio volume level

// portal elements
WiFiManagerParameter* tagElementParam[TOTAL_ITEMS]; // Arrays to store pointers to tag objects
WiFiManagerParameter* urlElementParam[TOTAL_ITEMS]; // and url objects


/*
 *
 *    SetUp 
 * 
 */
void setup() {

  pinMode(PORTAL_PIN, INPUT_PULLUP);  // call for wifi portal
  pinMode(STREAM_PIN, INPUT_PULLUP);  // load default streams
  pinMode(NVS_CLR_PIN, INPUT_PULLUP); // clear non-volatile memory

  // Message port
  Serial.begin(115200);
  Serial.println(F("Aether Streamer"));
  Serial.print(F("Steven R Stuart,  "));
  Serial.print(F(__DATE__));
  Serial.print(F(" "));
  Serial.println(F(__TIME__));
  Serial.print(F("ver "));
  Serial.println(Version());
  
  // OLED display device
  Wire.begin(SDA_PIN, SCL_PIN);  // start i2c interface
  Wire.setClock(400000L);
  oled.begin(&Adafruit128x32, I2C_ADDRESS);  // start oled
  oled.setFont(System5x7);
  //oled.setFont(lcd5x7);
  
  if (digitalRead(NVS_CLR_PIN) == LOW) WipeNVS(); // user request to clear memory

  if (digitalRead(STREAM_PIN) == LOW) 
    initializeStreams();   // user request to load default streams
  populateStreams();       // fill streamsX array from prefs    ////////// TEST AND EDIT THIS FUNC

  // Reload the default streams if desired
  currentIndex = settingGet(listened);  // get index of previous listened stream

  // Configure Wifi system
  WifiPortalMessage();
  wifiMan.setDebugOutput(false);  // true if you want send to serial debug
  if (digitalRead(PORTAL_PIN) == LOW) {
    Serial.println(F("User requested WiFi configuration portal"));
    //wifiMan.resetSettings();     // erase wifi configuration
    WifiPortalMessage();
    wifiMan.startConfigPortal(portalName); 
  }
  else {
    // Tries to connect to the last known network. Launches a
    // captive portal if the connection fails or the timeout is reached.
    if(wifiMan.autoConnect(portalName)) {   
        // Retrieve the current Wi-Fi configuration
        wifi_config_t conf;
        if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
          if (digitalRead(STREAM_PIN) == LOW) {
            // show the stored ssid and password if STREAM_PIN is also low
            Serial.printf("SSID: %s\n", (char*)conf.sta.ssid);
            Serial.printf("Password: %s\n", (char*)conf.sta.password);
          }
        } 
        else {
          Serial.println(F("Failed to get WiFi config"));
          oled.clear();
          oled.print(F("CONFIG FAIL\nWiFi Error\n"));
        }
        oled.clear();
    } 
    else {
      Serial.println(F("Failed to connect to WiFi."));
      oled.clear();
      oled.print(F("CONNECT FAIL\nWiFi Error\nRestarting..."));
      delay(OLED_TIMER);
      esp_restart();
    }
  }

  oled.clear();
  oled.print(F("Aether Streamer\nSteven R Stuart\nW8AN"));
  //oled.print(Version());

  // Keyes KY-040
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, 100, false); // minValue, maxValue, circular
  rotaryEncoder.setEncoderValue(100-settingGet(audiovol));   // initial volume 
  rotaryEncoder.setAcceleration(25);
  //rotaryEncoder.disableAcceleration();

  // Audio system error messages (Debug, Info, Warning, Error)
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);   

  // Output stream configuration
  auto config = i2s.defaultConfig(TX_MODE);
  config.pin_bck = 26;  // BCLK  -max98357 pins
  config.pin_ws  = 25;  // LRC
  config.pin_data = 22; // DIN
  i2s.begin(config);

  // Set up i2s based on sampling rate provided by decoder
  mp3decode.begin();

  // Volume control
  volume.begin(config);   // config provides the bits_per_sample and channels
  volLevel = settingGet(audiovol);     // get the saved volume level
  volume.setVolume(volLevel / 100.0);  // set that volume
}


/*
 * Set up some loop globals
 */

// oled screen status and blanking timing
bool displayOn = true;
unsigned long oledStartTime; 
unsigned long oledCurrentTime = 0;   

// stream status
bool systemStreaming = false;

// menu vars
bool menuOpen = false;
int menuIndex = currentIndex;
long volumePos;

// sleep timer status and timing
bool systemSleeping = false;
bool timerRunning = true;
unsigned long sleepStartTime = millis();
unsigned long sleepCurrentTime = 0;

// portal
int portalMode = PORTAL_DOWN; 
bool firstPortal = true;


/*
 *
 *    Loop
 * 
 */
void loop() {
  if (systemSleeping) {
    
    if (rotaryEncoder.isEncoderButtonClicked() ||
        rotaryEncoder.encoderChanged()) {

      // wake from sleep
      systemSleeping = false;
      runSleepTimer(timerRunning);  // reset timer

      oled.clear();
      oled.println(F("WAKE UP"));
      displayOn = true;
      oledStartTime = millis(); // tickle the display timer
    }

  }
  else {

    if (systemStreaming) copier.copy();  // Run the open audio stream
    
    else {  // start a stream

      if (checkProtocol(currentIndex)) {
        // url seems ok
        urlstream.begin(streamsGetUrl(currentIndex));
        urlstream.setMetadataCallback(metadataCallback); // metadata processor
        settingPut(listened, currentIndex);              // save current selection
        systemStreaming = true;
      }
      else { 
        // url string length is too short
        currentIndex = settingGet(listened); // get previous stream
        menuIndex = currentIndex;  // reset menu display pointer
        oled.clear();
        oled.println(F("ERROR\nMissing URL\nReverting"));
      }

      displayOn = true;
      oledStartTime = millis(); // reset display timer
    }

    if (rotaryEncoder.isEncoderButtonClicked()) { 
      // button was pressed

      if (volLevel == 0) {
        // Handle timer or sleep mode
        oled.clear();
        sleepCurrentTime = millis();

        if (timerRunning & (sleepCurrentTime - sleepStartTime) < 3000) {
          // If user presses button within 3 seconds of timer set (twice 
          // in 3 seconds) when the volume is zero, then fall into cpu 
          // light-sleep mode. Note that if the timer is running, then the 
          // first click will be caught by the timer cancel code. It then
          // takes 2 more clicks to power down.
          oled.print(F("SYSTEM POWER DOWN\n\nv.")); // status notification
          oled.print(Version());
          urlstream.end();          // stop stream download
          systemStreaming = false;  // set state signals
          systemSleeping = true;
          esp_sleep_enable_ext0_wakeup((gpio_num_t) ROTARY_ENCODER_BUTTON_PIN, LOW); // set the restart signal
          esp_wifi_stop();          // shut down wifi
          delay(OLED_TIMER);
          oled.clear();
          esp_light_sleep_start();  // put cpu to sleep

          // CPU is now in sleep mode. ZZZzzzz

          // code will resume here when restart signal is asserted
          oled.println(F("SYSTEM RESTART")); // notification
          esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
          esp_restart();
        }

        else {
          // Set or reset sleep timer when volume is zero
          if (timerRunning) {
            runSleepTimer(false);
            oled.println(F("CANCEL TIMER"));
          }
          else {
            runSleepTimer(true);
            oled.println(F("TIMER SET")); 
            oled.print(SLEEP_TIMER/60000); // convert to minutes
            oled.println(F(" minutes"));  
          }
        }

        displayOn = true;
        oledStartTime = millis(); // tickle the display timer
      }

      else {
        if (menuOpen) { 
          // close the menu, prepare selected stream
          menuOpen = false;
          rotaryEncoder.setEncoderValue(volumePos);   // restore volume position 
          currentIndex = menuIndex;  // user chose this stream
          urlstream.end();           // stop stream download
          systemStreaming = false;   // signal that another stream is selected
          oledStatusDisplay();
        }

        else { 
          // open the menu, select a stream
          menuOpen = true;
          volumePos = rotaryEncoder.readEncoder(); // get volume setting
          rotaryEncoder.setEncoderValue(50);       // initialize position
          menuDisplay(currentIndex);  // show stream selection list
        }
      }

      displayOn = true;
      oledStartTime = millis(); // tickle the display timer
    }
  
    if (rotaryEncoder.encoderChanged()) {   // knob has turned

      if (menuOpen) {  
        // scroll the menu
        long encoderCurrPos = rotaryEncoder.readEncoder();
        if (encoderCurrPos > 50) menuIndex--; // determine direction
        else menuIndex++;
        rotaryEncoder.setEncoderValue(50); // re-initialize position

        if (menuIndex >= (TOTAL_ITEMS-1)) menuIndex = 0; // wrap around
        if (menuIndex < 0) menuIndex = TOTAL_ITEMS-1;

        menuDisplay(menuIndex);  // show stream selection list
      }

      else { 
        // Menu is not open, so set the volume level
        // volLevel value will be saved when oled timeout occurs
        volLevel = 100 - rotaryEncoder.readEncoder();
        volume.setVolume(volLevel / 100.0); // set speaker level
        //settingPut(audiovol, volLevel);   // store the setting (moved)
        oledStatusDisplay();  
      }

      displayOn = true;
      oledStartTime = millis(); // tickle the display timer
    }
  }

  // Turn off oled after a few seconds.
  // Also save volume level here to avoid overusing the prefs data write
  oledCurrentTime = millis();
  if (displayOn) {

    if (oledCurrentTime - oledStartTime > OLED_TIMER) {

      // oled timer has expired, turn it off
      if (menuOpen) {
        menuOpen = false;
        menuIndex = currentIndex;  // no selection, reset menu display pointer
        rotaryEncoder.setEncoderValue(volumePos);   // restore volume position 
      }

      oled.clear();  // blank the display
      displayOn = false;

      if (portalMode == PORTAL_UP) StreamPortalMessage(); // an override message

      // Store volume level only after user has settled on a value
      if ((volLevel != settingGet(audiovol)) && (volLevel > 0)) 
        settingPut(audiovol, volLevel);  // store the setting 
    }
  }

  if (!systemSleeping && timerRunning) { 
    // system is awake and timer is running
    
    // watch for timeout
    sleepCurrentTime = millis();
    if (sleepCurrentTime - sleepStartTime > SLEEP_TIMER) {

      // timer has expired, go to sleep (silent idle mode)
      urlstream.end();         // stop stream download
      systemStreaming = false; // set state signals
      systemSleeping = true;

      oled.clear();
      oled.println(F("SLEEPING"));
      displayOn = true;
      oledStartTime = millis(); // tickle the display timer
    }
  }

  // Portal functions
  if (portalMode == PORTAL_UP) wifiMan.process(); // process the portal web page
  bool portalSwitch = !digitalRead(PORTAL_PIN);   // true when pin is low

  if ((portalMode == PORTAL_IDLE) && !portalSwitch) 
    // switch has been opened. reset the mode
    portalMode = PORTAL_DOWN; // portal down
  
  if (portalSwitch) { 
    // user call for portal
    if (portalMode == PORTAL_DOWN) { 
      // start the portal

      //  set up portal parameters
      if (firstPortal) {
        // only instantiate new parameter objects if they have not yet been created
        firstPortal = false;
        for (int i=0; i<TOTAL_ITEMS; i++) {
          tagElementParam[i] = new WiFiManagerParameter{tagElement[i], nameElement[i], streamsGetTag(i), STREAM_ELEMENT_SIZE-1};
          urlElementParam[i] = new WiFiManagerParameter{urlElement[i], urlElement[i], streamsGetUrl(i), STREAM_ELEMENT_SIZE-1};
          wifiMan.addParameter(tagElementParam[i]);
          wifiMan.addParameter(urlElementParam[i]);
        }
      }
      else {
        for (int i=0; i<TOTAL_ITEMS; i++) {
          WiFiManagerParameter{tagElement[i], nameElement[i], streamsGetTag(i), STREAM_ELEMENT_SIZE-1};
          WiFiManagerParameter{urlElement[i], urlElement[i], streamsGetUrl(i), STREAM_ELEMENT_SIZE-1};
        }
      }
      wifiMan.setSaveParamsCallback(saveParamsCallback);  // param web page save event
      wifiMan.startWebPortal();
      portalMode = PORTAL_UP; 
      StreamPortalMessage();
    }
    else { 
      if (portalMode == PORTAL_SAVE) { 

        // Store data fields from portal into streamsX array
        for (int i=0; i<TOTAL_ITEMS; i++) {
          streamsPut(i, tagElementParam[i]->getValue(), urlElementParam[i]->getValue()); 
        }
        // Save changes to the prefs database
        populatePrefs();  

        // stop the portal
        wifiMan.stopWebPortal();

        // set mode according to switch position
        if (portalSwitch) portalMode = PORTAL_IDLE;
        else portalMode = PORTAL_DOWN;

        oled.clear();
        oled.println(F("SAVED"));
        displayOn = true;
        oledStartTime = millis(); // reset display timer
      }
    }

  }
  else {
    // switch is off
    if (portalMode != PORTAL_DOWN) {
      // force portal shutdown

      wifiMan.stopWebPortal();
      portalMode = PORTAL_DOWN;

      oled.clear();
      oled.print(F("PORTAL DOWN"));
      displayOn = true;
      oledStartTime = millis(); // reset display timer
    }
  }
}



/*
 *
 *    Program Functions
 *
 */

/*
 * Parameter web page callback function
 */
void saveParamsCallback(void) {
  // called when the SAVE button is clicked on the 
  // portal parameter web page -> http://<local_ip>/param

  portalMode = PORTAL_SAVE; // data save is pending
}


/*
 * Metadata callback function
 */
void metadataCallback(MetaDataType type, const char* str, int len) {
  // called when stream metadata is available
  // note that ICYStream must be used

  Serial.print("==> ");
  Serial.print(toStr(type));
  Serial.print(": ");
  Serial.println(str);

/*  INCOMPLETE * * *

  // Example: Extract the "StreamTitle" field
  int titleStart = metadata.indexOf("StreamTitle='");
  if (titleStart != -1) {
    titleStart += 13; // Move past "StreamTitle='"
    int titleEnd = metadata.indexOf("';", titleStart);
    if (titleEnd != -1) {
      String streamTitle = metadata.substring(titleStart, titleEnd);
      Serial.println("Now Playing: " + streamTitle);
    }
  }
*/  
}


/*
 * Enable or disable sleep timer
 */
void runSleepTimer(bool enabled) {
  // enabled = true to set, false to clear
  timerRunning = enabled;
  if (enabled) {
    sleepStartTime = millis();  // sleep timer baseline
  }
}


/*
 * Display the timer, signal, volume level
 */
void oledStatusDisplay(void) {

  int dBm = WiFi.RSSI();
  oled.clear();
  oled.println(streamsGetTag(currentIndex)); // stream name
  if (timerRunning) {
    oled.print(F("timer : "));
    oled.println(timerTimeLeft());
  }
  oled.print(F("signal: "));
  oled.print(dBm); 
  if (dBm >= -30)  oled.println(F(" excellent"));
  else if (dBm >= -67 ) oled.println(F(" good"));
    else if (dBm >= -70 ) oled.println(F(" fair"));
      else if (dBm >= -80 ) oled.println(F(" weak"));
        else oled.println(F(" very weak"));
  oled.print(F("volume: "));
  oled.print(volLevel);
}


/*
 * Display the stream configuration portal message
 */
void StreamPortalMessage(void) {

  oled.clear();
  oled.println(F("PORTAL OPEN"));
  oled.print(WiFi.localIP());
  oled.println(F("/param"));
}


/*
 * Display the Wifi configuration portal message
 */
void WifiPortalMessage(void) {

  oled.clear();
  oled.println(F("WIFI PORTAL"));
  oled.println(F("Configure at"));
  oled.print(F("ssid: "));
  oled.println(portalName);
  oled.println(F("ip:   192.168.4.1"));
}


/*
 * Return the minutes left on the timer
 */
String timerTimeLeft(void) {

    sleepCurrentTime = millis(); // set baseline

    // calculate time left
    long totalSeconds = SLEEP_TIMER/1000 - ((sleepCurrentTime - sleepStartTime)/1000);
    int minutes = (totalSeconds % 3600) / 60;
    return String(minutes) + " mins";
}


/*
 * Display the stream selection menu
 */
void menuDisplay(int menuIndex) {
  
  oled.clear();
  oled.println(streamsGetTag(currentIndex));  // title line

  // previous line item
  int lineIndex = (menuIndex == 0 ? (TOTAL_ITEMS-1) : menuIndex-1);
  //oled.print(F(" ")); // indent
  if (checkProtocol(lineIndex)) oled.println(streamsGetTag(lineIndex));
  else oled.println(lineIndex+1); // print only line number if error
  
  // current line item
  if (checkProtocol(menuIndex)) {  // selection line
    oled.setInvertMode(true);
    //oled.print(F(">")); // current item pointer
    oled.println(streamsGetTag(menuIndex));
    oled.setInvertMode(false);
  } 
  else {
    //oled.print(F(" ")); // indent
    oled.println(menuIndex+1);
  }
  
  // next line item
  lineIndex = (menuIndex == (TOTAL_ITEMS-1) ? 0 : menuIndex+1);
  //oled.print(F(" ")); // indent
  if (checkProtocol(lineIndex)) oled.println(streamsGetTag(lineIndex));
  else oled.print(lineIndex+1);
}


/*
 * Return true if the url protocol text is correct
 */
bool checkProtocol(int menuIndex) {
  return strncmp(streamsGetUrl(menuIndex), "http://", 7) == 0;
}


/*
 * Retrieve a persistent preference setting
 */
int settingGet(const char* setting) {
  prefs.begin(settings, PREF_RO);
  int settingVal = prefs.getInt(setting, 0); // default = 0
  prefs.end();
  return settingVal;
}


/*
 * Store a persistent preference setting
 */
void settingPut(const char* setting, int settingVal) {
  prefs.begin(settings, PREF_RW);
  prefs.putInt(setting, settingVal);
  prefs.end();
}


/*
 * Load the streamsX array with streams that are saved in prefs object
 */
void populateStreams(void) {  
  // This function will initialize default streams when nvs is blank
  // You can manually re-load the default streams by pulling STREAM_PIN low at boot.

  // place a marker key to indicate that the prefs is populated
  prefs.begin(settings, PREF_RO);
  bool prefExist = prefs.isKey(initPref);  // look for test key
  prefs.end();

  if (!prefExist) { 
    // test key was not found, create it then set default streams
    prefs.begin(settings, PREF_RW);
    prefs.putInt(initPref, 1);   // create the test key
    prefs.end(); 
    initializeStreams();
  }

  // fill steamsX with names and urls from prefs
  for (int item=0; item < TOTAL_ITEMS; item++) {
    prefs.begin(stream_item[item], PREF_RW); 
    // check for key existence and place a blank string if missing
    if (!prefs.isKey(stream_type[TYPE_TAG])) prefs.putString(stream_type[TYPE_TAG], "");
    if (!prefs.isKey(stream_type[TYPE_URL])) prefs.putString(stream_type[TYPE_URL], "");
    // get the values and place in streamsX array
    streamsPut(item, prefs.getString(stream_type[TYPE_TAG]).c_str(), 
                     prefs.getString(stream_type[TYPE_URL]).c_str());
    prefs.end();
  }
}


/*
 * Fill the prefs object with data from the streamsX array
 */
void populatePrefs(void) {
  for (int item=0; item < TOTAL_ITEMS; item++) {
    prefs.begin(stream_item[item], PREF_RW); 
    prefs.clear();
    prefs.putString(stream_type[TYPE_TAG], streamsGetTag(item));
    prefs.putString(stream_type[TYPE_URL], streamsGetUrl(item));
    prefs.end();
  }
}


/*
 * Put the stream tag and url strings into the streamsX array
 */
void streamsPut(int index, const char* tag, const char* url) {
  strncpy( streamsX + (index * STREAM_ITEM_SIZE), tag, STREAM_ELEMENT_SIZE );
  strncpy( streamsX + (index * STREAM_ITEM_SIZE + STREAM_ELEMENT_SIZE), url, STREAM_ELEMENT_SIZE );
}


/*
 * Get the name tag string from the streamsX array at index
 */
char* streamsGetTag(int index) { // index = 0..9
  return streamsX + (index * STREAM_ITEM_SIZE);
}


/*
 * Get the url string from the streamsX array at index
 */
char* streamsGetUrl(int index) { // index = 0..9
  return streamsX + (index * STREAM_ITEM_SIZE + STREAM_ELEMENT_SIZE);
}


/*
 * Load the following default stream name tags and urls into the prefs object  
 */
void initializeStreams(void) {
  // This function clobbers any user entered streams.

  oled.clear();
  oled.println("INITIALIZE");
  oled.print("Loading default\nstreams...\n");
  
  // Default Streams (49 char name + 49 char url, +nulls)
  char stream_data[72][50] = { // 72 elements of 50 chars (72/2=36 streams)
    // 1-10
    "Psyndora Chillout","http://cast.magicstreams.gr:9125",          
    "Psyndora Psytrance","http://cast.magicstreams.gr:9111",         
    "Radio Play Emotions", "http://5.39.82.157:8054/stream",         
    "Rare 80s Music", "http://209.9.238.4:9844/",                    
    "Simply Oldies","http://uk5.internet-radio.com:8153",                 
    "Skylark Stream", "http://uk2.internet-radio.com:8164/listen.ogg",
    "Synphaera Radio","http://ice2.somafm.com/synphaera-128-mp3",     
    "The Seagull","http://us5.internet-radio.com:8121",                  
    "XRDS.fm","http://us1.internet-radio.com:8321",                      
    "Ambient Radio", "http://uk2.internet-radio.com:8171/stream",     
    // 11-20
    "Best of Art Bell","http://108.161.128.117:8050",                 
    "Big 80s Station","http://158.69.114.190:8024",                   
    "Big Hair Radio","http://192.111.140.11:8508",                    
    "Box UK Radio", "http://uk7.internet-radio.com:8226",                      
    "Classical Radio","http://classicalradiostream.com:8010",               
    "Dark Edge Radio","http://5.35.214.196:8000",                     
    "Detroit Industrial Underground", "http://138.197.0.4:8000/stream",
    "Dimensione Relax", "http://51.161.115.200:8012/stream",           
    "Disco Funk","http://eu10.fastcast4u.com:8120",                      
    "EarthSong Experimental","http://cast3.my-control-panel.com:7084/autodj", 
    // 21-30
    "First Amendment Radio","http://198.178.123.8:7862",                      
    "Gothville", "http://gothville.radio:8000/stream",                   
    "HardTecho and Schranz","http://schranz.in:8000",                    
    "J-Pop Sakura","http://cast1.torontocast.com:2170",                  
    "KXFU - RDSN.net","http://184.95.62.170:9788",                       
    "Lounge Radio", "http://fr1.streamhosting.ch:80/lounge128.mp3",      
    "Majestic Jukebox","http://uk3.internet-radio.com:8405",             
    "Mangled Web Radio", "http://144.126.151.19:8000/mp3",               
    "Megaton Cafe Radio","http://us2.internet-radio.com:8443",           
    "Metal Express Radio","http://5.135.154.69:11590",                   
    // 31-36
    "Metal Rock Radio","http://kathy.torontocast.com:2800",              
    "Mission Control Radio","http://151.80.42.191:8372",                 
    "Moon Mission Recordings","http://uk5.internet-radio.com:8306",      
    "Move Da House","http://uk7.internet-radio.com:8000",                
    "Mr. Liberty Show","http://198.178.123.5:8258",                      
    "Musical Ventur Radio","http://us3.internet-radio.com:8614"         
  };

  for (int item=0; item < TOTAL_ITEMS; item++) {
    // stuff the prefs object with default data
    prefs.begin(stream_item[item], PREF_RW);
    prefs.clear();
    prefs.putString(stream_type[TYPE_TAG], stream_data[item*2]);
    prefs.putString(stream_type[TYPE_URL], stream_data[item*2+1]);
    prefs.end();

    oled.setCursor(0, 3); // col, row
    oled.clearToEOL();
    oled.setCursor(0, 3);
    oled.print(stream_data[item*2]);
  }
  settingPut(listened, 0);  // default to first stream
}

/*
 * Create a version tag from compile time
 */
String Version(void) {
    // Extract date and time from __DATE__ and __TIME__
    const char monthStr[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", 
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    
    // Parse __DATE__
    char month[4];
    int day, year;
    sscanf(__DATE__, "%s %d %d", month, &day, &year);

    // Convert month name to number
    int monthNumber = 0;
    for (int i = 0; i < 12; i++) {
        if (strcmp(month, monthStr[i]) == 0) {
            monthNumber = i + 1;
            break;
        }
    }

    // Parse __TIME__
    int hour, minute, second;
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);

    // Format as "YYYYMMDD.HHMMSS"
    char dateTimeString[20];
    snprintf(dateTimeString, sizeof(dateTimeString), "%04d%02d%02d.%02d%02d",
             year, monthNumber, day, hour, minute);

    return String(dateTimeString);
}

/*
 * Wipe the NVS memory (wifi, prefs, etc.)
 */
void WipeNVS(void) {
  oled.clear();
  oled.print(F("NVS\nClearing Memory\n"));
  nvs_flash_erase();      // erase the NVS partition and...
  nvs_flash_init();       // initialize the NVS partition.
  oled.println(F("Complete"));
  oled.printf("Unshort D%d pin", NVS_CLR_PIN);
  while (true);           // loop forever
}

// eof
