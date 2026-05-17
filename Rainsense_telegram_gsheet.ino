/****************************************************
 * ESP8266 Rain Monitor for NodeMCU
 *
 * Features
 * - A0 rain sensor, sampled every 10 seconds
 * - 30-second averaged raw A0 drives:
 *   1) rain category
 *   2) current wetness percentage
 * - Rolling 10-minute and 1-hour averages
 * - Telegram updates to CHAT_ID when category != No Rain
 * - Default active interval comes from ACTIVE_REPORT_INTERVAL_MS macro
 * - If ACTIVE_REPORT_INTERVAL_MS is not defined, default = 30 seconds
 * - FAST_RAIN_REPORT_TEST can force 15 seconds at compile time
 * - Bot command /interval N sets active interval to N seconds for 10 minutes
 * - Bot command /interval 0 resets back to default interval
 * - Dry reminder every configured hours, stops after configured total hours
 * - Health command via Telegram bot chat
 * - Reads last rain epoch from Google Sheet state endpoint
 * - Simulation mode available for testing
 ****************************************************/

// --------------------------------------------------
// Compile-time options
// --------------------------------------------------
#define DEBUG_ENABLED
// #define TEST_RAIN_SENSOR_SIMULATION
// #define FAST_RAIN_REPORT_TEST

#ifdef DEBUG_ENABLED
  #define DBG_BEGIN(x)         Serial.begin(x)
  #define DBG_PRINT(x)         Serial.print(x)
  #define DBG_PRINTLN(x)       Serial.println(x)
  #define DBG_PRINT_F(x, d)    Serial.print((x), (d))
  #define DBG_PRINTLN_F(x, d)  Serial.println((x), (d))
  #define DBG_PRINTF(...)      Serial.printf(__VA_ARGS__)
#else
  #define DBG_BEGIN(x)
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_PRINT_F(x, d)
  #define DBG_PRINTLN_F(x, d)
  #define DBG_PRINTF(...)
#endif

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <UniversalTelegramBot.h>
#include <time.h>

#include "config.h"

// --------------------------------------------------
// Active report interval selection
// 1) FAST_RAIN_REPORT_TEST forces 15 sec
// 2) Else if ACTIVE_REPORT_INTERVAL_MS is defined, use it
// 3) Else default to 30 sec
// --------------------------------------------------
#ifdef FAST_RAIN_REPORT_TEST
  #ifdef ACTIVE_REPORT_INTERVAL_MS
    #undef ACTIVE_REPORT_INTERVAL_MS
  #endif
  #define ACTIVE_REPORT_INTERVAL_MS (15UL * 1000UL)
#endif

#ifndef ACTIVE_REPORT_INTERVAL_MS
  #define ACTIVE_REPORT_INTERVAL_MS (30UL * 1000UL)
#endif

const unsigned long ACTIVE_REPORT_INTERVAL_DEFAULT_MS = ACTIVE_REPORT_INTERVAL_MS;
const unsigned long TEMP_INTERVAL_DURATION_MS         = 10UL * 60UL * 1000UL; // 10 min

// --------------------------------------------------
// Hardware
// --------------------------------------------------
static const uint8_t RAIN_PIN = A0;
const int ADC_MIN_RAW = 0;
const int ADC_MAX_RAW = 1023;

// --------------------------------------------------
// Timing
// --------------------------------------------------
const unsigned long SAMPLE_INTERVAL_MS       = 10UL * 1000UL;
const unsigned long BLOCK_INTERVAL_MS        = 30UL * 1000UL;
const unsigned long WIFI_RETRY_MS            = 10UL * 1000UL;
const unsigned long NTP_RESYNC_MS            = 6UL * 60UL * 60UL * 1000UL;
const unsigned long SHEET_STATE_REFRESH_MS   = 30UL * 60UL * 1000UL;
const unsigned long BOT_POLL_INTERVAL_MS     = 2000UL;

// --------------------------------------------------
// Rolling windows
// --------------------------------------------------
const uint8_t  BLOCKS_FOR_10MIN = 20;   // 20 x 30 sec = 10 min
const uint16_t BLOCKS_FOR_1HOUR = 120;  // 120 x 30 sec = 1 hour

struct RollingAverageWindow10 {
  float values[BLOCKS_FOR_10MIN];
  uint8_t count = 0;
  uint8_t head  = 0;
};

struct RollingAverageWindow60 {
  float values[BLOCKS_FOR_1HOUR];
  uint16_t count = 0;
  uint16_t head  = 0;
};

RollingAverageWindow10 g_window10;
RollingAverageWindow60 g_window60;

// --------------------------------------------------
// Telegram / WiFi
// --------------------------------------------------
const size_t WIFI_NETWORK_COUNT = sizeof(WIFI_SSIDS) / sizeof(WIFI_SSIDS[0]);

WiFiClientSecure telegramClient;
WiFiClientSecure sheetsClient;
UniversalTelegramBot bot(BOT_TOKEN, telegramClient);

// --------------------------------------------------
// Category enum
// --------------------------------------------------
enum RainCategory {
  CAT_NO_RAIN = 0,
  CAT_LIGHT,
  CAT_MODERATE,
  CAT_HEAVY
};

// --------------------------------------------------
// Runtime state
// --------------------------------------------------
unsigned long lastSampleMs        = 0;
unsigned long lastBlockMs         = 0;
unsigned long lastActiveReportMs  = 0;
unsigned long lastDryReminderMs   = 0;
unsigned long lastWifiRetryMs     = 0;
unsigned long lastNtpSyncMs       = 0;
unsigned long lastSheetStateMs    = 0;
unsigned long lastBotPollMs       = 0;

unsigned long g_tempActiveReportIntervalMs = 0;
unsigned long g_tempActiveReportStartMs    = 0;

int   latestRawSample            = 1023;
float latest30SecAverageRaw      = 1023.0f;
float latest30SecPercent         = 0.0f;

uint32_t rawAccumulator          = 0;
uint8_t  rawSampleCount          = 0;

RainCategory currentCategory     = CAT_NO_RAIN;
RainCategory previousCategory    = CAT_NO_RAIN;

bool dryUpdatesStopped           = false;
time_t g_lastRainEpoch           = 0;

#ifdef TEST_RAIN_SENSOR_SIMULATION
int simulatedRainRaw = 1023;
unsigned long lastSimulationModeChangeMs = 0;
uint8_t simulationMode = 0;  // 0=no-rain, 1=light, 2=moderate, 3=heavy
#endif

// --------------------------------------------------
// Utility
// --------------------------------------------------
String urlEncode(const String &input) {
  const char *hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(input.length() * 3);

  for (size_t i = 0; i < input.length(); i++) {
    unsigned char c = static_cast<unsigned char>(input.charAt(i));

    if (isalnum(static_cast<int>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

time_t getCurrentEpoch() {
  time_t now = time(nullptr);
  if (now < 1609459200L) {
    return 0;
  }
  return now;
}

float clampRaw(float rawValue) {
  if (rawValue < ADC_MIN_RAW) return ADC_MIN_RAW;
  if (rawValue > ADC_MAX_RAW) return ADC_MAX_RAW;
  return rawValue;
}

float rawToWetPercent(float rawValue) {
  rawValue = clampRaw(rawValue);
  return ((ADC_MAX_RAW - rawValue) * 100.0f) / ADC_MAX_RAW;
}

RainCategory classifyRainFromRaw(int rawValue) {
  if (rawValue > NO_RAIN_RAW_THRESHOLD) {
    return CAT_NO_RAIN;
  } else if (rawValue >= LIGHT_RAIN_RAW_MIN) {
    return CAT_LIGHT;
  } else if (rawValue >= MODERATE_RAIN_RAW_MIN) {
    return CAT_MODERATE;
  } else {
    return CAT_HEAVY;
  }
}

const char* categoryText(RainCategory cat) {
  switch (cat) {
    case CAT_LIGHT:    return "Light";
    case CAT_MODERATE: return "Moderate";
    case CAT_HEAVY:    return "Heavy";
    default:           return "No Rain";
  }
}

const char* categoryEmoji(RainCategory cat) {
  switch (cat) {
    case CAT_LIGHT:    return "🌦️";
    case CAT_MODERATE: return "🌧️";
    case CAT_HEAVY:    return "⛈️";
    default:           return "☀️";
  }
}

bool isRainCategory(RainCategory cat) {
  return (cat != CAT_NO_RAIN);
}

String formatEpochLocal(time_t epochValue) {
  if (epochValue <= 0) {
    return "unknown";
  }

  struct tm timeInfo;
  localtime_r(&epochValue, &timeInfo);

  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
  return String(buf);
}

// --------------------------------------------------
// WiFi diagnostics
// --------------------------------------------------
const char* wifiStatusToText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:           return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:         return "WL_NO_SSID_AVAIL";
    case WL_CONNECTED:             return "WL_CONNECTED";
    case WL_CONNECT_FAILED:        return "WL_CONNECT_FAILED";
#ifdef WL_CONNECT_WRONG_PASSWORD
    case WL_CONNECT_WRONG_PASSWORD:return "WL_CONNECT_WRONG_PASSWORD";
#endif
#ifdef WL_CONNECTION_LOST
    case WL_CONNECTION_LOST:       return "WL_CONNECTION_LOST";
#endif
    case WL_DISCONNECTED:          return "WL_DISCONNECTED";
    default:                       return "UNKNOWN_STATUS";
  }
}

void printWifiDiagnostics() {
#ifdef DEBUG_ENABLED
  DBG_PRINTLN("---- WiFi diagnostics ----");
  DBG_PRINT("Mode: ");
  DBG_PRINTLN((int)WiFi.getMode());

  DBG_PRINT("Status code: ");
  DBG_PRINT((int)WiFi.status());
  DBG_PRINT(" -> ");
  DBG_PRINTLN(wifiStatusToText((wl_status_t)WiFi.status()));

  DBG_PRINT("Hostname: ");
  DBG_PRINTLN(WiFi.hostname());

  DBG_PRINT("Auto reconnect: ");
  DBG_PRINTLN(WiFi.getAutoReconnect() ? "ON" : "OFF");

  DBG_PRINT("Local IP: ");
  DBG_PRINTLN(WiFi.localIP());

  DBG_PRINT("Gateway: ");
  DBG_PRINTLN(WiFi.gatewayIP());

  DBG_PRINT("Subnet: ");
  DBG_PRINTLN(WiFi.subnetMask());

  DBG_PRINT("DNS: ");
  DBG_PRINTLN(WiFi.dnsIP());

  DBG_PRINT("MAC: ");
  DBG_PRINTLN(WiFi.macAddress());

  DBG_PRINT("Current SSID: ");
  DBG_PRINTLN(WiFi.SSID());

  DBG_PRINT("RSSI: ");
  DBG_PRINT(WiFi.RSSI());
  DBG_PRINTLN(" dBm");

  WiFi.printDiag(Serial);
  DBG_PRINTLN("--------------------------");
#endif
}

void scanAndPrintNearbyWiFi(const char* preferredSsid = nullptr) {
#ifdef DEBUG_ENABLED
  DBG_PRINTLN("Scanning nearby WiFi...");
  int n = WiFi.scanNetworks();

  if (n < 0) {
    DBG_PRINTLN("WiFi scan failed.");
    return;
  }

  DBG_PRINT("Networks found: ");
  DBG_PRINTLN(n);

  bool targetFound = false;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    long rssi = WiFi.RSSI(i);

    DBG_PRINT("#");
    DBG_PRINT(i + 1);
    DBG_PRINT(" SSID=");
    DBG_PRINT(ssid);
    DBG_PRINT(" RSSI=");
    DBG_PRINT(rssi);
    DBG_PRINT(" dBm ENC=");
#if defined(ENC_TYPE_NONE)
    DBG_PRINTLN((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? "OPEN" : "SECURED");
#else
    DBG_PRINTLN((WiFi.encryptionType(i) == 7) ? "OPEN" : "SECURED");
#endif

    if (preferredSsid && ssid == String(preferredSsid)) {
      targetFound = true;
    }
  }

  if (preferredSsid) {
    DBG_PRINT("Target SSID ");
    DBG_PRINT(preferredSsid);
    DBG_PRINT(": ");
    DBG_PRINTLN(targetFound ? "FOUND" : "NOT FOUND");
  }

  WiFi.scanDelete();
#else
  (void)preferredSsid;
#endif
}

// --------------------------------------------------
// Active interval helpers
// --------------------------------------------------
bool parseUnsignedLongAfterSpace(const String &text, unsigned long &outValue) {
  int idx = text.indexOf(' ');
  if (idx < 0) return false;

  String numStr = text.substring(idx + 1);
  numStr.trim();
  if (numStr.length() == 0) return false;

  for (size_t i = 0; i < numStr.length(); i++) {
    if (!isDigit(numStr.charAt(i))) {
      return false;
    }
  }

  outValue = (unsigned long)numStr.toInt();
  return true;
}

bool isTempIntervalActive() {
  if (g_tempActiveReportIntervalMs == 0) {
    return false;
  }

  if (millis() - g_tempActiveReportStartMs >= TEMP_INTERVAL_DURATION_MS) {
    g_tempActiveReportIntervalMs = 0;
    g_tempActiveReportStartMs = 0;
    return false;
  }

  return true;
}

unsigned long getEffectiveActiveReportIntervalMs() {
  if (isTempIntervalActive()) {
    return g_tempActiveReportIntervalMs;
  }
  return ACTIVE_REPORT_INTERVAL_DEFAULT_MS;
}

String getIntervalStatusText() {
  String s;
  s.reserve(90);

  if (isTempIntervalActive()) {
    unsigned long elapsed = millis() - g_tempActiveReportStartMs;
    unsigned long remainingSec = (TEMP_INTERVAL_DURATION_MS - elapsed) / 1000UL;

    s += "Temp interval ";
    s += String(g_tempActiveReportIntervalMs / 1000UL);
    s += "s, remaining ";
    s += String(remainingSec);
    s += "s";
  } else {
    s += "Default interval ";
    s += String(ACTIVE_REPORT_INTERVAL_DEFAULT_MS / 1000UL);
    s += "s";
  }

  return s;
}

// --------------------------------------------------
// Rolling averages
// --------------------------------------------------
void push30SecondAverageRaw(float rawValue) {
  g_window10.values[g_window10.head] = rawValue;
  g_window10.head = (g_window10.head + 1) % BLOCKS_FOR_10MIN;
  if (g_window10.count < BLOCKS_FOR_10MIN) {
    g_window10.count++;
  }

  g_window60.values[g_window60.head] = rawValue;
  g_window60.head = (g_window60.head + 1) % BLOCKS_FOR_1HOUR;
  if (g_window60.count < BLOCKS_FOR_1HOUR) {
    g_window60.count++;
  }
}

float get10MinuteAverageRaw() {
  if (g_window10.count == 0) return latest30SecAverageRaw;

  float sum = 0.0f;
  for (uint8_t i = 0; i < g_window10.count; i++) {
    sum += g_window10.values[i];
  }
  return sum / (float)g_window10.count;
}

float get1HourAverageRaw() {
  if (g_window60.count == 0) return latest30SecAverageRaw;

  float sum = 0.0f;
  for (uint16_t i = 0; i < g_window60.count; i++) {
    sum += g_window60.values[i];
  }
  return sum / (float)g_window60.count;
}

float get10MinuteAveragePercent() {
  return rawToWetPercent(get10MinuteAverageRaw());
}

float get1HourAveragePercent() {
  return rawToWetPercent(get1HourAverageRaw());
}

// --------------------------------------------------
// WiFi
// --------------------------------------------------
bool connectWiFi(uint32_t timeoutMs = 20000) {
  DBG_PRINTLN("\nConnecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  for (size_t i = 0; i < WIFI_NETWORK_COUNT; i++) {
    const char* ssid = WIFI_SSIDS[i];
    const char* pass = WIFI_PASSWORDS[i];

    DBG_PRINTLN("================================");
    DBG_PRINT("Trying SSID [");
    DBG_PRINT(i + 1);
    DBG_PRINT("/");
    DBG_PRINT(WIFI_NETWORK_COUNT);
    DBG_PRINT("]: ");
    DBG_PRINTLN(ssid);

    scanAndPrintNearbyWiFi(ssid);

    WiFi.disconnect();
    delay(300);

    DBG_PRINT("Calling WiFi.begin(\"");
    DBG_PRINT(ssid);
    DBG_PRINTLN("\", ****)");

    WiFi.begin(ssid, pass);

    unsigned long startMs = millis();
    unsigned long lastStatusPrintMs = 0;
    wl_status_t lastStatus = WL_IDLE_STATUS;

    while ((millis() - startMs) < timeoutMs) {
      wl_status_t st = (wl_status_t)WiFi.status();

      if (st == WL_CONNECTED) {
        DBG_PRINTLN("");
        DBG_PRINT("WiFi connected to: ");
        DBG_PRINTLN(ssid);
        DBG_PRINT("IP address: ");
        DBG_PRINTLN(WiFi.localIP());
        DBG_PRINT("RSSI: ");
        DBG_PRINT(WiFi.RSSI());
        DBG_PRINTLN(" dBm");

        telegramClient.setInsecure();
        sheetsClient.setInsecure();
        return true;
      }

      if (st != lastStatus || millis() - lastStatusPrintMs >= 2000) {
        DBG_PRINT("Status=");
        DBG_PRINT((int)st);
        DBG_PRINT(" (");
        DBG_PRINT(wifiStatusToText(st));
        DBG_PRINT("), elapsed=");
        DBG_PRINT((millis() - startMs) / 1000);
        DBG_PRINTLN("s");

        lastStatus = st;
        lastStatusPrintMs = millis();
      }

      delay(250);
      yield();
    }

    wl_status_t finalStatus = (wl_status_t)WiFi.status();

    DBG_PRINT("Failed to connect to SSID: ");
    DBG_PRINTLN(ssid);
    DBG_PRINT("Final status=");
    DBG_PRINT((int)finalStatus);
    DBG_PRINT(" (");
    DBG_PRINT(wifiStatusToText(finalStatus));
    DBG_PRINTLN(")");

    if (finalStatus == WL_NO_SSID_AVAIL) {
      DBG_PRINTLN("Hint: SSID not visible. Check WiFi name, 2.4 GHz availability, or router range.");
    } else if (finalStatus == WL_CONNECT_FAILED) {
      DBG_PRINTLN("Hint: Authentication failed. Check password or router security mode.");
    } else if (finalStatus == WL_DISCONNECTED) {
      DBG_PRINTLN("Hint: Station disconnected. Router may be rejecting the device or signal may be weak.");
    }

    printWifiDiagnostics();
    DBG_PRINTLN("================================");
  }

  DBG_PRINTLN("Failed to connect to all configured WiFi networks.");
  return false;
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long nowMs = millis();
  if (nowMs - lastWifiRetryMs < WIFI_RETRY_MS) return;

  lastWifiRetryMs = nowMs;
  DBG_PRINTLN("WiFi lost. Retrying...");
  if (connectWiFi()) {
    DBG_PRINTLN("WiFi restored.");
  }
}

// --------------------------------------------------
// NTP
// --------------------------------------------------
void initTime() {
  DBG_PRINTLN("Configuring NTP...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);

  time_t now = time(nullptr);
  int retries = 0;

  DBG_PRINT("Waiting for NTP sync");
  while (now < 1609459200L && retries < 90) {
    delay(500);
    DBG_PRINT(".");
    now = time(nullptr);
    retries++;
  }
  DBG_PRINTLN("");

  if (now < 1609459200L) {
    DBG_PRINTLN("NTP sync FAILED");
  } else {
    struct tm timeInfo;
    localtime_r(&now, &timeInfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
    DBG_PRINT("NTP sync OK: ");
    DBG_PRINTLN(buf);
  }

  lastNtpSyncMs = millis();
}

bool getLocalTimeStruct(struct tm *timeInfo) {
  time_t now = time(nullptr);
  if (now < 1609459200L) return false;
  localtime_r(&now, timeInfo);
  return true;
}

String getTimestampString() {
  struct tm timeInfo;
  if (!getLocalTimeStruct(&timeInfo)) {
    return "time-not-synced";
  }

  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
  return String(buf);
}

void ensureTimeSync() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long nowMs = millis();
  if (nowMs - lastNtpSyncMs >= NTP_RESYNC_MS) {
    initTime();
  }
}

// --------------------------------------------------
// Sensor read
// --------------------------------------------------
#ifdef TEST_RAIN_SENSOR_SIMULATION
void updateSimulationMode() {
  unsigned long nowMs = millis();

  if (lastSimulationModeChangeMs == 0 || (nowMs - lastSimulationModeChangeMs >= 120000UL)) {
    simulationMode = (simulationMode + 1) % 4;
    lastSimulationModeChangeMs = nowMs;

    DBG_PRINT("Simulation mode changed to: ");
    switch (simulationMode) {
      case 0: DBG_PRINTLN("NO RAIN"); break;
      case 1: DBG_PRINTLN("LIGHT"); break;
      case 2: DBG_PRINTLN("MODERATE"); break;
      case 3: DBG_PRINTLN("HEAVY"); break;
    }
  }
}

int readRainRawValue() {
  updateSimulationMode();

  switch (simulationMode) {
    case 0: simulatedRainRaw = random(1001, 1024); break;
    case 1: simulatedRainRaw = random(800, 1001);  break;
    case 2: simulatedRainRaw = random(600, 800);   break;
    case 3: simulatedRainRaw = random(0, 600);     break;
    default: simulatedRainRaw = random(0, 1024);   break;
  }

  return constrain(simulatedRainRaw, 0, 1023);
}
#else
int readRainRawValue() {
  return analogRead(RAIN_PIN);
}
#endif

void takeRainSample() {
  latestRawSample = readRainRawValue();
  rawAccumulator += latestRawSample;
  rawSampleCount++;

  DBG_PRINT("Sample raw=");
  DBG_PRINT(latestRawSample);
  DBG_PRINT(" count=");
  DBG_PRINTLN(rawSampleCount);
}

// --------------------------------------------------
// Last-rain helpers
// --------------------------------------------------
void getSinceLastRain(time_t lastRainEpoch, long &totalHours, long &days, long &weeks, long &months) {
  totalHours = 0;
  days = 0;
  weeks = 0;
  months = 0;

  time_t now = getCurrentEpoch();
  if (lastRainEpoch <= 0 || now <= 0 || now < lastRainEpoch) {
    return;
  }

  long diffSec = (long)(now - lastRainEpoch);
  totalHours = diffSec / 3600L;
  days = diffSec / 86400L;
  weeks = days / 7L;

  struct tm nowTm;
  struct tm lastTm;
  localtime_r(&now, &nowTm);
  localtime_r(&lastRainEpoch, &lastTm);

  months = (nowTm.tm_year - lastTm.tm_year) * 12L + (nowTm.tm_mon - lastTm.tm_mon);
  if (nowTm.tm_mday < lastTm.tm_mday) {
    months--;
  }
  if (months < 0) {
    months = 0;
  }
}

String formatLastRainSinceShort() {
  long totalHours, days, weeks, months;
  getSinceLastRain(g_lastRainEpoch, totalHours, days, weeks, months);

  if (g_lastRainEpoch <= 0) {
    return "unknown";
  }

  String s;
  s.reserve(32);
  s += String(days);
  s += "d | ";
  s += String(weeks);
  s += "w | ";
  s += String(months);
  s += "mo";
  return s;
}

// --------------------------------------------------
// Google state fetch
// Expected response:
// status=OK
// device=...
// lastRainEpoch=...
// --------------------------------------------------
long parseLongFromKeyValue(const String &payload, const String &key) {
  String needle = key + "=";
  int start = payload.indexOf(needle);
  if (start < 0) return 0;

  start += needle.length();
  int end = payload.indexOf('\n', start);
  if (end < 0) end = payload.length();

  String value = payload.substring(start, end);
  value.trim();
  return value.toInt();
}

bool fetchLastRainEpochFromGoogleSheet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(GOOGLE_SCRIPT_URL);
  url += "?action=get_state";
  url += "&device=" + urlEncode(String(DEVICE_NAME));

  HTTPClient https;
  bool ok = false;

  sheetsClient.stop();
  sheetsClient.setInsecure();
  delay(100);

  if (https.begin(sheetsClient, url)) {
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = https.GET();

    DBG_PRINT("State HTTP code: ");
    DBG_PRINTLN(httpCode);

    if (httpCode > 0) {
      String payload = https.getString();

      DBG_PRINT("State payload len: ");
      DBG_PRINTLN(payload.length());
      DBG_PRINT("State payload: ");
      DBG_PRINTLN(payload);

      long epochVal = parseLongFromKeyValue(payload, "lastRainEpoch");
      if (epochVal > 0) {
        g_lastRainEpoch = (time_t)epochVal;
      }
      ok = true;
    }
    https.end();
  }

  sheetsClient.stop();
  lastSheetStateMs = millis();
  return ok;
}

// --------------------------------------------------
// Telegram helpers
// --------------------------------------------------
bool isAuthorizedChat(const String &chat_id) {
  return (chat_id == String(COMMAND_CHAT_ID));
}

bool checkTelegramServerReachability() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure testClient;
  testClient.setInsecure();
  testClient.setTimeout(5000);

  bool ok = testClient.connect("api.telegram.org", 443);
  testClient.stop();
  return ok;
}

bool sendTelegramToChat(const String &destChatId, const String &message) {
  if (WiFi.status() != WL_CONNECTED) {
    DBG_PRINTLN("Telegram send skipped: WiFi not connected");
    return false;
  }

  telegramClient.stop();
  telegramClient.setInsecure();
  yield();
  delay(100);

  bool ok = bot.sendMessage(destChatId, message, "");

  DBG_PRINT("Telegram send result: ");
  DBG_PRINTLN(ok ? "OK" : "FAILED");

  telegramClient.stop();
  return ok;
}

String buildRainUpdateMessage() {
  String msg;
  msg.reserve(300);

  msg += String(categoryEmoji(currentCategory));
  msg += " ";
  msg += categoryText(currentCategory);
  msg += " rain\n";

  msg += "🕒 ";
  msg += getTimestampString();

  msg += "\n📟 A0 ";
  msg += String((int)roundf(latest30SecAverageRaw));

  msg += "\n💧 Now ";
  msg += String(latest30SecPercent, 1);
  msg += "%";

  msg += "\n⏱️ 10m ";
  msg += String(get10MinuteAveragePercent(), 1);
  msg += "% | 1h ";
  msg += String(get1HourAveragePercent(), 1);
  msg += "%";

  msg += "\n⏲️ ";
  msg += String(getEffectiveActiveReportIntervalMs() / 1000UL);
  msg += "s";

#ifdef TEST_RAIN_SENSOR_SIMULATION
  msg += "\n🧪 SIM";
#endif

  return msg;
}

String buildDryReminderMessage() {
  String msg;
  msg.reserve(180);

  msg += "☀️ No rain\n";
  msg += "🕒 ";
  msg += getTimestampString();
  msg += "\n⏳ Last rain ";
  msg += formatLastRainSinceShort();

#ifdef TEST_RAIN_SENSOR_SIMULATION
  msg += "\n🧪 SIM";
#endif

  return msg;
}

String buildHealthMessage() {
  String msg;
  msg.reserve(500);

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool telegramReachOk = checkTelegramServerReachability();

  msg += "🩺 Node health\n";
  msg += "🕒 ";
  msg += getTimestampString();

  msg += "\n📶 WiFi: ";
  msg += (wifiOk ? "OK" : "DOWN");

  if (wifiOk) {
    msg += "\n📡 ";
    msg += WiFi.SSID();
    msg += " | RSSI ";
    msg += String(WiFi.RSSI());
    msg += " dBm";

    msg += "\n🌐 ";
    msg += WiFi.localIP().toString();
  }

  msg += "\n🤖 Telegram API: ";
  msg += (telegramReachOk ? "Reachable" : "Unreachable");

  msg += "\n🌧️ Last rain: ";
  msg += formatEpochLocal(g_lastRainEpoch);

  msg += "\n⏳ Since: ";
  msg += formatLastRainSinceShort();

  msg += "\n📟 30s A0: ";
  msg += String((int)roundf(latest30SecAverageRaw));

  msg += "\n💧 Wetness: ";
  msg += String(latest30SecPercent, 1);
  msg += "% | ";
  msg += categoryText(currentCategory);

  msg += "\n⏲️ ";
  msg += getIntervalStatusText();

#ifdef TEST_RAIN_SENSOR_SIMULATION
  msg += "\n🧪 SIM";
#endif

  return msg;
}

void handleTelegramCommands(int numNewMessages) {
  DBG_PRINT("New Telegram messages: ");
  DBG_PRINTLN(numNewMessages);

  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from = bot.messages[i].from_name;

    DBG_PRINT("Message from ");
    DBG_PRINT(from);
    DBG_PRINT(": ");
    DBG_PRINTLN(text);

    if (!isAuthorizedChat(chat_id)) {
      sendTelegramToChat(chat_id, "Unauthorized user");
      continue;
    }

    text.trim();

    if (text == "/start") {
      String welcome;
      welcome.reserve(350);
      welcome = "Rain monitor bot\n";
      welcome += "Commands:\n";
      welcome += "/health - node health\n";
      welcome += "/status - same as /health\n";
      welcome += "/last_rain - last rain info\n";
      welcome += "/interval N - set active rain interval in seconds for next 10 min\n";
      welcome += "/interval 0 - reset interval to default\n";
      welcome += "/help - command list";
      sendTelegramToChat(chat_id, welcome);
    }
    else if (text == "/help") {
      String msg;
      msg.reserve(250);
      msg = "Commands:\n";
      msg += "/health\n";
      msg += "/status\n";
      msg += "/last_rain\n";
      msg += "/interval N\n";
      msg += "/interval 0\n";
      msg += getIntervalStatusText();
      sendTelegramToChat(chat_id, msg);
    }
    else if (text == "/health" || text == "/status") {
      sendTelegramToChat(chat_id, buildHealthMessage());
    }
    else if (text == "/last_rain") {
      String msg;
      msg.reserve(180);
      msg = "🌧️ Last rain: ";
      msg += formatEpochLocal(g_lastRainEpoch);
      msg += "\n⏳ Since: ";
      msg += formatLastRainSinceShort();
      sendTelegramToChat(chat_id, msg);
    }
    else if (text.startsWith("/interval")) {
      unsigned long sec = 0;

      if (!parseUnsignedLongAfterSpace(text, sec)) {
        sendTelegramToChat(chat_id,
          "Usage:\n"
          "/interval 15  -> use 15 sec for next 10 minutes\n"
          "/interval 0   -> reset to default");
      }
      else if (sec == 0) {
        g_tempActiveReportIntervalMs = 0;
        g_tempActiveReportStartMs = 0;

        String msg = "Interval reset. ";
        msg += getIntervalStatusText();
        sendTelegramToChat(chat_id, msg);
      }
      else if (sec < 5 || sec > 3600) {
        sendTelegramToChat(chat_id,
          "Invalid interval. Use 5 to 3600 seconds, or 0 to reset.");
      }
      else {
        g_tempActiveReportIntervalMs = sec * 1000UL;
        g_tempActiveReportStartMs = millis();
        lastActiveReportMs = 0; // immediate effect

        String msg = "Temporary interval applied for 10 minutes. ";
        msg += getIntervalStatusText();
        sendTelegramToChat(chat_id, msg);
      }
    }
    else {
      sendTelegramToChat(chat_id, "Unknown command. Use /help");
    }
  }
}

void pollTelegramCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long nowMs = millis();
  if (nowMs - lastBotPollMs < BOT_POLL_INTERVAL_MS) return;

  telegramClient.stop();
  telegramClient.setInsecure();

  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  while (numNewMessages) {
    handleTelegramCommands(numNewMessages);
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }

  telegramClient.stop();
  lastBotPollMs = nowMs;
}

// --------------------------------------------------
// Google Sheets logging
// --------------------------------------------------
bool uploadToGoogleSheet() {
  if (WiFi.status() != WL_CONNECTED) {
    DBG_PRINTLN("Sheets upload skipped: WiFi not connected");
    return false;
  }

  float avg10Raw = get10MinuteAverageRaw();
  float avg60Raw = get1HourAverageRaw();

  float avg10Pct = rawToWetPercent(avg10Raw);
  float avg60Pct = rawToWetPercent(avg60Raw);

  RainCategory avg10Cat = classifyRainFromRaw((int)roundf(avg10Raw));
  RainCategory avg60Cat = classifyRainFromRaw((int)roundf(avg60Raw));

  time_t nowEpoch = getCurrentEpoch();
  if (nowEpoch > 0 && isRainCategory(currentCategory)) {
    g_lastRainEpoch = nowEpoch;
  }

  String url = String(GOOGLE_SCRIPT_URL);
  url += "?action=log";
  url += "&device=" + urlEncode(String(DEVICE_NAME));
  url += "&timestamp=" + urlEncode(getTimestampString());
  url += "&epoch=" + String((unsigned long)g_lastRainEpoch);
  url += "&state=" + String(isRainCategory(currentCategory) ? "RAIN" : "DRY");
  url += "&raw=" + String((int)roundf(latest30SecAverageRaw));
  url += "&current=" + String(latest30SecPercent, 1);
  url += "&currentCat=" + urlEncode(String(categoryText(currentCategory)));
  url += "&avg10=" + String(avg10Pct, 1);
  url += "&avg10Cat=" + urlEncode(String(categoryText(avg10Cat)));
  url += "&avg60=" + String(avg60Pct, 1);
  url += "&avg60Cat=" + urlEncode(String(categoryText(avg60Cat)));
#ifdef TEST_RAIN_SENSOR_SIMULATION
  url += "&testMode=SIMULATED";
#else
  url += "&testMode=LIVE";
#endif

  HTTPClient https;
  bool ok = false;

  sheetsClient.stop();
  sheetsClient.setInsecure();
  delay(100);

  if (https.begin(sheetsClient, url)) {
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = https.GET();

    DBG_PRINT("Sheets HTTP code: ");
    DBG_PRINTLN(httpCode);

    if (httpCode > 0) {
      String payload = https.getString();
      DBG_PRINT("Sheets response: ");
      DBG_PRINTLN(payload);
    }

    ok = (httpCode >= 200 && httpCode < 300);
    https.end();
  } else {
    DBG_PRINTLN("HTTPS begin failed for Google Sheets");
  }

  sheetsClient.stop();
  return ok;
}

// --------------------------------------------------
// 30-second block finalization
// --------------------------------------------------
void finalize30SecondBlock() {
  if (rawSampleCount == 0) {
    latest30SecAverageRaw = latestRawSample;
  } else {
    latest30SecAverageRaw = (float)rawAccumulator / (float)rawSampleCount;
  }

  latest30SecPercent = rawToWetPercent(latest30SecAverageRaw);
  push30SecondAverageRaw(latest30SecAverageRaw);

  previousCategory = currentCategory;
  currentCategory = classifyRainFromRaw((int)roundf(latest30SecAverageRaw));

  DBG_PRINT("30s avg raw=");
  DBG_PRINT((int)roundf(latest30SecAverageRaw));
  DBG_PRINT(" cat=");
  DBG_PRINT(categoryText(currentCategory));
  DBG_PRINT(" pct=");
  DBG_PRINT_F(latest30SecPercent, 1);
  DBG_PRINT("% 10m=");
  DBG_PRINT_F(get10MinuteAveragePercent(), 1);
  DBG_PRINT("% 1h=");
  DBG_PRINT_F(get1HourAveragePercent(), 1);
  DBG_PRINTLN("%");

  rawAccumulator = 0;
  rawSampleCount = 0;

  if (!isRainCategory(previousCategory) && isRainCategory(currentCategory)) {
    DBG_PRINTLN("Rain started.");
    time_t nowEpoch = getCurrentEpoch();
    if (nowEpoch > 0) {
      g_lastRainEpoch = nowEpoch;
    }
    lastActiveReportMs = 0;
    dryUpdatesStopped = false;
    lastDryReminderMs = millis();
  }
  else if (isRainCategory(previousCategory) && !isRainCategory(currentCategory)) {
    DBG_PRINTLN("Rain stopped.");
    lastDryReminderMs = millis();
  }
}

// --------------------------------------------------
// Reporting
// --------------------------------------------------
void handleReporting() {
  unsigned long nowMs = millis();

  if (isRainCategory(currentCategory)) {
    unsigned long effectiveIntervalMs = getEffectiveActiveReportIntervalMs();

    if (lastActiveReportMs == 0 || (nowMs - lastActiveReportMs >= effectiveIntervalMs)) {
      DBG_PRINT("Using report interval: ");
      DBG_PRINT(effectiveIntervalMs / 1000UL);
      DBG_PRINTLN(" sec");

      DBG_PRINTLN("Sending active rain update...");

      bool telegramOk = sendTelegramToChat(String(CHAT_ID), buildRainUpdateMessage());
      delay(500);

      if (telegramOk) {
        uploadToGoogleSheet();
      }

      lastActiveReportMs = nowMs;
    }
    return;
  }

  if (g_lastRainEpoch <= 0 && WiFi.status() == WL_CONNECTED) {
    if (lastSheetStateMs == 0 || (nowMs - lastSheetStateMs >= SHEET_STATE_REFRESH_MS)) {
      fetchLastRainEpochFromGoogleSheet();
    }
  }

  long totalHours, days, weeks, months;
  getSinceLastRain(g_lastRainEpoch, totalHours, days, weeks, months);

  if (STOP_DRY_UPDATES_AFTER_HOURS > 0 && totalHours >= STOP_DRY_UPDATES_AFTER_HOURS) {
    if (!dryUpdatesStopped) {
      DBG_PRINTLN("Dry updates stopped due to configured limit.");
      dryUpdatesStopped = true;
    }
    return;
  }

  unsigned long dryIntervalMs = (unsigned long)DRY_UPDATE_INTERVAL_HOURS * 60UL * 60UL * 1000UL;
  if (!dryUpdatesStopped && (nowMs - lastDryReminderMs >= dryIntervalMs)) {
    DBG_PRINTLN("Sending dry reminder...");
    sendTelegramToChat(String(CHAT_ID), buildDryReminderMessage());
    lastDryReminderMs = nowMs;
  }
}

// --------------------------------------------------
// Setup / Loop
// --------------------------------------------------
void setup() {
  DBG_BEGIN(115200);
  delay(200);
  DBG_PRINTLN("\nBooting rain monitor...");

#ifdef TEST_RAIN_SENSOR_SIMULATION
  randomSeed(micros());
  DBG_PRINTLN("TEST_RAIN_SENSOR_SIMULATION is ENABLED");
#else
  DBG_PRINTLN("Live A0 rain sensor mode is ENABLED");
#endif

#ifdef FAST_RAIN_REPORT_TEST
  DBG_PRINTLN("FAST_RAIN_REPORT_TEST is ENABLED");
#endif

  DBG_PRINT("Default active interval: ");
  DBG_PRINT(ACTIVE_REPORT_INTERVAL_DEFAULT_MS / 1000UL);
  DBG_PRINTLN(" sec");

  WiFi.setAutoReconnect(true);

  while (!connectWiFi()) {
    delay(2000);
  }

  initTime();
  fetchLastRainEpochFromGoogleSheet();

  unsigned long nowMs = millis();
  lastSampleMs = nowMs;
  lastBlockMs = nowMs;
  lastDryReminderMs = nowMs;
  lastBotPollMs = nowMs;

  DBG_PRINTLN("Setup complete.");
}

void loop() {
  ensureWiFi();
  ensureTimeSync();

  unsigned long nowMs = millis();

  if (nowMs - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = nowMs;
    takeRainSample();
  }

  if (nowMs - lastBlockMs >= BLOCK_INTERVAL_MS) {
    lastBlockMs = nowMs;
    finalize30SecondBlock();
  }

  handleReporting();
  pollTelegramCommands();

  delay(20);
}
