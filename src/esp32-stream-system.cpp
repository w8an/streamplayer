/*
 * esp32-stream-system.cpp
 * jul-sep 2024, feb 2025
 * 
 * AetherStream - Stream From the Beyond
 * Copyright (C)2024,2025 Steven R Stuart
 * 
 * This program outputs icy/mp3 internet stream to max98357a i2c audio device(s).
 * Turn the knob to set volume. Press the button to change station.
 * Set volume to 0 and wait 5 seconds and the system will shut-down.
 * Device has an automatic shut-off timer that can be set to 1 hour, 6 or 12 hours.
 * Click button with volume set to 0 configures timer.
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
#include "prototypes.h"
#include "AetherStreams.h"

// display I/O
#define I2C_ADDRESS 0x3C        // ssd1306 oled
#define SDA_PIN 18              // i2c data
#define SCL_PIN 19              // i2c clock

// user control inputs
#define NVS_CLR_PIN 17          // clear non-volatile memory when low on reset
#define STREAM_PIN 16           // set default streams when low on reset
#define PORTAL_PIN 15           // enable wifi portal when pulled low      
#define META_PIN 27             // display stream meta when high (default)

// portal states
#define PORTAL_DOWN 0           // portal is off
#define PORTAL_UP 1             // portal is running
#define PORTAL_SAVE 2           // data save is pending
#define PORTAL_IDLE 4           // waiting for switch reset

// display parameters
#define OLED_LINEWIDTH 21       // char width of display (5x7 font)
#define OLED_TIMER   3500       // display timeout in milliseconds

// timer settings
#define MS1HOUR   3600000       // 1 hour in milliseconds
#define MS6HOUR  21600000       // 6 hours in ms
#define MS12HOUR 43200000       // 12 hours in ms

// System is hard coded to 36 streams (TOTAL_ITEMS)
// Each item in the streamsX array contains a text name and a url,
// the name and url elements are each STREAM_ELEMENT_SIZE in length.
// The arrays stream_item[], tagElement[], and nameElement[] must have TOTAL_ITEMS strings. 
#define STREAM_ELEMENT_SIZE 50        // max length of a name or url (each is 49 char + nul)
#define STREAM_ITEM_SIZE 100          // line item width (two stream elements: the name & url)
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
const char* timerVal = "timerVal";    // 0 = 1 hour, else 6 hours
const char* timerOn  = "timerOn";     // setting key of timer activity state
const char* initPref = "initPref";    // key for initilization

// MAX98357 I2S audio board pins
#define MAX_DIN 22   // serial data
#define MAX_LRC 25   // word select
#define MAX_BCLK 26  // serial clock

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
ICYStream icystream;                  // stream object with metadata
I2SStream i2s;                        // audio data
VolumeStream volume(i2s);
EncodedAudioStream mp3decode(&volume, new MP3DecoderHelix()); // Decoder stream
StreamCopy copier(mp3decode, icystream); // copy icystream to decoder
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
long volLevel;                     // audio volume level
bool timerIsRunning;                 // timer active state
unsigned long sleepTimerDuration;  // user desired sleep time

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
  pinMode(META_PIN, INPUT_PULLUP);    // enable meta title function

  // Message port
  Serial.begin(115200);
  Serial.println(F("Aether Streamer"));
  Serial.print(F("Steven R Stuart,  "));
  Serial.print(F(__DATE__));
  Serial.print(F(" "));
  Serial.println(F(__TIME__));
  Serial.print(F("ver "));
  Serial.println(version());
  
  // SSD1306 OLED display device
  Wire.begin(SDA_PIN, SCL_PIN);  // start i2c interface
  Wire.setClock(400000L);
  oled.begin(&Adafruit128x32, I2C_ADDRESS);  // start oled
  oled.setFont(Adafruit5x7);     // choose a font
  //oled.setFont(System5x7);
  //oled.setFont(font5x7);
  //oled.setFont(lcd5x7);
  
  if (digitalRead(NVS_CLR_PIN) == LOW) wipeNVS(); // user request to clear memory

  if (digitalRead(STREAM_PIN) == LOW) 
    initializeStreams();   // user request to load default streams
  populateStreams();       // fill streamsX array from prefs 

  // Reload the default streams if desired
  currentIndex = getSetting(listened);  // get index of previous listened stream

  // Configure Wifi system
  wifiPortalMessage();
  wifiMan.setDebugOutput(false);  // true if you want send to serial debug
  if (digitalRead(PORTAL_PIN) == LOW) {  
    // user requested WiFi configuration portal

    //wifiMan.resetSettings();     // erase wifi configuration
    wifiPortalMessage();
    wifiMan.startConfigPortal(portalName); 
  }

  else { // configuration portal has not been requested.

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

    else {  // wifi autoconnect failed
      Serial.println(F("Failed to connect to WiFi."));
      oled.clear();
      oled.print(F("CONNECT FAIL\nWiFi Error\nRestarting..."));
      delay(OLED_TIMER);
      esp_restart();
    }
  }

  oled.clear();
  oled.print(F("Aether Streamer\nSteven R Stuart\n\n"));
  //oled.print(version());

  // Keyes KY-040
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, 100, false); // minValue, maxValue, circular
  rotaryEncoder.setEncoderValue(100-getSetting(audiovol));   // initial volume 
  rotaryEncoder.setAcceleration(25);
  //rotaryEncoder.disableAcceleration();

  // Audio system error messages (Debug, Info, Warning, Error)
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);   

  // Output stream configuration
  auto config = i2s.defaultConfig(TX_MODE);
  config.pin_bck  = MAX_BCLK; // serial clock (MAX98357 pins)
  config.pin_ws   = MAX_LRC;  // word select
  config.pin_data = MAX_DIN;  // serial data
  i2s.begin(config);

  // Set up i2s based on sampling rate provided by decoder
  mp3decode.begin();
  
  // Get the sleep timer settings
  assignTimerValsFromPrefs();
  //displayTimerEnabledSetting();  // shows enable state and timer duration value

  // Volume control
  volume.begin(config);   // config provides the bits_per_sample and channels
  volLevel = getSetting(audiovol);     // get the saved volume level
  volume.setVolume(volLevel / 100.0);  // set that volume
}


/*
 * Set up some loop globals
 */

// oled screen status and blanking timing
bool displayIsOn = true;
unsigned long oledStartTime; 
unsigned long oledCurrentTime = 0;   

// stream status
bool systemStreaming = false;

// menu vars
bool streamSelectionMenuIsOpen = false;
int menuIndex = currentIndex;
long volumePos;

// sleep timer status and timing
bool systemIsSleeping = false;
bool timerInSetupMode = false;
unsigned long sleepStartTime = millis();
unsigned long sleepCurrentTime = 0;

// portal
int portalMode = PORTAL_DOWN;     // current state
bool firstPortal = true;          // op flag

// meta data
bool metaEnabled = false;         // true when data desired (set by META_PIN input)
bool metaQueryTriggered = false;  // true when queued for display
String metaTitle;


/*
 *
 *    Loop
 * 
 */
void loop() {
  if (systemIsSleeping) {
    
    if (rotaryEncoder.isEncoderButtonClicked() ||
        rotaryEncoder.encoderChanged()) {

      // wake from sleep
      systemIsSleeping = false;
      sleepStartTime = millis();  // reset sleep timer baseline

      oled.clear();
      oled.println(F("WAKE UP"));
      displayIsOn = true;
      oledStartTime = millis(); // tickle the display timer
    }

  }
  else { // system is active

    if (systemStreaming) copier.copy();  // Run the open audio stream
    
    else {  // start a stream

      if (checkProtocol(currentIndex)) {
        // url seems ok

        icystream.begin(getStreamsUrl(currentIndex));
        icystream.setMetadataCallback(callbackMetadata); // metadata processor
        putSetting(listened, currentIndex);              // save current selection
        systemStreaming = true;
      }
      else { // url string error
        
        // url string is not recognized
        currentIndex = getSetting(listened); // get previous stream
        menuIndex = currentIndex;  // reset menu display pointer
        oled.clear();
        oled.println(F("ERROR\nMissing URL\nReverting"));
      }

      metaTitle = String("");        // clear old title
      metaQueryTriggered = true;     // show title if available

      displayIsOn = true;
      oledStartTime = millis();      // reset display timer
    }

    if (rotaryEncoder.isEncoderButtonClicked()) { 
      // button was clicked
      
      if (volLevel == 0) { 
        // button clicked and volume is zero
        // enter the timer configuration

        // turn the speaker audio on
        volLevel = getSetting(audiovol);     // get the saved volume level
        volume.setVolume(volLevel / 100.0);  // set that volume
        volumePos = rotaryEncoder.readEncoder(); // get encoder setting

        timerInSetupMode = true;

        oled.clear();
        oled.println(F("TIMER"));
        displayTimerEnabledSetting();     // show prefs values
        if (getSetting(timerOn) != 0) displayTimerValSetting(); 

      }

      else {  
        // button was clicked and volume is not zero

        if (timerInSetupMode) {
          // Store and activate timer setting

          timerInSetupMode = false;
          oled.clear();
          oled.println(F("TIMER SAVED"));
          
          if (timerIsRunning) {  // user selected a timer duration

            sleepStartTime = millis();  // sleep timer baseline
            putSetting(timerVal, timerDurationToValue(sleepTimerDuration)); // store prefs
            putSetting(timerOn, 1);
            displayTimerDurationSetting();
          }
          else { // user disabled the timer

            putSetting(timerOn, 0);
            displayTimerEnabledSetting();  // show saved prefs
          }

          rotaryEncoder.setEncoderValue(100-getSetting(audiovol)); // restore encoder position 
        }

        else { 
          // button was clicked, volume is not zero, and timer is not in setup mode

          if (streamSelectionMenuIsOpen) { 

            // close the menu, prepare selected stream
            streamSelectionMenuIsOpen = false;
            rotaryEncoder.setEncoderValue(volumePos);   // restore volume position 
            currentIndex = menuIndex;  // user chose this stream
            icystream.end();           // stop stream download
            systemStreaming = false;   // signal that another stream is selected
            oledStatusDisplay();
          }

          else { // stream selection menu is not open

            // open the menu, select a stream
            streamSelectionMenuIsOpen = true;
            volumePos = rotaryEncoder.readEncoder(); // get volume setting
            rotaryEncoder.setEncoderValue(50);       // initialize position
            displayStreamMenu(currentIndex);  // show stream selection list
          }
        }
      }

      displayIsOn = true;
      oledStartTime = millis(); // tickle the display timer
    }
  
    if (rotaryEncoder.encoderChanged()) {   // knob has been turned

      if (streamSelectionMenuIsOpen) { 
        // knob has turned and stream selection menu is open
        // scroll the menu

        long encoderCurrPos = rotaryEncoder.readEncoder();
        if (encoderCurrPos > 50) menuIndex--; // determine direction
        else menuIndex++;
        rotaryEncoder.setEncoderValue(50); // re-initialize position

        if (menuIndex >= (TOTAL_ITEMS-1)) menuIndex = 0; // wrap around
        if (menuIndex < 0) menuIndex = TOTAL_ITEMS-1;

        displayStreamMenu(menuIndex);  // show stream selection list
      }

      else { 
        // knob has turned and stream selection menu is not open

        if (timerInSetupMode) { 
          // choose the next timer setting

          oled.clear(); 
          oled.println(F("SET TIMER"));
          oled.println();
          changeTimerDuration();          // select next duration

          if (timerIsRunning == false) oled.println(F("Disabled"));
          else displayTimerDurationSetting();  
        }

        else { 
          // knob has turned, but not setting timer, and not selecting stream
          // (this is default procedure for an encoder change)

          // adjust the volume level
          // volLevel value will be saved when oled timeout occurs
          volLevel = 100 - rotaryEncoder.readEncoder();
          volume.setVolume(volLevel / 100.0); // set speaker level

          oledStatusDisplay();  

          metaQueryTriggered = true;  // enable meta data (displays when metaEnabled)
        }
      }

      displayIsOn = true;
      oledStartTime = millis(); // tickle the display timer
    }
  }

  oledCurrentTime = millis();
  if (displayIsOn) {
    // Turn off oled after a few seconds.
    // Save volume level into prefs data if changed.
    // Indicate portal status if it is active.
    // Shut-down system if directed.

    if (oledCurrentTime - oledStartTime > OLED_TIMER) {
      // oled timer has expired

      oled.clear();  // blank the display
      displayIsOn = false;

      if (volLevel == 0) {
        systemPowerDown(); // Put into sleep mode
      }

      if (streamSelectionMenuIsOpen) {

        streamSelectionMenuIsOpen = false;
        menuIndex = currentIndex;  // no selection, reset menu display pointer
        rotaryEncoder.setEncoderValue(volumePos);   // restore volume position 
      }

      if (metaQueryTriggered) {      // time for meta display
        metaQueryTriggered = false;  // reset metadata flag
        if (metaEnabled) {           // if user wants meta
          displayMeta();             // show it now
        }
      }

      if (portalMode == PORTAL_UP) StreamPortalMessage(); // an override message

      if (timerInSetupMode) {
  
        timerInSetupMode = false;
        oled.println(F("TIMER"));
        oled.println(F("Not Changed"));

        // restore the timer settings from prefs
        assignTimerValsFromPrefs();
        displayTimerEnabledSetting();  // show the saved prefs
        displayTimerValSetting();

        rotaryEncoder.setEncoderValue(100-getSetting(audiovol)); // restore encoder position 

        displayIsOn = true;
        oledStartTime = millis();     // tickle the display timer
      }
      
      // Store volume level only after user has settled on a value
      if (volLevel != getSetting(audiovol)) 
        putSetting(audiovol, volLevel);  // store the setting 

    }
  }

  if (!systemIsSleeping && timerIsRunning) { 
    // system is awake and timer is running
    
    // watch for timeout
    sleepCurrentTime = millis();
    if (sleepCurrentTime - sleepStartTime > sleepTimerDuration) {

      // timer has expired, go to sleep (silent idle mode)
      icystream.end();         // stop stream download
      systemStreaming = false; // set state signals
      systemIsSleeping = true;

      oled.clear();
      oled.println(F("SLEEPING"));
      displayIsOn = true;
      oledStartTime = millis(); // tickle the display timer
    }
  }

  // display meta at status timeout when enabled
  if (digitalRead(META_PIN) == HIGH) metaEnabled = true;
  else metaEnabled = false;   // pull pin low to disable
  
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
          tagElementParam[i] = new WiFiManagerParameter{tagElement[i], nameElement[i], getStreamsTag(i), STREAM_ELEMENT_SIZE-1};
          urlElementParam[i] = new WiFiManagerParameter{urlElement[i], urlElement[i], getStreamsUrl(i), STREAM_ELEMENT_SIZE-1};
          wifiMan.addParameter(tagElementParam[i]);
          wifiMan.addParameter(urlElementParam[i]);
        }
      }

      else { // portal settings have already been created

        for (int i=0; i<TOTAL_ITEMS; i++) {
          WiFiManagerParameter{tagElement[i], nameElement[i], getStreamsTag(i), STREAM_ELEMENT_SIZE-1};
          WiFiManagerParameter{urlElement[i], urlElement[i], getStreamsUrl(i), STREAM_ELEMENT_SIZE-1};
        }
      }

      wifiMan.setSaveParamsCallback(callbackSaveParams);  // param web page save event
      wifiMan.startWebPortal();
      portalMode = PORTAL_UP; 
      StreamPortalMessage();
    }

    else { // portal is not down

      if (portalMode == PORTAL_SAVE) { 

        // Store data fields from portal into streamsX array
        for (int i=0; i<TOTAL_ITEMS; i++) {
          putStreams(i, tagElementParam[i]->getValue(), urlElementParam[i]->getValue()); 
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
        displayIsOn = true;
        oledStartTime = millis(); // reset display timer
      }
    }

  }
  else { // portal switch is off
    
    if (portalMode != PORTAL_DOWN) {
      // force portal shutdown

      wifiMan.stopWebPortal();
      portalMode = PORTAL_DOWN;

      oled.clear();
      oled.print(F("PORTAL DOWN"));
      displayIsOn = true;
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
 * Display the stream metadata
 */
void displayMeta(void) {

  oled.clear();
  
  int strLength = metaTitle.length();
  if (strLength == 0) return;           // no data

  if (strLength < (OLED_LINEWIDTH-3)*3) // suppress heading on long title
    oled.println("NOW PLAYING");        // heading
  oledSplitString(metaTitle);           // display the metadata

  displayIsOn = true;
  oledStartTime = millis();            // reset the display timer
}


/*
 * Split the string into words and display them unbroken
 */
void oledSplitString(String str) {
  // convert to char array for tokenization
  char buffer[str.length() + 1];
  str.toCharArray(buffer, sizeof(buffer));
  
  // initialize current line and word pointer
  String currentLine = "";
  char* word = strtok(buffer, " ");
  
  // process each word
  while (word != NULL) {
      String wordStr(word);
      
      // check if adding this word would exceed line length
      if (currentLine.length() + wordStr.length() + 1 <= OLED_LINEWIDTH) {
          // Add word to current line
          if (currentLine.length() > 0) {
              currentLine += " ";
          }
          currentLine += wordStr;
      } 
      else {
          // Print current line and start new one
          oled.println(currentLine);
          currentLine = wordStr;
      }

      word = strtok(NULL, " ");
  }
  
  // print last line if not empty
  if (currentLine.length() > 0) {
      oled.println(currentLine);
  }
}


/*
 * Parameter web page callback function
 * called when the SAVE button is clicked on the 
 * portal parameter web page:
 * http://<local_ip>/param
 */
void callbackSaveParams(void) {

  portalMode = PORTAL_SAVE;     // data save is pending
}


/*
 * Metadata callback function
 * called when stream metadata is available
 */
void callbackMetadata(MetaDataType type, const char* str, int len) {

  // we are only interested in the stream title
  if (type == MetaDataType::Title) metaTitle = String(str);
}


/*
 * Display the timer, signal, volume level
 */
void oledStatusDisplay(void) {

  int dBm = WiFi.RSSI();
  oled.clear();
  if (volLevel > 0) {
    oled.println(getStreamsTag(currentIndex)); // stream name
    if (timerIsRunning) {
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
  else {
    oled.clear();
    oled.println(F("ZERO FUNCTION"));
    oled.println(F("Click for Timer"));
    oled.println(F(" or"));
    oled.print(F("Wait for Shutdown"));
  }
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
void wifiPortalMessage(void) {

  oled.clear();
  oled.println(F("WIFI PORTAL"));
  oled.println(F("Configure at"));
  oled.print(F("ssid: "));
  oled.println(portalName);
  oled.println(F("ip:   192.168.4.1"));
}


/*
 * Return a formatted string of time remaining on shut-off timer
 */
String timerTimeLeft(void) {

    sleepCurrentTime = millis(); // set baseline
    unsigned long totalSeconds = 
                  sleepTimerDuration/1000 - ((sleepCurrentTime - sleepStartTime)/1000);
    unsigned long hours = totalSeconds / 3600;
    unsigned long minutes = (totalSeconds % 3600) / 60;

    char timeString[10]; // Buffer for formatted string
    if (hours < 1) return String(minutes) + " mins";
    else {
      snprintf(timeString, sizeof(timeString), "%2lu:%02lu", hours, minutes);
      return String(timeString);
    }
}


/*
 * Display the stream selection menu
 */
void displayStreamMenu(int menuIndex) {
  
  oled.clear();
  oled.println(getStreamsTag(currentIndex));  // title line

  // previous line item
  int lineIndex = (menuIndex == 0 ? (TOTAL_ITEMS-1) : menuIndex-1);
  //oled.print(F(" ")); // indent
  if (checkProtocol(lineIndex)) oled.println(getStreamsTag(lineIndex));
  else oled.println(lineIndex+1); // print only line number if error
  
  // current line item
  if (checkProtocol(menuIndex)) {  // selection line
    oled.setInvertMode(true);
    //oled.print(F(">")); // current item pointer
    oled.println(getStreamsTag(menuIndex));
    oled.setInvertMode(false);
  } 
  else {
    //oled.print(F(" ")); // indent
    oled.println(menuIndex+1);
  }
  
  // next line item
  lineIndex = (menuIndex == (TOTAL_ITEMS-1) ? 0 : menuIndex+1);
  //oled.print(F(" ")); // indent
  if (checkProtocol(lineIndex)) oled.println(getStreamsTag(lineIndex));
  else oled.print(lineIndex+1);
}


/*
 * Set system timer settings from stored values
 */
void assignTimerValsFromPrefs(void) {
  switch (getSetting(timerVal)) {
    case 1: sleepTimerDuration = MS6HOUR; break;
    case 2: sleepTimerDuration = MS12HOUR; break;
    default: sleepTimerDuration = MS1HOUR;
  }
  if (getSetting(timerOn) == 1) timerIsRunning = true;
  else timerIsRunning = false;
}


/*
 * Display the timer enabled setting at current oled cursor position
 */
void displayTimerEnabledSetting(void) {
  // show the pref setting
  if (getSetting(timerOn) == 0) timerEnabledText(false);
  else timerEnabledText(true);
}


/*
 * Timer display text logic
 */
void displayTimerIsRunningSetting(void) {
  // show the running setting
  if (timerIsRunning) timerEnabledText(true);
  else timerEnabledText(false);
}


/*
 * Output the timer enabled state text
 */
void timerEnabledText(bool en) {
  if (en) oled.println(F("Enabled"));
  else oled.println(F("Disabled"));
}


/*
 * Display the timer duration pref setting at current oled cursor position
 */
void displayTimerValSetting(void) {
  // from pref
  timerDurationText(getSetting(timerVal));
}


/*
 * Timer duration text logic
 */
void displayTimerDurationSetting(void) {    
  // current time length setting
  timerDurationText(timerDurationToValue(sleepTimerDuration));
}


/*
 * Output the timer duration text
 */
void timerDurationText(int durval) { 

  switch (durval) {
    case 1: oled.println(F("6 hours")); break;
    case 2: oled.println(F("12 hours")); break;
    default: oled.println(F("1 hour")); //MS1HOUR
  }
}


/*
 * Rotate to next timer duration or disable
 */
void changeTimerDuration(void) {
  // Rotate to next timer value

  if (timerIsRunning == false) {
    timerIsRunning = true;  
    sleepTimerDuration = MS1HOUR;  // 1 hr en
  } 
  else {
    switch (sleepTimerDuration) {
      case MS1HOUR:
        sleepTimerDuration = MS6HOUR;
        break;
      case MS6HOUR: 
        sleepTimerDuration = MS12HOUR;
        break;
      case MS12HOUR:
        sleepTimerDuration = MS1HOUR; 
        timerIsRunning = false;
        break;
    }
  }
}


/*
 * Convert timer pref value to duration
 */
unsigned long timerValueToDuration(int settingVal) {
  // prefs timerVal to duration
  switch (settingVal) {
    case 1: return MS6HOUR; 
    case 2: return MS12HOUR;
  }
  return MS1HOUR;  // case 0
}


/*
 * Convert timer duration to pref value
 */
int timerDurationToValue(unsigned long duration) {
  // duration to prefs timerVal
  switch (duration) {
    case MS6HOUR: return 1;
    case MS12HOUR: return 2;
  }
  return 0;
}


/*
 * Return true if the url protocol text is correct
 */
bool checkProtocol(int menuIndex) {
  return strncmp(getStreamsUrl(menuIndex), "http://", 7) == 0;
}


/*
 * Retrieve a persistent preference setting
 */
int getSetting(const char* setting) {
  prefs.begin(settings, PREF_RO);
  int settingVal = prefs.getInt(setting, 0); // default = 0
  prefs.end();
  return settingVal;
}


/*
 * Store a persistent preference setting
 */
void putSetting(const char* setting, int settingVal) {
  prefs.begin(settings, PREF_RW);
  prefs.putInt(setting, settingVal);
  prefs.end();
}


/*
 * Load the streamsX array with streams that are saved in prefs object
 */
void populateStreams(void) {  
  // This function will initialize default streams and timer when nvs is blank
  // You can manually re-load the defaults by pulling STREAM_PIN low at boot.

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
    
    // set default timer values
    putSetting(timerOn, 1);   // timer enabled
    putSetting(timerVal, 0);  // 1 hour 
  }

  // fill steamsX with names and urls from prefs
  for (int item=0; item < TOTAL_ITEMS; item++) {
    prefs.begin(stream_item[item], PREF_RW); 
    // check for key existence and place a blank string if missing
    if (!prefs.isKey(stream_type[TYPE_TAG])) prefs.putString(stream_type[TYPE_TAG], "");
    if (!prefs.isKey(stream_type[TYPE_URL])) prefs.putString(stream_type[TYPE_URL], "");
    // get the values and place in streamsX array
    putStreams(item, prefs.getString(stream_type[TYPE_TAG]).c_str(), 
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
    prefs.putString(stream_type[TYPE_TAG], getStreamsTag(item));
    prefs.putString(stream_type[TYPE_URL], getStreamsUrl(item));
    prefs.end();
  }
}


/*
 * Put the stream tag and url strings into the streamsX array
 */
void putStreams(int index, const char* tag, const char* url) {
  strncpy( streamsX + (index * STREAM_ITEM_SIZE), tag, STREAM_ELEMENT_SIZE );
  strncpy( streamsX + (index * STREAM_ITEM_SIZE + STREAM_ELEMENT_SIZE), url, STREAM_ELEMENT_SIZE );
}


/*
 * Get the name tag string from the streamsX array at index
 */
char* getStreamsTag(int index) { // index = 0..TOTAL_ITEMS-1
  return streamsX + (index * STREAM_ITEM_SIZE);
}


/*
 * Get the url string from the streamsX array at index
 */
char* getStreamsUrl(int index) { // index = 0..TOTAL_ITEMS-1
  return streamsX + (index * STREAM_ITEM_SIZE + STREAM_ELEMENT_SIZE);
}


/*
 * Create a version tag derived from the compile time
 */
String version(void) {
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
 * Put CPU to sleep. Reboot when wake up requested.
 */
void systemPowerDown(void) {
  oled.print(F("SYSTEM POWER DOWN\nClick to Restart\n\nv.")); // notification
  oled.print(version());
  icystream.end();          // stop stream download
  systemStreaming = false;  // set state signals
  systemIsSleeping = true;
  esp_sleep_enable_ext0_wakeup((gpio_num_t) ROTARY_ENCODER_BUTTON_PIN, LOW); // set the restart signal
  esp_wifi_stop();          // shut down wifi
  delay(OLED_TIMER);
  oled.clear();
  esp_light_sleep_start();  // put cpu to sleep

  // CPU is now in sleep mode. ZZZzzzz

  // code will resume here when restart signal is asserted
  oled.println(F("SYSTEM RESTART")); // notification
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_restart();            // reboot cpu
}


/*
 * Wipe the NVS memory (wifi, prefs, etc.)
 */
void wipeNVS(void) {
  oled.clear();
  oled.print(F("NVS\nClearing Memory\n"));
  nvs_flash_erase();      // erase the NVS partition and...
  nvs_flash_init();       // initialize the NVS partition.
  oled.println(F("Complete"));
  oled.printf("Remove D%d jumper", NVS_CLR_PIN);
  while (true);           // loop forever
}


/*
 * Load the following default stream name tags and urls into the prefs object  
 */
void initializeStreams(void) {
  // This function clobbers any user entered streams.

  oled.clear();
  oled.println(F("INITIALIZE"));
  oled.print(F("Loading default\nstreams...\n"));
  
  // Default Streams (49 char name + 49 char url, +nulls)
  char stream_data[72][50] = { // 72 elements of 50 chars (72/2=36 streams)
    // 1-10
    STREAMTAG_1,  STREAMURL_1,  STREAMTAG_2,  STREAMURL_2,
    STREAMTAG_3,  STREAMURL_3,  STREAMTAG_4,  STREAMURL_4,
    STREAMTAG_5,  STREAMURL_5,  STREAMTAG_6,  STREAMURL_6,
    STREAMTAG_7,  STREAMURL_7,  STREAMTAG_8,  STREAMURL_8,
    STREAMTAG_9,  STREAMURL_9,  STREAMTAG_10, STREAMURL_10,
    STREAMTAG_11, STREAMURL_11, STREAMTAG_12, STREAMURL_12,
    STREAMTAG_13, STREAMURL_13, STREAMTAG_14, STREAMURL_14,
    STREAMTAG_15, STREAMURL_15, STREAMTAG_16, STREAMURL_16,
    STREAMTAG_17, STREAMURL_17, STREAMTAG_18, STREAMURL_18,
    STREAMTAG_19, STREAMURL_19, STREAMTAG_20, STREAMURL_20,
    STREAMTAG_21, STREAMURL_21, STREAMTAG_22, STREAMURL_22,
    STREAMTAG_23, STREAMURL_23, STREAMTAG_24, STREAMURL_24,
    STREAMTAG_25, STREAMURL_25, STREAMTAG_26, STREAMURL_26,
    STREAMTAG_27, STREAMURL_27, STREAMTAG_28, STREAMURL_28,
    STREAMTAG_29, STREAMURL_29, STREAMTAG_30, STREAMURL_30,
    STREAMTAG_31, STREAMURL_31, STREAMTAG_32, STREAMURL_32,
    STREAMTAG_33, STREAMURL_33, STREAMTAG_34, STREAMURL_34,
    STREAMTAG_35, STREAMURL_35, STREAMTAG_36, STREAMURL_36
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
  putSetting(listened, 0);  // default to first stream
}

// eof