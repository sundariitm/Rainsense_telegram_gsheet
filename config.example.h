#pragma once

/*
 * Example config.h for GitHub
 * ---------------------------
 * Copy this file to config.h and replace all placeholder values.
 * Do NOT commit your real config.h with live credentials or tokens.
 */

// --------------------------------------------------
// WiFi credentials
// Keep multiple SSIDs for fallback connection attempts
// --------------------------------------------------
const char* WIFI_SSIDS[] = {
  "YourWiFi_1",
  "YourWiFi_2",
  "YourHotspot_2G"
};

const char* WIFI_PASSWORDS[] = {
  "YourPassword_1",
  "YourPassword_2",
  "YourHotspotPassword"
};

// --------------------------------------------------
// Telegram configuration
// BOT_TOKEN: create with @BotFather
// CHAT_ID: destination for alerts, can be a user, group, or channel id
// COMMAND_CHAT_ID: only this chat can use bot commands like /health
// --------------------------------------------------
const char* BOT_TOKEN       = "1234567890:REPLACE_WITH_BOTFATHER_TOKEN";
const char* CHAT_ID         = "-1001234567890";
const char* COMMAND_CHAT_ID = "123456789";

// --------------------------------------------------
// Google Apps Script web app endpoint
// Deploy your script as a Web App and paste the /exec URL here
// --------------------------------------------------
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/REPLACE_WITH_SCRIPT_ID/exec";

// --------------------------------------------------
// Device info
// --------------------------------------------------
const char* DEVICE_NAME = "nodemcu-rain-01";

// --------------------------------------------------
// NTP and timezone
// Example below is for India Standard Time (UTC+5:30)
// --------------------------------------------------
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
const long  GMT_OFFSET_SEC = 19800;
const int   DAYLIGHT_OFFSET_SEC = 0;

// --------------------------------------------------
// Rain classification thresholds
// These are based on the 30-second averaged raw A0 value.
// Adjust after calibration on your actual sensor.
//
// Example logic used by the sketch:
// raw > NO_RAIN_RAW_THRESHOLD      => No Rain
// raw >= LIGHT_RAIN_RAW_MIN        => Light Rain
// raw >= MODERATE_RAIN_RAW_MIN     => Moderate Rain
// raw <  MODERATE_RAIN_RAW_MIN     => Heavy Rain
// --------------------------------------------------
const int NO_RAIN_RAW_THRESHOLD = 1000;
const int LIGHT_RAIN_RAW_MIN    = 800;
const int MODERATE_RAIN_RAW_MIN = 600;

// --------------------------------------------------
// Dry reminder behavior
// DRY_UPDATE_INTERVAL_HOURS: send one dry reminder every N hours
// STOP_DRY_UPDATES_AFTER_HOURS: stop dry reminders after this many hours
// Set to 0 if you never want to stop dry reminders
// --------------------------------------------------
const uint8_t  DRY_UPDATE_INTERVAL_HOURS    = 1;
const uint16_t STOP_DRY_UPDATES_AFTER_HOURS = 24;

// --------------------------------------------------
// Optional permanent active reporting interval
// If not defined, the sketch defaults to 30 seconds.
// Uncomment to override, e.g. 60 seconds:
// #define ACTIVE_REPORT_INTERVAL_MS (60UL * 1000UL)
// --------------------------------------------------

