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

Preferences prefs;                       // persistent data store
char stream_item[25][2] = {              // prefs stream item names
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", 
  "A", "B", "C", "D", "E", "F", "G", "H", "J", "K", "L", "M", "N", "P", "R"};
char stream_type[2][4] = {"tag", "url"}; // prefs key names

// Types for the array streamsX[] and the preference stream_type[] 
#define TYPE_TAG 0
#define TYPE_URL 1

#define HOURS_24 86400000  // milliseconds in a day

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
      WiFiManagerParameter tag0("tag0", "Stream 1 Name", streamsGetTag(0), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag0);
      WiFiManagerParameter url0("url0", "URL 1", streamsGetUrl(0), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url0);

      WiFiManagerParameter tag1("tag1", "Stream 2 Name", streamsGetTag(1), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag1);
      WiFiManagerParameter url1("url1", "URL 2", streamsGetUrl(1), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url1);

      WiFiManagerParameter tag2("tag2", "Stream 3 Name", streamsGetTag(2), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag2);
      WiFiManagerParameter url2("url2", "URL 3", streamsGetUrl(2), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url2);
      
      WiFiManagerParameter tag3("tag3", "Stream 4 Name", streamsGetTag(3), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag3);
      WiFiManagerParameter url3("url3", "URL 4", streamsGetUrl(3), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url3);

      WiFiManagerParameter tag4("tag4", "Stream 5 Name", streamsGetTag(4), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag4);
      WiFiManagerParameter url4("url4", "URL 5", streamsGetUrl(4), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url4);

      WiFiManagerParameter tag5("tag5", "Stream 6 Name", streamsGetTag(5), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag5);
      WiFiManagerParameter url5("url5", "URL 6", streamsGetUrl(5), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url5);

      WiFiManagerParameter tag6("tag6", "Stream 7 Name", streamsGetTag(6), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag6);
      WiFiManagerParameter url6("url6", "URL 7", streamsGetUrl(6), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url6);

      WiFiManagerParameter tag7("tag7", "Stream 8 Name", streamsGetTag(7), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag7);
      WiFiManagerParameter url7("url7", "URL 8", streamsGetUrl(7), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url7);

      WiFiManagerParameter tag8("tag8", "Stream 9 Name", streamsGetTag(8), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag8);
      WiFiManagerParameter url8("url8", "URL 9", streamsGetUrl(8), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url8);

      WiFiManagerParameter tag9("tag9", "Stream 10 Name", streamsGetTag(9), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag9);
      WiFiManagerParameter url9("url9", "URL 10", streamsGetUrl(9), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url9);

      WiFiManagerParameter tag10("tag10", "Stream 11 Name", streamsGetTag(10), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag10);
      WiFiManagerParameter url10("url10", "URL 11", streamsGetUrl(10), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url10);

      WiFiManagerParameter tag11("tag11", "Stream 12 Name", streamsGetTag(11), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag11);
      WiFiManagerParameter url11("url11", "URL 12", streamsGetUrl(11), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url11);

      WiFiManagerParameter tag12("tag12", "Stream 13 Name", streamsGetTag(12), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag12);
      WiFiManagerParameter url12("url12", "URL 13", streamsGetUrl(12), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url12);
      
      WiFiManagerParameter tag13("tag13", "Stream 14 Name", streamsGetTag(13), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag13);
      WiFiManagerParameter url13("url13", "URL 14", streamsGetUrl(13), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url13);

      WiFiManagerParameter tag14("tag14", "Stream 15 Name", streamsGetTag(14), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag14);
      WiFiManagerParameter url14("url14", "URL 15", streamsGetUrl(14), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url14);

      WiFiManagerParameter tag15("tag15", "Stream 16 Name", streamsGetTag(15), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag15);
      WiFiManagerParameter url15("url15", "URL 16", streamsGetUrl(15), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url15);

      WiFiManagerParameter tag16("tag16", "Stream 17 Name", streamsGetTag(16), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag16);
      WiFiManagerParameter url16("url16", "URL 17", streamsGetUrl(16), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url16);

      WiFiManagerParameter tag17("tag17", "Stream 18 Name", streamsGetTag(17), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag17);
      WiFiManagerParameter url17("url17", "URL 18", streamsGetUrl(17), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url17);

      WiFiManagerParameter tag18("tag18", "Stream 19 Name", streamsGetTag(18), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag18);
      WiFiManagerParameter url18("url18", "URL 19", streamsGetUrl(18), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url18);

      WiFiManagerParameter tag19("tag19", "Stream 20 Name", streamsGetTag(19), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag19);
      WiFiManagerParameter url19("url19", "URL 20", streamsGetUrl(19), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url19);

      WiFiManagerParameter tag20("tag20", "Stream 21 Name", streamsGetTag(20), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag20);
      WiFiManagerParameter url20("url20", "URL 21", streamsGetUrl(20), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url20);

      WiFiManagerParameter tag21("tag21", "Stream 22 Name", streamsGetTag(21), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag21);
      WiFiManagerParameter url21("url21", "URL 22", streamsGetUrl(21), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url21);

      WiFiManagerParameter tag22("tag22", "Stream 23 Name", streamsGetTag(22), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag22);
      WiFiManagerParameter url22("url22", "URL 23", streamsGetUrl(22), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url22);
      
      WiFiManagerParameter tag23("tag23", "Stream 24 Name", streamsGetTag(23), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag23);
      WiFiManagerParameter url23("url23", "URL 24", streamsGetUrl(23), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url23);

      WiFiManagerParameter tag24("tag24", "Stream 25 Name", streamsGetTag(24), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&tag24);
      WiFiManagerParameter url24("url24", "URL 25", streamsGetUrl(24), STREAM_ELEMENT_SIZE-1);
      wifiMan.addParameter(&url24);

      oled.clear();
      oled.print(F("Portal is On-Line\nSSID: "));
      oled.print(portalName);
      oled.print(F("\nIP  : 192.168.4.1"));

      wifiMan.startConfigPortal(portalName); 

      // Store data from portal into streamsX
      streamsPut(0, tag0.getValue(), url0.getValue()); 
      streamsPut(1, tag1.getValue(), url1.getValue());
      streamsPut(2, tag2.getValue(), url2.getValue());
      streamsPut(3, tag3.getValue(), url3.getValue());
      streamsPut(4, tag4.getValue(), url4.getValue());
      streamsPut(5, tag5.getValue(), url5.getValue());
      streamsPut(6, tag6.getValue(), url6.getValue());
      streamsPut(7, tag7.getValue(), url7.getValue());
      streamsPut(8, tag8.getValue(), url8.getValue());
      streamsPut(9, tag9.getValue(), url9.getValue());
      streamsPut(10, tag10.getValue(), url10.getValue()); 
      streamsPut(11, tag11.getValue(), url11.getValue());
      streamsPut(12, tag12.getValue(), url12.getValue());
      streamsPut(13, tag13.getValue(), url13.getValue());
      streamsPut(14, tag14.getValue(), url14.getValue());
      streamsPut(15, tag15.getValue(), url15.getValue());
      streamsPut(16, tag16.getValue(), url16.getValue());
      streamsPut(17, tag17.getValue(), url17.getValue());
      streamsPut(18, tag18.getValue(), url18.getValue());
      streamsPut(19, tag19.getValue(), url19.getValue());
      streamsPut(20, tag20.getValue(), url20.getValue()); 
      streamsPut(21, tag21.getValue(), url21.getValue());
      streamsPut(22, tag22.getValue(), url22.getValue());
      streamsPut(23, tag23.getValue(), url23.getValue());
      streamsPut(24, tag24.getValue(), url24.getValue());

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
unsigned long sleepTimer = HOURS_24; // 24 hours


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
        // Set sleep timer when volume is zero
        oled.clear();
        oled.println("SET TIMER");
        setSleepTime();  // choose timer minutes

        oled.clear();
        if (sleepTimer == 0) {
          // cancel timer
          timerRunning = false;
          oled.println("CANCEL TIMER");
        }
        else {
          timerRunning = true;
          oled.println("TIMER SET"); 
          oled.println(sleepTimer);
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

// Choose sleep time in minutes
void setSleepTime(void) {

  // INCOMPLETE 
  // **for now, just set timer to 60 mins if it is off, or
  // **cancel it by setting to zero mins if it is running.

  // set to 0 (zero) to disable timer
  /*
    6000  = 6    secs
   60000  = 60        = 1 minutes
  600000  = 600       = 10   mins
 6000000  = 6000 s    = 100  mins
 
 43,200,000 ms = 12 hour
 86,400,000 ms = 24 hr
 
       900,000 = 15 mins
     1,800,000 = 30
     3,600,000 = 1 hour
     7,200,000 = 2 hour
*/
  //sleepTimer = 15000;    // 15 secs for testing
  //sleepTimer = 3600000;    // 60 mins

  sleepTimer = (timerRunning ? 0 : 3600000);
  sleepStartTime = millis();
}

// display timer, signal, volume level
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

    // Concatenate the result in "HH:MM:SS" format
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
    // default streams, 25 totalItems
    "Psyndora Chillout","http://cast.magicstreams.gr:9125/stream",
    "Detroit Industrial Underground", "http://138.197.0.4:8000/stream",
    "Gothville", "http://gothville.radio:8000/stream",
    "Synphaera Radio","http://ice2.somafm.com/synphaera-128-mp3",
    "Mangled Web Radio", "http://144.126.151.19:8000/mp3",
    "Rare 80s Music", "http://209.9.238.4:9844/",
    "Ambient Radio", "http://uk2.internet-radio.com:8171/stream",
    "Dimensione Relax", "http://51.161.115.200:8012/stream",
    "Lounge Radio", "http://fr1.streamhosting.ch:80/lounge128.mp3",
    "Skylark Stream", "http://uk2.internet-radio.com:8164/listen.ogg",

    "Radio Play Emotions", "http://5.39.82.157:8054/stream",
    "","",
    "","",
    "","",
    "","",
    "","",
    "","",
    "","",
    "","",
    "","",

    "Best of Art Bell","http://108.161.128.117:8050",
    "WDBF","http://s4.voscast.com:8296",
    "WTCS","http://96.31.83.86:8001",
    "First Amendment Radio","http://198.178.123.8:7862",
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
