/**
 * esp32-stream-system.cpp
 * jul-aug 2024
 * 
 * Copyright (C)2024, Steven R Stuart
 * 
 * This program outputs url stream to max98357a i2c audio device(s).
 * Turn the knob to set volume. Press the button to change station.
 * Set volume to 0 then press button to set timer. Do again to cancel.
 * Hold pin 15 low on reset to launch configuration portal.
 * Hold pin 16 low on reset to store stream data from presets.
 */

/*
  Stream tag (name) and url are stored in the preferences system (prefs)
    for off-line data persistence.
  The system uses the streamsX array when running.
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
#define ROTARY_ENCODER_STEPS 2

#define I2C_ADDRESS 0x3C  // ssd1306 oled
#define SDA_PIN 18        // i2c data
#define SCL_PIN 19        // i2c clock

#define OLED_TIMER 5000   // 5 seconds, display timeout
#define STREAM_TIMER 2000 // 2 seconds, audio wait time after selection    // // deprecate

#define PORTAL_PIN 15     // wifi portal when low on reset      
#define STREAM_PIN 16     // set default streams when held low on reset

// System is hard coded to 25 streams
// Each item in the streamsX array contains a text name and a url
// The name and url elements are each STREAM_ELEMENT_SIZE in length
#define STREAM_ITEM_SIZE 100          // line item width
#define STREAM_ELEMENT_SIZE 50        // max length of name, url
const int totalItems = 25;            // number of line items in streamsX
char streamsX[2500];                  // STREAM_ITEM_SIZE * totalItems
int currentIndex = 0;                 // stream pointer 

const char* portalName = "NETRADIO";  // portal ssid
const char* settings = "settings";    // general purpose namespace in prefs
const char* listened = "listened";    // settings key of last listened to stream
const char* audiovol = "volume";      // settings key of audio level

AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(
    ROTARY_ENCODER_A_PIN, 
    ROTARY_ENCODER_B_PIN, 
    ROTARY_ENCODER_BUTTON_PIN, 
    -1,                          // Vcc pin, -1 = not used
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

char stream_item[25][2] = {     // prefs stream item names
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", 
  "A", "B", "C", "D", "E", "F", "G", "H", "J", "K", "L", "M", "N", "P", "R"};
char stream_type[2][4] = {"tag", "url"}; // prefs key names

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

// Types for the array streamsX[] and the preference stream_type[] 
#define TYPE_TAG 0
#define TYPE_URL 1

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
void setSleepTime(void);
void oledStatusDisplay(void);
String timerTimeLeft(void);


/*
 *
 *    SetUp 
 */
void setup() {

  pinMode(PORTAL_PIN, INPUT_PULLUP);  // call for portal
  pinMode(STREAM_PIN, INPUT_PULLUP);  // default stream set up

  // Message port
  Serial.begin(115200);
  Serial.println(F("--[ Stream Player ]--"));
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
  oled.print(F("Stream Player\nSteven R Stuart"));

  // Reload the default streams if desired
  if (digitalRead(STREAM_PIN) == LOW) {
    initializeStreams(); // get the defaults
  }

  populateStreams();  // fill streamsX array
  currentIndex = settingGet(listened);  // get index of prev listened stream

  // Wifi
  WiFiManager wifiMan;
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
          // show the stored ssid and password if stream pin is also low
          Serial.printf("SSID: %s\n", (char*)conf.sta.ssid);
          Serial.printf("Password: %s\n", (char*)conf.sta.password);
        }
      } 
      else {
        Serial.println("Failed to get WiFi config");
      }

      //  set up portal parameters
      WiFiManagerParameter* tagElementParam[25]; // Array to store pointers to WiFiManagerParameter tag objects
      WiFiManagerParameter* urlElementParam[25]; // and WiFiManagerParameter url objects
      for (int i=0; i<25; i++) {
        tagElementParam[i] = new WiFiManagerParameter{tagElement[i], nameElement[i], streamsGetTag(i), STREAM_ELEMENT_SIZE-1};
        wifiMan.addParameter(tagElementParam[i]);
        urlElementParam[i] = new WiFiManagerParameter{urlElement[i], urlElement[i], streamsGetUrl(i), STREAM_ELEMENT_SIZE-1};
        wifiMan.addParameter(urlElementParam[i]);
      }

      oled.clear();
      oled.print(F("Portal is On-Line\nSSID: "));
      oled.print(portalName);
      oled.print(F("\nIP  : 192.168.4.1"));

      wifiMan.startConfigPortal(portalName); 

      // Store data fields from portal into streamsX array
      for (int i=0; i<25; i++) {
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
    oled.print("WiFi Failed");
    ESP.restart();
  }

  // Keyes KY-040
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, 100, false); // minValue, maxValue, circle?
  rotaryEncoder.setEncoderValue(100-settingGet(audiovol));   // initial volume 
  rotaryEncoder.setAcceleration(25);
  //rotaryEncoder.disableAcceleration();

  // Audio error messages
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);  // Debug, Info, Warning, Error

  // Output stream configuration
  auto config = i2s.defaultConfig(TX_MODE);
  config.pin_bck = 26;  // BCLK  -max98357 pins
  config.pin_ws  = 25;  // LRC
  config.pin_data = 22; // DIN
  i2s.begin(config);

  // Set up i2s based on sampling rate provided by decoder
  mp3decode.begin();

  // Volume control
  volume.begin(config);   // we need to provide the bits_per_sample and channels
  volume.setVolume(settingGet(audiovol) / 100.0);  // get saved volume
}


/*
 * Set up some loop globals
 */

// oled screen blank timing
bool displayOn = true;
unsigned long oledStartTime; 
unsigned long oledCurrentTime = 0;   

// stream startup timing
bool systemStreaming = false;
unsigned long streamStartTime = millis(); 
unsigned long streamCurrentTime = 0; 

// menu vars
bool menuOpen = false;
int menuIndex = currentIndex;
long volumePos, volLevel;

// sleep timer
bool systemSleeping = false;
bool timerRunning = false;
unsigned long sleepStartTime = millis();
unsigned long sleepCurrentTime = 0;
unsigned long sleepTimer;


/*
 *
 *    Loop
 */
void loop() {
  if (systemSleeping) {
    timerRunning = false;
    if (rotaryEncoder.isEncoderButtonClicked() ||
        rotaryEncoder.encoderChanged()) {
      // wake from sleep
      systemSleeping = false;

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
        urlstream.begin(streamsGetUrl(currentIndex));
        settingPut(listened, currentIndex);  // save selection
        systemStreaming = true;
      }
      else { // url string length too short
        currentIndex = settingGet(listened); // get previous stream
        menuIndex = currentIndex;  // reset menu display pointer
        oled.clear();
        oled.println("ERROR");
        oled.println("Missing URL");
        oled.println();
        oled.print("Reverting");
      }
      displayOn = true;
      oledStartTime = millis(); // tickle the display timer
    }

    if (rotaryEncoder.isEncoderButtonClicked()) { 
      if (settingGet(audiovol) == 0) { 
        // set/reset sleep timer or power down when volume is zero
        oled.clear();

        sleepCurrentTime = millis();
        if (timerRunning & ((sleepCurrentTime - sleepStartTime) < 3000)) {
          // if user presses button within 3 seconds of timer set
          // and volume is zero, stop audio streaming
          systemSleeping = true;
          timerRunning = false;
          urlstream.end();
          systemStreaming = false;
          oled.println("SYSTEM HALT");
        }
        else {
          // Set or reset sleep timer when volume is zero
          setSleepTime();  // set/reset timer minutes

          if (sleepTimer == 0) {
            // cancel timer
            timerRunning = false;
            oled.println("CANCEL TIMER");
          } 
          else {
            timerRunning = true;
            oled.println("TIMER SET"); 
            oled.println("60 minutes");  
          }
        }
        displayOn = true;
        oledStartTime = millis(); // tickle the display timer
      }
      else {
        if (menuOpen) { // close the menu
          menuOpen = false;
          rotaryEncoder.setEncoderValue(volumePos);   // restore volume position 
          currentIndex = menuIndex;  // user chose this stream
          urlstream.end();
          systemStreaming = false;   // signal that another stream is selected
          oledStatusDisplay();
        }
        else { // open the menu, select a stream
          menuOpen = true;
          volumePos = rotaryEncoder.readEncoder(); // get volume setting
          rotaryEncoder.setEncoderValue(50);       // initialize position
          menuDisplay(currentIndex);
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

        if (menuIndex >= (totalItems-1)) menuIndex = 0; // wrap around
        if (menuIndex < 0) menuIndex = totalItems-1;

        menuDisplay(menuIndex);  // show stream selection list
      }
      else { // menu is not open, set volume
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
      if (menuOpen) {
        menuOpen = false;
        menuIndex = currentIndex;  // reset menu display pointer
        rotaryEncoder.setEncoderValue(volumePos);   // restore volume position 
      }
      oled.clear();  // blank the display
      displayOn = false;
    }
  }

  if (!systemSleeping && timerRunning) { // awake and timer is running
    sleepCurrentTime = millis();
    if (sleepCurrentTime - sleepStartTime > sleepTimer) {
      // time to go to sleep
      systemSleeping = true;
      timerRunning = false;
      urlstream.end();
      systemStreaming = false;

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

// set sleep time to 60 minutes
void setSleepTime(void) {
  /* millisecs
       900,000 = 15 mins
     1,800,000 = 30 mins
     3,600,000 = 1 hour
     7,200,000 = 2 hour
  */
  // set to 0 (zero) to disable timer
  sleepTimer = (timerRunning ? 0 : 3600000); // set timer or reset if running
  sleepStartTime = millis();  // sleep timer baseline
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
    long totalSeconds = sleepTimer/1000 - ((sleepCurrentTime - sleepStartTime)/1000);

    // Calculate hours, minutes, and seconds
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;

    // Format each part as two digits
    String hoursStr = (hours < 10) ? "0" + String(hours) : String(hours);
    String minutesStr = (minutes < 10) ? "0" + String(minutes) : String(minutes);
    String secondsStr = (seconds < 10) ? "0" + String(seconds) : String(seconds);

    // Result in "HH:MM:SS" format
    //return hoursStr + ":" + minutesStr + ":" + secondsStr;
    return String(minutes) + " mins";
}

// Display the stream selection menu
void menuDisplay(int menuIndex) {
  
  oled.clear();
  oled.println(streamsGetTag(currentIndex));  // title line

  // previous line item
  int lineIndex = (menuIndex == 0 ? (totalItems-1) : menuIndex-1);
  oled.print(" ");
  if (checkProtocol(lineIndex)) oled.println(streamsGetTag(lineIndex));
  else oled.println(lineIndex+1);
  
  // current line item
  if (checkProtocol(menuIndex)) {  // selection line
    oled.print(">");
    oled.println(streamsGetTag(menuIndex));
  } 
  else {
    oled.print(" ");
    oled.println(menuIndex+1);
  }
  
  // next line item
  lineIndex = (menuIndex == (totalItems-1) ? 0 : menuIndex+1);
  oled.print(" ");
  if (checkProtocol(lineIndex)) oled.println(streamsGetTag(lineIndex));
  else oled.print(lineIndex+1);
}

// Return true if the url protocol is correct
bool checkProtocol(int menuIndex) {
  return strncmp(streamsGetUrl(menuIndex), "http://", 7) == 0;
}

// Retrieve a persistent setting
int settingGet(const char* setting) {
  prefs.begin(settings, true); // read only
  int settingVal = prefs.getInt(setting, 0); // default = 0
  prefs.end();
  return settingVal;
}

// Store a persistent setting
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
  for (int item=0; item < totalItems; item++) {
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
  for (int item=0; item < totalItems; item++) {
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

// Place the following stream name tags and urls into the prefs object  
void initializeStreams(void) {

  // This function clobbers any already stored streams.
  Serial.println("Initializing stream array from defaults");

  // 25 Streams (50 char name + 50 char url)
  char stream_data[50][50] = { // 50 elements of 50 chars
    // 1-10, default streams, 25 totalItems
    "Psyndora Chillout","http://cast.magicstreams.gr:9125/stream",
    "Radio Bloodstream","http://uk1.internet-radio.com:8294/live.m3u",
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

  for (int item=0; item < totalItems; item++) {
    // stuff the prefs object
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

