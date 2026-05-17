# ESP8266 Rain Monitor with Telegram and Google Sheets

A NodeMCU ESP8266 project for remotely monitoring rain intensity, sending Telegram alerts, and logging data to Google Sheets.

This firmware is designed for rain-sensor deployments where the device may be installed away from the developer, so it emphasizes:

- Multi-SSID Wi-Fi fallback
- NTP-based timestamps
- Telegram status and control commands
- Google Sheets cloud logging
- Non-blocking `millis()` timing
- Extensive serial debugging for field diagnostics

## What this project does

The NodeMCU reads a rain sensor on analog pin `A0` every 10 seconds, builds a 30-second average, and maintains rolling averages for the last 10 minutes and 1 hour.

When rain is detected, the device:

- Sends periodic Telegram alerts
- Uploads readings to Google Sheets
- Includes timestamp, current rain level, and rolling averages in updates

When rain stops, the device can switch to less frequent dry reminders so the remote device still confirms it is alive without flooding messages.

## Main features

- NodeMCU ESP8266 compatible
- Rain sensor on analog input `A0`
- Multi-network Wi-Fi retry from `config.h`
- NTP time synchronization
- Telegram alerting and command support
- Google Sheets logging through Google Apps Script
- Rolling 10-minute and 1-hour calculations
- Threshold-based rain classification
- Configurable active and dry reporting intervals
- Simulation mode for bench testing before remote installation
- Verbose Wi-Fi diagnostics when connection fails

## Project structure

```text
rain-monitor-esp8266/
├── Rainsense_telegram_gsheet.ino   # Main firmware sketch
├── config.h                        # Local secrets and settings (not committed)
├── config.example.h                # GitHub-safe example configuration
├── README.md                       # Project documentation
└── .gitignore                      # Should exclude config.h
```

Recommended `.gitignore` entry:

```gitignore
config.h
```

## Hardware required

- NodeMCU ESP8266 development board
- Analog rain sensor module with analog output
- Stable 5V USB power source or suitable regulated supply
- Wi-Fi network or mobile hotspot
- USB cable for programming and serial monitoring

## Wiring

### Basic wiring

- Rain sensor analog output -> `A0`
- Rain sensor VCC -> module-rated supply
- Rain sensor GND -> `GND`

> Check the rain sensor board voltage requirements before wiring. Some sensor modules use a comparator board and provide both analog and digital outputs; this project uses the analog output only.

## Software stack

The sketch is built around commonly used ESP8266 Arduino components:

- `ESP8266WiFi`
- `WiFiClientSecure`
- `ESP8266HTTPClient`
- `UniversalTelegramBot`
- `time.h`

## How the logic works

### Sampling and averaging

1. The sensor is sampled every 10 seconds.
2. Three samples are averaged into one 30-second block value.
3. The sketch stores block values to compute:
   - 10-minute average
   - 1-hour average
4. The latest 30-second averaged raw value is used for rain-state classification.

### Rain state logic

The sketch classifies the averaged raw analog value into one of these states:

- No Rain
- Light Rain
- Moderate Rain
- Heavy Rain

Thresholds are set in `config.h` and should be tuned after calibration.

### Reporting behavior

#### While raining

The sketch sends active updates to Telegram and uploads to Google Sheets at the configured interval.

#### While dry

The sketch sends less frequent reminders based on the dry-reminder settings. This is useful after remote installation because it confirms the device is still powered, connected, and running.

## Telegram integration

The project uses a Telegram bot for:

- Alert messages to a chat, group, or channel
- Manual health checks
- Command-based diagnostics
- Temporary interval override for active rain updates

Typical commands supported by the firmware:

- `/start`
- `/help`
- `/health`
- `/status`
- `/last_rain`
- `/interval N`
- `/interval 0`

### Telegram setup

1. Open Telegram and start a chat with `@BotFather`.
2. Create a new bot and copy the bot token.
3. Decide where alerts should be sent:
   - personal chat
   - group
   - channel
4. Put that destination into `CHAT_ID`.
5. Put your authorized command source into `COMMAND_CHAT_ID`.
6. If using a channel, add the bot as an admin.

## Google Sheets integration

The sketch logs rain data through a deployed Google Apps Script Web App.

Typical flow:

1. Create a Google Sheet.
2. Open **Extensions -> Apps Script**.
3. Add a `doGet(e)` handler that accepts device parameters.
4. Deploy the script as a Web App.
5. Copy the `/exec` URL into `GOOGLE_SCRIPT_URL`.

Suggested logged fields:

- device name
- timestamp
- epoch
- rain state
- current averaged raw A0
- current wetness percentage
- 10-minute average
- 1-hour average
- current category
- test/live mode

## First-time setup

### 1. Install Arduino IDE support

1. Install Arduino IDE.
2. Install the **ESP8266 by ESP8266 Community** board package.
3. Select the board: **NodeMCU 1.0 (ESP-12E Module)**.

### 2. Install required libraries

Install from Arduino Library Manager if needed:

- `UniversalTelegramBot`
- `ArduinoJson` if required as a dependency

### 3. Create local configuration

1. Copy `config.example.h` to `config.h`.
2. Replace all placeholder values.
3. Do **not** commit your real `config.h` to GitHub.

### 4. Configure Wi-Fi

Add one or more SSIDs and passwords in `config.h`.

The firmware tries each configured SSID until one connects.

### 5. Configure Telegram

Fill in:

- `BOT_TOKEN`
- `CHAT_ID`
- `COMMAND_CHAT_ID`

### 6. Configure Google Sheets

Fill in:

- `GOOGLE_SCRIPT_URL`

### 7. Configure timezone

Adjust:

- `GMT_OFFSET_SEC`
- `DAYLIGHT_OFFSET_SEC`

### 8. Configure thresholds

Start with the example thresholds and then calibrate them using live sensor readings.

## Sensor calibration

Calibration is important because different rain-sensor boards and NodeMCU boards may behave differently.

### Suggested calibration method

1. Upload the sketch with debug enabled.
2. Open Serial Monitor at `115200` baud.
3. Observe the raw A0 values when the sensor is fully dry.
4. Sprinkle or wet the plate and observe the values again.
5. Adjust these settings in `config.h`:
   - `NO_RAIN_RAW_THRESHOLD`
   - `LIGHT_RAIN_RAW_MIN`
   - `MODERATE_RAIN_RAW_MIN`
6. Repeat until state transitions are stable.

## Bench testing before remote installation

Before mounting the sensor remotely, test the firmware on a bench.

### Recommended bench tests

- Compile test in Arduino IDE
- Boot and Wi-Fi connection test
- NTP synchronization test
- Telegram alert test
- Telegram command test
- Google Sheets upload test
- Simulated rain-state transitions
- Reboot recovery test
- Wi-Fi loss and reconnect test

### Useful command tests

- `/health` -> confirm Wi-Fi, time, last rain, and signal details
- `/status` -> quick state view
- `/last_rain` -> verify stored timestamp logic
- `/interval 15` -> temporary fast updates for test verification
- `/interval 0` -> return to default interval

## Interpreting Wi-Fi debug logs

The sketch prints detailed Wi-Fi diagnostics to help troubleshoot remote deployments.

Examples of what the logs can tell you:

- whether the target SSID was found in a scan
- current `WiFi.status()` transitions
- whether failure is likely due to missing SSID or password issues
- signal strength of nearby networks
- hints about 2.4 GHz visibility, SSID mismatch, or weak signal

If the logs show `WL_NO_SSID_AVAIL`, the SSID may be missing, hidden, out of range, on the wrong band, or spelled differently than expected.

## Deployment recommendations

For the first live deployment:

- keep debug enabled
- test with a stable power source
- verify Telegram and Sheets both work
- verify timestamps are correct
- confirm the hotspot/router uses 2.4 GHz
- test dry-to-rain and rain-to-dry transitions
- run the node for an extended bench period before final installation

## Security notes

- Never upload your real `config.h` to GitHub.
- Rotate your bot token if it is ever exposed.
- Use `config.example.h` as the public template.
- Limit bot commands by using `COMMAND_CHAT_ID`.

## Troubleshooting

### Device does not connect to Wi-Fi

Check:

- SSID spelling, including spaces
- password
- 2.4 GHz availability
- signal strength
- hotspot visibility
- router MAC filtering or access control

### Telegram messages are not sent

Check:

- bot token
- chat ID
- whether the bot was added to the group/channel
- internet connectivity on the NodeMCU

### Google Sheets updates fail

Check:

- Apps Script deployment mode
- correct `/exec` URL
- public access or suitable permissions
- script response format

### Wrong rain classification

Check:

- sensor calibration values
- moisture on the plate
- noise on analog input
- power stability

## Example GitHub publishing workflow

1. Keep the real `config.h` only on your local machine.
2. Commit `config.example.h` instead.
3. Commit the main `.ino` sketch.
4. Commit this `README.md`.
5. Add screenshots or serial log samples later if needed.

## License

Choose a license before public release, for example MIT, Apache-2.0, or GPL depending on how you want others to reuse the project.
