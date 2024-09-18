/**
 * esp32-stream-system.cpp
 * jul-sep 2024
 * 
 * Copyright (C)2024, Steven R Stuart
 * 
 * This program outputs mp3 internet stream to max98357a i2c audio device(s).
 * Turn the knob to set volume. Press the button to change station.
 * Set volume to 0 then press button to set timer. Do again to cancel.
 * Two clicks within 3 seconds with zero volume halts stream.
 * Hold PORTAL_PIN low on reset to launch wifi configuration portal.
 * Hold STREAM_PIN low on reset to load and store default stream data.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
//#include "esp_wifi.h"
#include <WiFiManager.h>
#include "AudioTools.h"
#include "AudioCodecs/CodecMP3Helix.h"
#include "AiEsp32RotaryEncoder.h"
#include <Wire.h>
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"

#define ROTARY_ENCODER_A_PIN 33      // clk
#define ROTARY_ENCODER_B_PIN 32      // dt  
#define ROTARY_ENCODER_BUTTON_PIN 34 // sw
#define ROTARY_ENCODER_STEPS 2       // 1, 2 or 4

#define I2C_ADDRESS 0x3C   // ssd1306 oled
#define SDA_PIN 18         // i2c data
#define SCL_PIN 19         // i2c clock

#define PORTAL_PIN 15      // enable wifi portal when low on reset      
#define STREAM_PIN 16      // set default streams when low on reset

#define OLED_TIMER 5000    // display timeout in milliseconds
#define SLEEP_TIME 3600000 // one hour in milliseconds

// System is hard coded to 25 streams (TOTAL_ITEMS)
// Each item in the streamsX array contains a text name and a url,
// the name and url elements are each STREAM_ELEMENT_SIZE in length
#define STREAM_ITEM_SIZE 100          // line item width
#define STREAM_ELEMENT_SIZE 50        // max length of name, url
#define TOTAL_ITEMS 25                // number of line items in streamsX
int currentIndex = 0;                 // stream pointer 
char streamsX[TOTAL_ITEMS * STREAM_ITEM_SIZE]; // names & urls of stations

const char* portalName = "NETRADIO";  // portal ssid
const char* settings = "settings";    // general purpose namespace in prefs
const char* listened = "listened";    // settings key of last listened to stream
const char* audiovol = "volume";      // settings key of audio level

AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(
    ROTARY_ENCODER_A_PIN,        // ky040 clk
    ROTARY_ENCODER_B_PIN,        //   "   dt
    ROTARY_ENCODER_BUTTON_PIN,   //   "   sw
    -1,                          // Vcc pin -1 = not used
    ROTARY_ENCODER_STEPS         
);

void IRAM_ATTR readEncoderISR() {  // interrupt service routine
    rotaryEncoder.readEncoder_ISR();
}

// Instatiate the objects
URLStream urlstream;
I2SStream i2s;
VolumeStream volume(i2s);
EncodedAudioStream mp3decode(&volume, new MP3DecoderHelix()); // Decoder stream
StreamCopy copier(mp3decode, urlstream); // copy urlstream to decoder
SSD1306AsciiWire oled;
Preferences prefs;              // persistent data store

char stream_item[TOTAL_ITEMS][2] = {     // prefs stream item names
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", 
  "A", "B", "C", "D", "E", "F", "G", "H", "J", "K", 
  "L", "M", "N", "P", "R"
};
char stream_type[2][4] = {"tag", "url"}; // prefs key names
#define TYPE_TAG 0  // Type references
#define TYPE_URL 1  

// Must have enough unique element names for TOTAL_ITEMS
const char* tagElement[] = {    // portal html element tags
  "tag0",  "tag1",  "tag2",  "tag3",  "tag4", 
  "tag5",  "tag6",  "tag7",  "tag8",  "tag9", 
  "tag10", "tag11", "tag12", "tag13", "tag14", 
  "tag15", "tag16", "tag17", "tag18", "tag19",
  "tag20", "tag21", "tag22", "tag23", "tag24"
};
const char* nameElement[] = {   // portal html tag field titles
  "Name 1",  "Name 2",  "Name 3",  "Name 4",  "Name 5",  
  "Name 6",  "Name 7",  "Name 8",  "Name 9",  "Name 10", 
  "Name 11", "Name 12", "Name 13", "Name 14", "Name 15", 
  "Name 16", "Name 17", "Name 18", "Name 19", "Name 20", 
  "Name 21", "Name 22", "Name 23", "Name 24", "Name 25"
};
const char* urlElement[] = {    // portal url element tags/titles
  "URL_1",  "URL_2",  "URL_3",  "URL_4",  "URL_5",  
  "URL_6",  "URL_7",  "URL_8",  "URL_9",  "URL_10", 
  "URL_11", "URL_12", "URL_13", "URL_14", "URL_15", 
  "URL_16", "URL_17", "URL_18", "URL_19", "URL_20", 
  "URL_21", "URL_22", "URL_23", "URL_24", "URL_25"
};

// Function prototypes
void populateStreams(void);
void populatePrefs(void);
void streamsPut(int, const char*, const char*);
char* streamsGetTag(int);
char* streamsGetUrl(int);
void initializeStreams(void);
int settingGet(const char*);
void settingPut(const char*, int);
void menuDisplay(int);
bool checkProtocol(int);
void runSleepTimer(bool);
void oledStatusDisplay(void);
String timerTimeLeft(void);


/*
 *
 *    SetUp 
 */
void setup() {

  pinMode(PORTAL_PIN, INPUT_PULLUP);  // call for wifi portal
  pinMode(STREAM_PIN, INPUT_PULLUP);  // load default streams

  // Message port
  Serial.begin(115200);
  Serial.println(F("Stream Player"));
  Serial.print(F("Steven R Stuart,  "));
  Serial.print(F(__DATE__));
  Serial.print(F(" "));
  Serial.println(F(__TIME__));

  // OLED display device
  Wire.begin(SDA_PIN, SCL_PIN);  // start i2c interface
  Wire.setClock(400000L);
  oled.begin(&Adafruit128x32, I2C_ADDRESS);  // start oled
  oled.setFont(System5x7);
  oled.clear();
  oled.print(F("Stream Player\nSteven R Stuart\nW8AN\n"));

  // Reload the default streams if desired
  if (digitalRead(STREAM_PIN) == LOW) {
    initializeStreams();          // get the default streams
  }
  populateStreams();              // fill streamsX array from prefs
  currentIndex = settingGet(listened);  // get index of previous listened stream

  // Configure Wifi system
  WiFiManager wifiMan;            // instatiate wifi object
  wifiMan.setDebugOutput(false);  // true if you want send to serial debug
  // wifiMan.resetSettings();     // force a wifi configuration portal

  // Tries to connect to the last known network. Launches a
  // captive portal if the connection fails or the timeout is reached.
  if(wifiMan.autoConnect(portalName)) {   // portal at 192.168.4.1
    if (digitalRead(PORTAL_PIN) == LOW) { // force the portal
      Serial.println("User requested configuration portal");
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
        Serial.println("Failed to get WiFi config");
        oled.clear();
        oled.println("WiFi Error");
        oled.print("CONFIG FAIL");
      }

      //  set up portal parameters
      WiFiManagerParameter* tagElementParam[TOTAL_ITEMS]; // Array to store pointers to WiFiManagerParameter tag objects
      WiFiManagerParameter* urlElementParam[TOTAL_ITEMS]; // and WiFiManagerParameter url objects
      for (int i=0; i<TOTAL_ITEMS; i++) {
        tagElementParam[i] = new WiFiManagerParameter{tagElement[i], nameElement[i], streamsGetTag(i), STREAM_ELEMENT_SIZE-1};
        wifiMan.addParameter(tagElementParam[i]);
        urlElementParam[i] = new WiFiManagerParameter{urlElement[i], urlElement[i], streamsGetUrl(i), STREAM_ELEMENT_SIZE-1};
        wifiMan.addParameter(urlElementParam[i]);
      }

      oled.clear();
      oled.print(F("Portal is On-Line\nSSID: "));
      oled.print(portalName);
      oled.print(F("\nIP  : 192.168.4.1"));

      wifiMan.startConfigPortal(portalName); // get user data from a wifi connected device

      // Store data fields from portal into streamsX array
      for (int i=0; i<TOTAL_ITEMS; i++) {
        streamsPut(i, tagElementParam[i]->getValue(), urlElementParam[i]->getValue()); 
      }

      // Update the prefs object from streamsX array for persistence
      populatePrefs();  
      oled.clear();
    }
  } 
  else {
    Serial.println("Failed to connect to WiFi.");
    oled.clear();
    oled.println("WiFi Error");
    oled.print("CONNECT FAIL");
    ESP.restart();
  }

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
  volume.setVolume(settingGet(audiovol) / 100.0);  // get saved volume
}


/*
 * Set up some loop globals
 */

// oled screen status and blanking timing
bool displayOn = true;
unsigned long oledStartTime; 
unsigned long oledCurrentTime = 0;   

// stream status and timing
bool systemStreaming = false;
unsigned long streamStartTime = millis(); 
unsigned long streamCurrentTime = 0; 

// menu vars
bool menuOpen = false;
int menuIndex = currentIndex;
long volumePos, volLevel;

// sleep timer status and timing
bool systemSleeping = false;
bool timerRunning = false;
unsigned long sleepStartTime = millis();
unsigned long sleepCurrentTime = 0;
//unsigned long sleepTimer;


/*
 *
 *    Loop
 */
void loop() {
  if (systemSleeping) {
    
    if (rotaryEncoder.isEncoderButtonClicked() ||
        rotaryEncoder.encoderChanged()) {

      // wake from sleep
      systemSleeping = false;
      runSleepTimer(timerRunning);  // reset timer

      oled.clear();
      oled.println("WAKE UP");
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
        settingPut(listened, currentIndex);  // save selection
        systemStreaming = true;
      }

      else { 
        // url string length is too short
        currentIndex = settingGet(listened); // get previous stream
        menuIndex = currentIndex;  // reset menu display pointer
        oled.clear();
        oled.println("ERROR");     // notify user
        oled.println("Missing URL");
        oled.println();
        oled.print("Reverting");
      }

      displayOn = true;
      oledStartTime = millis(); // tickle the display timer
    }

    if (rotaryEncoder.isEncoderButtonClicked()) { 
      // button was pressed

      if (settingGet(audiovol) == 0) { 
        // volume is zero

        // set\reset sleep timer or power down
        oled.clear();
        sleepCurrentTime = millis();
        if (timerRunning & (sleepCurrentTime - sleepStartTime) < 3000) {
          // if user presses button within 3 seconds of timer set
          // and the volume is zero, stop streaming
          urlstream.end();          // stop stream download
          systemStreaming = false;  // set state signals
          systemSleeping = true;
          oled.println("SYSTEM HALT"); // notify user
        }

        else {
          // Set or reset sleep timer when volume is zero

          if (timerRunning) {
            runSleepTimer(false);
            oled.println("CANCEL TIMER");
          }
          else {
            runSleepTimer(true);
            oled.println("TIMER SET"); 
            oled.print(SLEEP_TIME/60000); // convert to minutes
            oled.println(" minutes");  
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
        // menu is not open, so set the volume level
        volLevel = 100 - rotaryEncoder.readEncoder();
        volume.setVolume(volLevel / 100.0);
        settingPut(audiovol, volLevel);  // store the setting
        oledStatusDisplay();  
      }

      displayOn = true;
      oledStartTime = millis(); // tickle the display timer
    }
  } 

  // Turn off oled after a few seconds
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
    }
  }

  if (!systemSleeping && timerRunning) { 
    // system is awake and timer is running
    
    // watch for timeout
    sleepCurrentTime = millis();
    if (sleepCurrentTime - sleepStartTime > SLEEP_TIME) {
//    if (sleepCurrentTime - sleepStartTime > sleepTimer) {

      // timer has expired, go to sleep
      urlstream.end();         // stop stream download
      systemStreaming = false; // set state signals
      systemSleeping = true;

      oled.clear();
      oled.println("SLEEPING");
      displayOn = true;
      oledStartTime = millis(); // tickle the display timer
    }
  }
}

/*
 *
 *    Program Functions
 */

// enable or disable sleep timer
void runSleepTimer(bool enabled) {
  // enabled = true to set, false to clear
  timerRunning = enabled;
  if (enabled) {
    sleepStartTime = millis();  // sleep timer baseline
  }
}

// display the timer, signal, volume level
void oledStatusDisplay(void) {

  int dBm = WiFi.RSSI();
  oled.clear();
  oled.println(streamsGetTag(currentIndex)); // stream name
  if (timerRunning) {
    oled.print("timer : ");
    oled.println(timerTimeLeft());
  }
  oled.print("signal: ");
  oled.print(dBm); 
  if (dBm >= -30)  oled.println(" excellent");
  else if (dBm >= -67 ) oled.println(" good");
    else if (dBm >= -70 ) oled.println(" fair");
      else if (dBm >= -80 ) oled.println(" weak");
        else oled.println(" very weak");
  oled.print("volume: ");
  oled.print(volLevel);
}

String timerTimeLeft(void) {

    sleepCurrentTime = millis();

    // calculate time left in seconds
    long totalSeconds = SLEEP_TIME/1000 - ((sleepCurrentTime - sleepStartTime)/1000);
    int minutes = (totalSeconds % 3600) / 60;
    return String(minutes) + " mins";
}

// Display the stream selection menu
void menuDisplay(int menuIndex) {
  
  oled.clear();
  oled.println(streamsGetTag(currentIndex));  // title line

  // previous line item
  int lineIndex = (menuIndex == 0 ? (TOTAL_ITEMS-1) : menuIndex-1);
  oled.print(" "); // indent
  if (checkProtocol(lineIndex)) oled.println(streamsGetTag(lineIndex));
  else oled.println(lineIndex+1); // print only line number if error
  
  // current line item
  if (checkProtocol(menuIndex)) {  // selection line
    oled.print(">"); // current item pointer
    oled.println(streamsGetTag(menuIndex));
  } 
  else {
    oled.print(" "); // indent
    oled.println(menuIndex+1);
  }
  
  // next line item
  lineIndex = (menuIndex == (TOTAL_ITEMS-1) ? 0 : menuIndex+1);
  oled.print(" "); // indent
  if (checkProtocol(lineIndex)) oled.println(streamsGetTag(lineIndex));
  else oled.print(lineIndex+1);
}

// Return true if the url protocol text is correct
bool checkProtocol(int menuIndex) {
  return strncmp(streamsGetUrl(menuIndex), "http://", 7) == 0;
}

// Retrieve a persistent preference setting
int settingGet(const char* setting) {
  prefs.begin(settings, true); // read only
  int settingVal = prefs.getInt(setting, 0); // default = 0
  prefs.end();
  return settingVal;
}

// Store a persistent preference setting
void settingPut(const char* setting, int settingVal) {
  prefs.begin(settings, false); // not read only
  prefs.putInt(setting, settingVal);
  prefs.end();
}

// Fill the streamsX array with stream data from the prefs object
void populateStreams(void) {  

  Serial.println("Populating stream array from storage");
  // If prefs doesn't exist, this will throw a bunch of 'not found' errors.
  // Initialize the prefs storage by pulling STREAM_PIN low at boot.
  for (int item=0; item < TOTAL_ITEMS; item++) {
    prefs.begin(stream_item[item], true); // true = read only
    streamsPut(item, prefs.getString(stream_type[TYPE_TAG]).c_str(), 
                     prefs.getString(stream_type[TYPE_URL]).c_str());
    prefs.end();

    Serial.print(item);
    Serial.print(" = ");
    Serial.print(streamsGetTag(item));
    Serial.print(" - ");
    Serial.println(streamsGetUrl(item));
  }
}

// Fill the prefs object with data from the streamsX array
void populatePrefs(void) {
  for (int item=0; item < TOTAL_ITEMS; item++) {
    prefs.begin(stream_item[item], false); // false = not readonly
    prefs.clear();
    prefs.putString(stream_type[TYPE_TAG], streamsGetTag(item));
    prefs.putString(stream_type[TYPE_URL], streamsGetUrl(item));
    prefs.end();
  }
}

// Put the stream tag and url strings into the streamsX array
void streamsPut(int index, const char* tag, const char* url) {
  strncpy( streamsX + (index * STREAM_ITEM_SIZE), tag, STREAM_ELEMENT_SIZE );
  strncpy( streamsX + (index * STREAM_ITEM_SIZE + STREAM_ELEMENT_SIZE), url, STREAM_ELEMENT_SIZE );
}

// Get the name tag string from the streamsX array at index
char* streamsGetTag(int index) { // index = 0..9
  return streamsX + (index * STREAM_ITEM_SIZE);
}

// Get the url string from the streamsX array at index
char* streamsGetUrl(int index) { // index = 0..9
  return streamsX + (index * STREAM_ITEM_SIZE + STREAM_ELEMENT_SIZE);
}

// Load the following default stream name tags and urls into the prefs object  
void initializeStreams(void) {

  // This function clobbers any already stored streams.
  Serial.println("Initializing stream array from defaults");

  // 25 Streams (50 char name + 50 char url)
  char stream_data[50][50] = { // 50 elements of 50 chars
    // 1-10, default streams, 25 total items
    "Psyndora Chillout","http://cast.magicstreams.gr:9125",
    "Psyndora Psytrance","http://cast.magicstreams.gr:9111",
    "Radio Play Emotions", "http://5.39.82.157:8054/stream",
    "Rare 80s Music", "http://209.9.238.4:9844/",
    "Skylark Stream", "http://uk2.internet-radio.com:8164/listen.ogg",
    "Synphaera Radio","http://ice2.somafm.com/synphaera-128-mp3",
    "Tecknicky Stream","http://212.96.160.160:8054",
    "Ambient Radio", "http://uk2.internet-radio.com:8171/stream",
    "Best of Art Bell","http://108.161.128.117:8050",
    "Big 80s Station","http://158.69.114.190:8024",
    // 11-20
    "Big Hair Radio","http://192.111.140.11:8508",
    "Classical Bolivariana","http://131.0.136.54:7630",
    "Dark Edge Radio","http://5.35.214.196:8000",
    "Detroit Industrial Underground", "http://138.197.0.4:8000/stream",
    "Dimensione Relax", "http://51.161.115.200:8012/stream",
    "EarthSong Experimental","http://cast3.my-control-panel.com:7084/autodj",
    "First Amendment Radio","http://198.178.123.8:7862",
    "Gothville", "http://gothville.radio:8000/stream",
    "KXFU - RDSN.net","http://184.95.62.170:9788",
    "Lounge Radio", "http://fr1.streamhosting.ch:80/lounge128.mp3",
    // 21-25
    "Mangled Web Radio", "http://144.126.151.19:8000/mp3",
    "Metal Express Radio","http://5.135.154.69:11590",
    "Metal Rock Radio","http://kathy.torontocast.com:2800",
    "Mission Control Radio","http://151.80.42.191:8372",
    "Mr. Liberty Show","http://198.178.123.5:8258"
  };

  for (int item=0; item < TOTAL_ITEMS; item++) {
    // stuff the prefs object with default data
    prefs.begin(stream_item[item], false); // false = read/write
    prefs.clear();
    prefs.putString(stream_type[TYPE_TAG], stream_data[item*2]);
    prefs.putString(stream_type[TYPE_URL], stream_data[item*2+1]);
    prefs.end();

    Serial.print(item);
    Serial.print(" = ");
    Serial.print(stream_data[item*2]);
    Serial.print(" - ");
    Serial.println(stream_data[item*2+1]);
  }
  settingPut(listened, 0);  // set default to first stream
}

// eof

