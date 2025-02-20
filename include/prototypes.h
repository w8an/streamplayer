// Function prototypes
void callbackSaveParams(void);
void callbackMetadata(MetaDataType, const char*, int);
void oledStatusDisplay(void);
void StreamPortalMessage(void);
void wifiPortalMessage(void);
String timerTimeLeft(void);
void displayStreamMenu(int);
void assignTimerValsFromPrefs(void);
void displayTimerEnabledSetting(void);
void displayTimerIsRunningSetting(void);
void timerEnabledText(bool);
void displayTimerValSetting(void);
void displayTimerDurationSetting(void);
void timerDurationText(int);
void changeTimerDuration(void);
unsigned long timerValueToDuration(int);
int timerDurationToValue(unsigned long);
bool checkProtocol(int);
int getSetting(const char*);
void putSetting(const char*, int);
void populateStreams(void);
void populatePrefs(void);
void putStreams(int, const char*, const char*);
char* getStreamsTag(int);
char* getStreamsUrl(int);
void initializeStreams(void);
String version(void);
void systemPowerDown(void);
void wipeNVS(void);

  