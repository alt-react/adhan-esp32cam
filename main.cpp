#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <WiFiManager.h>
#include <PrayerTimes.h>

// --- Hardware Pins ---
#define OLED_SDA 14
#define OLED_SCL 15
#define LED_PIN 13  // Status LED pin (Change to 4 if you want to use the onboard flash LED)

// --- Display Configuration ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- State Variables ---
float latitude = 0.0;
float longitude = 0.0;
float tzOffsetHours = 0.0;
long tzOffsetSecs = 0;
int lastDay = -1;

int prayerTimesMins[5] = {0}; 
const char* prayerNames[5] = {"Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"};

bool warningPlayed[5] = {false};
bool adhanPlayed[5] = {false};

// --- LED Blink Engine State ---
enum AlertState { LED_OFF, WARNING_BLINK, ADHAN_BLINK };
AlertState currentAlert = LED_OFF;
unsigned long alertStartTime = 0;
unsigned long lastBlinkToggle = 0;
bool ledState = false;

const unsigned long ALERT_DURATION = 10000; // Total time the LED flashes per event (10 seconds)
const unsigned int WARNING_INTERVAL = 500;  // Slow blink for 10-min warning (500ms)
const unsigned int ADHAN_INTERVAL = 100;    // Rapid strobe for actual Adhan (100ms)

// --- Core Functions ---

bool fetchLocationData() {
    HTTPClient http;
    http.begin("http://ip-api.com/json/?fields=lat,lon,offset,status");
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        String payload = http.getString();
        StaticJsonDocument<512> doc;
        deserializeJson(doc, payload);
        
        if (String(doc["status"].as<const char*>()) == "success") {
            latitude = doc["lat"];
            longitude = doc["lon"];
            tzOffsetSecs = doc["offset"];
            tzOffsetHours = tzOffsetSecs / 3600.0;
            return true;
        }
    }
    return false;
}

void calculateOfflinePrayerTimes(struct tm &timeinfo) {
    int year = timeinfo.tm_year + 1900;
    int month = timeinfo.tm_mon + 1;
    int day = timeinfo.tm_mday;

    set_calc_method(ISNA); 
    
    double pTimes[7];
    get_prayer_times(year, month, day, latitude, longitude, tzOffsetHours, pTimes);
    
    prayerTimesMins[0] = (int)(pTimes[0] * 60); 
    prayerTimesMins[1] = (int)(pTimes[2] * 60); 
    prayerTimesMins[2] = (int)(pTimes[3] * 60); 
    prayerTimesMins[3] = (int)(pTimes[5] * 60); 
    prayerTimesMins[4] = (int)(pTimes[6] * 60); 
}

void updateDisplay(int h, int m, int nextIndex, int minsLeft) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    display.setTextSize(1);
    for (int i = 0; i < 5; i++) {
        display.setCursor(0, i * 12);
        if (i == nextIndex) display.print(">"); 
        else display.print(" ");
        
        display.print(prayerNames[i]);
        display.print(" ");
        
        int ph = prayerTimesMins[i] / 60;
        int pm = prayerTimesMins[i] % 60;
        if (ph < 10) display.print("0");
        display.print(ph);
        display.print(":");
        if (pm < 10) display.print("0");
        display.print(pm);
    }
    
    display.setCursor(72, 5);
    display.setTextSize(2);
    if (h < 10) display.print("0");
    display.print(h);
    display.print(":");
    if (m < 10) display.print("0");
    display.print(m);
    
    display.setTextSize(1);
    display.setCursor(72, 30);
    display.print("Next:");
    display.setCursor(72, 42);
    display.print(prayerNames[nextIndex]);
    
    display.setCursor(72, 54);
    int hLeft = minsLeft / 60;
    int mLeft = minsLeft % 60;
    display.print("-");
    if (hLeft > 0) { display.print(hLeft); display.print("h "); }
    display.print(mLeft);
    display.print("m");
    
    display.display();
}

// Non-blocking processing engine for managing visual LED sequences
void handleLEDEngine() {
    if (currentAlert == LED_OFF) {
        return;
    }

    // Shut down flashing sequence if time window expires
    if (millis() - alertStartTime >= ALERT_DURATION) {
        currentAlert = LED_OFF;
        digitalWrite(LED_PIN, LOW);
        ledState = false;
        return;
    }

    unsigned int currentInterval = (currentAlert == WARNING_BLINK) ? WARNING_INTERVAL : ADHAN_INTERVAL;

    // Toggle pin step based on elapsed interval cadence
    if (millis() - lastBlinkToggle >= currentInterval) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        lastBlinkToggle = millis();
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }
    
    // --- Step 1: Captive Portal ---
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 10);
    display.println("Connect to Wi-Fi:");
    display.println("ESP32-Adhan-Setup");
    display.println("IP: 192.168.4.1");
    display.display();

    WiFiManager wm;
    if (!wm.autoConnect("ESP32-Adhan-Setup")) {
        ESP.restart(); 
    }

    // --- Step 2: Fetch Region Data ---
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Connected.");
    display.println("Getting IP Data...");
    display.display();
    
    while (!fetchLocationData()) { delay(1000); }

    // --- Step 3: NTP Time Sync ---
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Syncing Time...");
    display.display();

    configTime(tzOffsetSecs, 0, "pool.ntp.org");
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo, 5000)) {
        delay(500); 
    }

    calculateOfflinePrayerTimes(timeinfo);
    lastDay = timeinfo.tm_mday;

    // --- Step 4: Sever Internet Operations ---
    WiFi.disconnect(true, false); 
    WiFi.mode(WIFI_OFF);          
}

void loop() {
    // Process lighting tasks continuously at maximum frequency execution loop
    handleLEDEngine(); 
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return; 
    
    int currentMins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    
    // Midnight reset
    if (timeinfo.tm_mday != lastDay) {
        if (lastDay != -1) {
            calculateOfflinePrayerTimes(timeinfo);
            for (int i = 0; i < 5; i++) {
                warningPlayed[i] = false;
                adhanPlayed[i] = false;
            }
        }
        lastDay = timeinfo.tm_mday;
    }
    
    int nextIndex = -1;
    for (int i = 0; i < 5; i++) {
        if (currentMins < prayerTimesMins[i]) {
            nextIndex = i;
            break;
        }
    }
    
    int targetIndex = (nextIndex == -1) ? 0 : nextIndex;
    int targetTime = prayerTimesMins[targetIndex];
    if (nextIndex == -1) { targetTime += 24 * 60; }
    
    int minsLeft = targetTime - currentMins;
    
    // Alarm Triggers targeting specific timing slots
    for (int i = 0; i < 5; i++) {
        // 10-Minute Warning (Slow Flash Initialization)
        if (!warningPlayed[i] && currentMins == (prayerTimesMins[i] - 10)) {
            currentAlert = WARNING_BLINK;
            alertStartTime = millis();
            lastBlinkToggle = millis();
            warningPlayed[i] = true;
        }
        // Actual Adhan Time (Fast Strobe Initialization)
        if (!adhanPlayed[i] && currentMins == prayerTimesMins[i]) {
            currentAlert = ADHAN_BLINK;
            alertStartTime = millis();
            lastBlinkToggle = millis();
            adhanPlayed[i] = true;
        }
    }
    
    static unsigned long lastOledUpdate = 0;
    if (millis() - lastOledUpdate > 1000) {
        updateDisplay(timeinfo.tm_hour, timeinfo.tm_min, targetIndex, minsLeft);
        lastOledUpdate = millis();
    }
    
    delay(10); // Keeps processing loop tight for precise LED timing resolution
}
