#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NonBlockingRTTTL.h>
#include <time.h>
#include <WiFiManager.h>
#include <PrayerTimes.h>

// --- Hardware Pins ---
#define OLED_SDA 14
#define OLED_SCL 15
#define BUZZER_PIN 13

// --- Display Configuration ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Audio Melodies ---
const char *warningMelody = "Warning:d=4,o=5,b=120:8c6,8e6,2g6";
const char *adhanMelody = "Adhan:d=4,o=5,b=90:2c,2c#,4e,4f,2g,2g,4g,4f,4e,4c#,2c";

// --- State Variables ---
float latitude = 0.0;
float longitude = 0.0;
float tzOffsetHours = 0.0;
long tzOffsetSecs = 0;
int lastDay = -1;

// Stores upcoming prayer times as minutes since midnight
int prayerTimesMins[5] = {0}; 
const char* prayerNames[5] = {"Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"};

bool warningPlayed[5] = {false};
bool adhanPlayed[5] = {false};

// --- Core Functions ---

// Get Location & Timezone from IP Address
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
            tzOffsetHours = tzOffsetSecs / 3600.0; // Math library uses fractional hours
            return true;
        }
    }
    return false;
}

// Perform entirely offline math to get today's exact adhan schedule
void calculateOfflinePrayerTimes(struct tm &timeinfo) {
    int year = timeinfo.tm_year + 1900;
    int month = timeinfo.tm_mon + 1;
    int day = timeinfo.tm_mday;

    // Configure the offline math logic
    set_calc_method(ISNA); // ISNA standard (15 degrees)
    
    // The library returns 7 fractional hour values (Fajr, Sunrise, Dhuhr, Asr, Sunset, Maghrib, Isha)
    double pTimes[7];
    get_prayer_times(year, month, day, latitude, longitude, tzOffsetHours, pTimes);
    
    // Extract the 5 adhans and convert fractional hours (e.g., 5.5) to minutes (330)
    prayerTimesMins[0] = (int)(pTimes[0] * 60); // Fajr
    prayerTimesMins[1] = (int)(pTimes[2] * 60); // Dhuhr
    prayerTimesMins[2] = (int)(pTimes[3] * 60); // Asr
    prayerTimesMins[3] = (int)(pTimes[5] * 60); // Maghrib
    prayerTimesMins[4] = (int)(pTimes[6] * 60); // Isha
}

// Draw the screen UI
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

void setup() {
    Serial.begin(115200);
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
    // Spawns access point if no saved networks are found
    if (!wm.autoConnect("ESP32-Adhan-Setup")) {
        ESP.restart(); // Retry if it stalls
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

    // Run first calculation
    calculateOfflinePrayerTimes(timeinfo);
    lastDay = timeinfo.tm_mday;

    // --- Step 4: SEVER INTERNET AND SHUT DOWN RADIO ---
    WiFi.disconnect(true, false); // Disconnect, but leave credentials saved in NVS for next boot
    WiFi.mode(WIFI_OFF);          // Completely powers down the Wi-Fi hardware and web server
}

void loop() {
    rtttl::play(); 
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return; 
    
    int currentMins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    
    // Midnight reset: Calculate completely offline!
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
    
    for (int i = 0; i < 5; i++) {
        if (!warningPlayed[i] && currentMins == (prayerTimesMins[i] - 10)) {
            rtttl::begin(BUZZER_PIN, warningMelody);
            warningPlayed[i] = true;
        }
        if (!adhanPlayed[i] && currentMins == prayerTimesMins[i]) {
            rtttl::begin(BUZZER_PIN, adhanMelody);
            adhanPlayed[i] = true;
        }
    }
    
    static unsigned long lastOledUpdate = 0;
    if (millis() - lastOledUpdate > 1000) {
        updateDisplay(timeinfo.tm_hour, timeinfo.tm_min, targetIndex, minsLeft);
        lastOledUpdate = millis();
    }
    
    delay(50); 
}