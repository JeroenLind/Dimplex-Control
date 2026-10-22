/*
 * Dimplex Control: Dimplex Opti-myst bridge (tested on the Engine 56/600): ESP32 + NimBLE + MQTT (Home Assistant) + web control page.
 *
 * WHAT IT DOES
 *   - Pairs with the fireplace over BLE (passkey) and controls it: flame effect on/off, flame level F1-F6,
 *     heater on/off, setpoint, heater power low/high. It reads the internal temperature (register 0x0055).
 *   - Web interface http://dimplex-control.local/ with three pages in a menu: Main (Bluetooth, flame effect, heater,
 *     setpoint), Settings (flame level, heater power, volume, fireplace selection, factory reset) and Debug (raw
 *     registers, temperature history, CSV status log download, system info). Values refresh by themselves every 3 s.
 *   - MQTT with Home Assistant auto-discovery: fireplace (flame effect) switch, flame intensity, heater switch,
 *     setpoint number, Bluetooth link switch (release the link so the original remote can connect), plus read-only sensors (heater power, internal temperature, raw registers).
 *   - Status LED(s): see "LED STATUS" below.
 *   - Volume (register 0x0076) is only offered on the Settings page, for models with a sound effect; it is not
 *     part of MQTT or the status log. See register-map.md.
 *
 * TASK LAYOUT (see task-architecture.md)
 *   loop()    core 1: web server only; never calls BLE or MQTT, so the page stays responsive.
 *   bleTask   core 0: the only code that calls NimBLE; executes queued commands, reads the fireplace every 5 s.
 *   mqttTask  core 0: the only code that calls PubSubClient; reconnects with backoff, publishes from the snapshot.
 *   ledTask   core 1: only drives the LED pin(s) from a few volatile status flags.
 *   Web and MQTT commands go through a queue to bleTask; state goes back through one mutex-protected snapshot.
 *
 * LED STATUS (onboard LED on GPIO 2; many ESP32 boards have one, the red power LED cannot be controlled)
 *   fast blink          setup portal 'Dimplex-Setup' is open
 *   1 flash + pause     no Wi-Fi
 *   2 flashes + pause   Wi-Fi ok, no MQTT broker
 *   3 flashes + pause   Wi-Fi and MQTT ok, no BLE link to the fireplace (connecting)
 *   slow blink (1 s)    all ok, but Bluetooth is released so the original remote can be used
 *   steady on           everything connected
 *   Optional: give LED_PIN_WIFI / LED_PIN_MQTT / LED_PIN_BLE a GPIO number for one external LED per
 *   connection: on = connected, blinking = trying, off = down / released.
 *
 * BLE (NimBLE): passkey pairing (default 064259 for this unit). Raw handles: 0x0040 effect/heater, 0x0014 heater,
 * 0x0018 setpoint, 0x0048 heater power, 0x0042 flame level, 0x0012 clock, 0x0087 init, see register-map.md.
 *
 * Board: ESP32 Dev Module.  Libraries: NimBLE-Arduino 2.x, PubSubClient, WiFiManager, built-in WiFi.
 *
 * CONFIG PORTAL (WiFiManager): no need to re-flash for WiFi/MQTT changes.
 *   - First boot (or Wi-Fi fails): starts an AP "Dimplex-Setup" (password "dimplex123").
 *   - Settings are saved to flash (NVS) and survive reboots and power loss.
 */
#include <WiFi.h>
#include <time.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <atomic>
#include <NimBLEDevice.h>
#include "esp_gap_ble_api.h"
#include <stdarg.h>
#include <esp_system.h>      // esp_reset_reason()
#include <esp_timer.h>       // esp_timer_get_time()

// =====================================================================================================
// 1. SETTINGS
// =====================================================================================================

// ---- LED pins (-1 = not used) ----
#define LED_PIN_STATUS       2      // combined status LED (onboard LED of most ESP32 Dev boards)
#define LED_PIN_WIFI        -1      // optional external LED: Wi-Fi
#define LED_PIN_MQTT        -1      // optional external LED: MQTT broker
#define LED_PIN_BLE         -1      // optional external LED: fireplace (Bluetooth)
#define LED_ACTIVE_HIGH     true    // false if the LEDs are wired to 3V3 (on = pin low)

// ---- Fireplace identity (auto-detected by default; override in the setup portal) ----
// The bridge finds the fireplace by scanning for a BLE device whose advertised name contains "Dimplex"
// (they show up as "FI####<Dimplex>"). To pin a specific unit, enter its BLE address in the portal.
#define FIREPLACE_NAME_MATCH   "Dimplex"
#define DEFAULT_PASSKEY_TEXT   "064259"
static char cfg_fireplaceAddress[20] = "00:a0:50:04:75:d9";   // "" = auto-detect; otherwise a pinned BLE MAC
static char cfg_passkeyText[12]      = DEFAULT_PASSKEY_TEXT;
static uint32_t g_passkeyNumber      = 64259;                  // cfg_passkeyText as a number

// ---- MQTT settings (defaults; the setup portal overrides them and stores them in flash) ----
// >>> USE YOUR OWN MQTT CREDENTIALS <<<  The values below are the author's local broker account.
static char cfg_mqttHost[40]     = "homeassistant.local";   // IP, hostname, or *.local (mDNS)
static char cfg_mqttPort[6]      = "1883";
static char cfg_mqttUser[32]     = "dimplex";
static char cfg_mqttPassword[40] = "dimplex";

// ---- Timing ----
#define FIREPLACE_POLL_INTERVAL_MS   5000     // how often bleTask reads the registers (temperature etc.)
#define BLE_RETRY_INTERVAL_MS        5000     // pause between BLE connect attempts
#define MESSAGE_SHOW_TIME_MS         8000     // how long a result message stays on the web page
#define PAGE_REFRESH_MS              3000     // live block refresh interval
#define PAGE_REFRESH_BUSY_MS         1000     // ... while commands are running or the link is connecting

Preferences g_prefs;
WiFiManager g_wifiManager;
WiFiManagerParameter param_mqttHost("host", "MQTT broker (IP or hostname, e.g. homeassistant.local)", cfg_mqttHost, 40);
WiFiManagerParameter param_mqttPort("port", "MQTT port", cfg_mqttPort, 6);
WiFiManagerParameter param_mqttUser("user", "MQTT username", cfg_mqttUser, 32);
WiFiManagerParameter param_mqttPassword("pass", "MQTT password", cfg_mqttPassword, 40);
// Fireplace picker: a <select> filled from a BLE scan when the portal opens (fills the address field below).
static char g_portalScanHtml[1400] = "";
WiFiManagerParameter param_scanList(g_portalScanHtml);
WiFiManagerParameter param_fireplaceAddress("fpaddr", "Fireplace BLE address (blank = auto-detect)", cfg_fireplaceAddress, 19);
WiFiManagerParameter param_passkey("pkey", "Pairing passkey", cfg_passkeyText, 11);

// ---- MQTT topics (unchanged names, so existing Home Assistant history stays) ----
static const char *TOPIC_AVAILABILITY       = "dimplex/status";
static const char *TOPIC_POWER_SET          = "dimplex/power/set";          // flame effect ON/OFF (OFF also switches the heater off)
static const char *TOPIC_POWER_STATE        = "dimplex/power/state";
static const char *TOPIC_FLAME_LEVEL_SET    = "dimplex/level/set";          // 1..6
static const char *TOPIC_FLAME_LEVEL_STATE  = "dimplex/level/state";
static const char *TOPIC_HEATER_SET         = "dimplex/heater/set";         // ON/OFF
static const char *TOPIC_HEATER_STATE       = "dimplex/heater/state";       // 0x0014 byte0, bit 0x04 clear = heater on
static const char *TOPIC_SETPOINT_SET       = "dimplex/setpoint/set";       // degrees C (15..30), only while the heater is on
static const char *TOPIC_SETPOINT_STATE     = "dimplex/setpoint/state";     // 0x0018, non-zero only while the heater is on
static const char *TOPIC_BLE_SET            = "dimplex/ble/set";            // ON = bridge connects to the fireplace, OFF = release the link (remote can connect)
static const char *TOPIC_BLE_STATE          = "dimplex/ble/state";          // ON/OFF: whether the bridge is allowed to hold the BLE link
static const char *TOPIC_HEATER_POWER_STATE = "dimplex/heater_power/state"; // low/high (0x0048: 01 = 0.9 kW, 03 = 1.8 kW)
static const char *TOPIC_TEMPERATURE_STATE  = "dimplex/temperature/state";  // 0x0055: byte1 = degrees, byte2 = tenths
static const char *TOPIC_RAW_STATE          = "dimplex/raw/state";          // diagnostics: raw bytes of the interesting registers
static const char *TOPIC_DIAG_STATE         = "dimplex/diag/state";         // JSON with restart reason, uptime, memory, signal ... (every 60 s)

WiFiClient g_wifiClient;                  // owned by mqttTask
PubSubClient g_mqttClient(g_wifiClient);  // owned by mqttTask: no other task may call it
WebServer g_webServer(80);                // owned by loop(): http://dimplex-control.local/
static bool g_webStarted = false;

// =====================================================================================================
// 2. SHARED DATA BETWEEN THE TASKS (command queue + state snapshot)
// =====================================================================================================
// Commands go web/MQTT -> bleTask through one queue. State goes bleTask -> web/MQTT through one snapshot that is
// protected by a mutex. The mutex is only held while copying, never during a BLE or network call.

enum CommandType : uint8_t { CMD_FLAME_EFFECT, CMD_HEATER, CMD_HEATER_POWER, CMD_SETPOINT, CMD_SETPOINT_DELTA,
                             CMD_FLAME_LEVEL, CMD_VOLUME, CMD_BLE_RELEASE, CMD_BLE_RESUME,
                             CMD_SCAN, CMD_APPLY_SETTINGS, CMD_FACTORY_RESET };
enum CommandSource : uint8_t { SOURCE_WEB = 0, SOURCE_MQTT = 1 };
struct Command { CommandType type; int16_t argument; CommandSource source; };
static QueueHandle_t g_commandQueue = nullptr;
static SemaphoreHandle_t g_snapshotMutex = nullptr;
static volatile bool g_commandRunning = false;             // bleTask is executing a command right now
static std::atomic<uint32_t> g_commandsQueued(0);          // commands accepted into the queue so far
static std::atomic<uint32_t> g_commandsCompleted(0);       // commands bleTask has finished (and put the result in the snapshot)

#define HISTORY_SIZE 120
struct StatusSample {                       // one row of the temperature table and the /csv log
  uint32_t uptimeSec, epoch;                // uptime in seconds, unix time (0 = no NTP time yet)
  char temperatureText[8], temperatureRaw[8];   // "21.5" and the raw bytes of 0x0055
  uint8_t control0, control1, heater0, heaterPower, flameLevel;   // 0x0040 (2 bytes), 0x0014 byte 0, 0x0048, 0x0042
  uint16_t setpoint, unknown3c;             // 0x0018, 0x003c
};
struct ScanEntry { char name[28]; char address[20]; int rssi; };   // one fireplace found by a BLE scan
#define SCAN_MAX 8
// Everything the web page and MQTT need. Plain data only (memcpy-able), no String.
struct SharedState {
  bool valid, connected, ready, bleEnabled, haveTarget;
  uint8_t regControl[2], regHeater[2], regHeaterPower, flameLevel, volume, regTemperature[3], regUnknown3c[3], regUnknown63[2];
  int setpoint, setpointMin, setpointMax;
  char targetAddress[20];                    // fireplace we are connected to / will connect to
  char configuredAddress[20], passkeyText[12];   // selected fireplace ("" = auto-detect) and passkey, as saved
  ScanEntry scan[SCAN_MAX]; int scanCount; bool scanning; uint32_t scanFinishedMs;   // result of the last scan
  char message[96]; uint32_t messageTimeMs;  // last result message of the BLE task and when it was set
  uint32_t lastReadMs;                       // millis() of the last successful register read
  StatusSample history[HISTORY_SIZE]; int historyCount, historyHead; float tempMin, tempMax; uint32_t lastTempChangeMs;
};
static SharedState g_shared;                       // written by bleTask under g_snapshotMutex
static volatile uint32_t g_sharedVersion = 0;      // incremented on every update of g_shared
static volatile uint32_t g_bleReadyCount = 0;      // incremented every time the BLE link becomes ready (MQTT republishes everything)
static SharedState g_webSnapshot;                  // private copy, used by loop() only
static SharedState g_mqttSnapshot;                 // private copy, used by mqttTask only

// Diagnostics shown on the web page (each is written by one task only).
static volatile uint32_t g_loopGapMaxUs = 0, g_webRequestMaxUs = 0, g_mqttFailCount = 0, g_commandsDropped = 0;
static volatile uint32_t g_bleStackFree = 0, g_mqttStackFree = 0;
static volatile bool g_mqttUp = false;
static volatile bool g_portalOpen = false;         // the 'Dimplex-Setup' portal is open (for the LED)

// NOTE for the Arduino IDE: it inserts automatic function prototypes before the FIRST function in this file. All types
// (struct / enum) must therefore be defined above the first function, otherwise the build fails with "does not name a type".
// Types used by the web feedback (see "feedback for pressed buttons" further down):
enum AwaitedControl { AW_BLE, AW_EFFECT, AW_HEATER, AW_HEATER_POWER, AW_SETPOINT, AW_FLAME_LEVEL, AW_VOLUME, AW_COUNT };
struct AwaitedValue { bool active; int target; uint32_t sinceMs; };

// ---- crash diagnostics: a small log that survives a restart ----
// The last log lines and the last step of each task are kept in RTC memory. That memory keeps its content over a crash,
// watchdog reset or software restart (not over a power loss). After a restart the previous session's lines are shown on the
// Debug page, at /log and in Home Assistant, so a crash can be investigated without a USB cable.
#define LOG_RING_LINES        30
#define LOG_RING_LINE_LENGTH  96
#define STEP_TEXT_LENGTH      48
#define RTC_LOG_MAGIC         0xD1A7C0DEu
struct RtcLog {
  uint32_t magic;                                     // RTC_LOG_MAGIC while the content is valid
  uint32_t head, count;                               // next line to write, number of stored lines
  uint32_t plannedRestart;                            // RTC_LOG_MAGIC when this restart was requested by us (restartDevice)
  char plannedReason[STEP_TEXT_LENGTH];               // why we asked for it
  char bleStep[STEP_TEXT_LENGTH], mqttStep[STEP_TEXT_LENGTH], webStep[STEP_TEXT_LENGTH];   // what each task was doing last
  char lines[LOG_RING_LINES][LOG_RING_LINE_LENGTH];
  uint32_t magicEnd;
};
static RTC_NOINIT_ATTR RtcLog g_rtcLog;
static SemaphoreHandle_t g_logMutex = nullptr;        // protects the ring (logLine is called from several tasks)
// What the previous session left behind (filled once at boot by startDiagnosticsSession()).
static char g_previousLog[LOG_RING_LINES * (LOG_RING_LINE_LENGTH + 1) + 1] = "";
static char g_previousBleStep[STEP_TEXT_LENGTH] = "", g_previousMqttStep[STEP_TEXT_LENGTH] = "", g_previousWebStep[STEP_TEXT_LENGTH] = "";
static bool g_previousLogValid = false;
static char g_restartReason[96] = "unknown";          // why the bridge started (shown on the Debug page and in Home Assistant)
static uint32_t g_bootCount = 0;                      // number of starts since the last factory reset

// =====================================================================================================
// LOGGING
// =====================================================================================================
// Write one line to the serial terminal as "[<ms since boot> ms] [<tag>] <message>". Use this instead of
// Serial.print* so every line has a timestamp. The line is built first and written in one piece, so lines from
// different tasks do not get mixed up. `format` works like printf; no newline needed.
static void logLine(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void logLine(const char *tag, const char *format, ...) {
  char message[240];
  va_list arguments; va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  char line[290];
  snprintf(line, sizeof(line), "[%lu ms] [%s] %s", (unsigned long) millis(), tag, message);
  Serial.println(line);
  if (g_logMutex && g_rtcLog.magic == RTC_LOG_MAGIC) {                       // keep the line in the restart-proof ring
    xSemaphoreTake(g_logMutex, portMAX_DELAY);
    strlcpy(g_rtcLog.lines[g_rtcLog.head], line, LOG_RING_LINE_LENGTH);
    g_rtcLog.head = (g_rtcLog.head + 1) % LOG_RING_LINES;
    if (g_rtcLog.count < LOG_RING_LINES) g_rtcLog.count = g_rtcLog.count + 1;
    xSemaphoreGive(g_logMutex);
  }
}

// ---- crash diagnostics: steps, restart reason, previous session ----
// Remember what a task is doing right now. If the bridge crashes, the last step of each task is shown after the restart.
static inline void setBleStep(const char *text)  { strlcpy(g_rtcLog.bleStep, text, sizeof(g_rtcLog.bleStep)); }
static inline void setMqttStep(const char *text) { strlcpy(g_rtcLog.mqttStep, text, sizeof(g_rtcLog.mqttStep)); }
static inline void setWebStep(const char *text)  { strlcpy(g_rtcLog.webStep, text, sizeof(g_rtcLog.webStep)); }

// Write the lines of a ring as text, oldest first, one per line. Returns the text length.
static size_t joinRingLines(const RtcLog &log, char *out, size_t outSize) {
  size_t length = 0;
  out[0] = 0;
  for (uint32_t i = 0; i < log.count; i++) {
    uint32_t index = (log.head + LOG_RING_LINES - log.count + i) % LOG_RING_LINES;
    size_t lineLength = strnlen(log.lines[index], LOG_RING_LINE_LENGTH);
    if (length + lineLength + 2 > outSize) break;
    memcpy(out + length, log.lines[index], lineLength); length += lineLength;
    out[length++] = '\n'; out[length] = 0;
  }
  return length;
}
// Human readable text for the reason of the last reset.
static const char *resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_EXT:       return "External reset pin";
    case ESP_RST_SW:        return "Software restart";
    case ESP_RST_PANIC:     return "Crash (panic / exception)";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "Task watchdog";
    case ESP_RST_WDT:       return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake-up";
    case ESP_RST_BROWNOUT:  return "Brownout (supply voltage too low)";
    default:                return "Unknown";
  }
}
// First thing at boot: save what the previous session left in RTC memory (last log lines, last steps), work out why the
// bridge restarted, count the start and begin a fresh ring for this session.
static void startDiagnosticsSession() {
  g_logMutex = xSemaphoreCreateMutex();
  esp_reset_reason_t reason = esp_reset_reason();
  bool valid = g_rtcLog.magic == RTC_LOG_MAGIC && g_rtcLog.magicEnd == RTC_LOG_MAGIC && g_rtcLog.head < LOG_RING_LINES && g_rtcLog.count <= LOG_RING_LINES;
  g_previousLogValid = valid && g_rtcLog.count > 0;
  bool planned = valid && g_rtcLog.plannedRestart == RTC_LOG_MAGIC;
  if (valid) {
    joinRingLines(g_rtcLog, g_previousLog, sizeof(g_previousLog));
    strlcpy(g_previousBleStep, g_rtcLog.bleStep, sizeof(g_previousBleStep));
    strlcpy(g_previousMqttStep, g_rtcLog.mqttStep, sizeof(g_previousMqttStep));
    strlcpy(g_previousWebStep, g_rtcLog.webStep, sizeof(g_previousWebStep));
  }
  if (reason == ESP_RST_SW && planned) snprintf(g_restartReason, sizeof(g_restartReason), "Software restart: %s", g_rtcLog.plannedReason);
  else if (reason == ESP_RST_SW)       snprintf(g_restartReason, sizeof(g_restartReason), "Software restart (not requested from the web page)");
  else                                 snprintf(g_restartReason, sizeof(g_restartReason), "%s", resetReasonText(reason));
  memset(&g_rtcLog, 0, sizeof(g_rtcLog));                // fresh ring for this session
  g_rtcLog.magic = g_rtcLog.magicEnd = RTC_LOG_MAGIC;
  g_prefs.begin("cfg", false); g_bootCount = g_prefs.getUInt("boots", 0) + 1; g_prefs.putUInt("boots", g_bootCount); g_prefs.end();
}
// Restart on purpose and remember why, so the next start can tell it was not a crash.
static void restartDevice(const char *why) {
  strlcpy(g_rtcLog.plannedReason, why, sizeof(g_rtcLog.plannedReason));
  g_rtcLog.plannedRestart = RTC_LOG_MAGIC;
  logLine("boot", "restarting: %s", why);
  ESP.restart();
}

// Copy the shared snapshot into a private copy (the only way readers look at it).
static void copySnapshot(SharedState &destination) {
  xSemaphoreTake(g_snapshotMutex, portMAX_DELAY); memcpy(&destination, &g_shared, sizeof(SharedState)); xSemaphoreGive(g_snapshotMutex);
}
static inline bool isFlameEffectOn(const SharedState &s) { return s.valid && s.regControl[0] != 0; }
static inline bool isHeaterOn(const SharedState &s)      { return s.valid && s.regControl[0] != 0 && (s.regHeater[0] & 0x04) == 0; }

// New fireplace settings entered on the web page. loop() stages them here (under the mutex); bleTask validates
// and applies them (CMD_APPLY_SETTINGS), so the settings variables are only changed by one task.
static char g_pendingAddress[24] = "", g_pendingPasskey[16] = "";
static bool g_pendingHasAddress = false, g_pendingHasPasskey = false;
static void stagePendingSettings(const String &address, bool hasAddress, const String &passkey, bool hasPasskey) {
  xSemaphoreTake(g_snapshotMutex, portMAX_DELAY);
  strlcpy(g_pendingAddress, address.c_str(), sizeof(g_pendingAddress)); g_pendingHasAddress = hasAddress;
  strlcpy(g_pendingPasskey, passkey.c_str(), sizeof(g_pendingPasskey)); g_pendingHasPasskey = hasPasskey;
  xSemaphoreGive(g_snapshotMutex);
}

// Put a command in the queue for bleTask. Never blocks. Returns false when the queue is full.
static bool queueCommand(CommandType type, int argument, CommandSource source) {
  Command c = { type, (int16_t) argument, source };
  if (!g_commandQueue || xQueueSend(g_commandQueue, &c, 0) != pdTRUE) { g_commandsDropped = g_commandsDropped + 1; return false; }
  g_commandsQueued++;
  return true;
}

// =====================================================================================================
// 3. BLE STATE (private to bleTask; the NimBLE callbacks only touch the volatile flags)
// =====================================================================================================
static NimBLEClient *g_bleClient = nullptr;
static NimBLERemoteCharacteristic *g_chControl = nullptr, *g_chFlameLevel = nullptr, *g_chClock = nullptr, *g_chInit = nullptr;
static NimBLERemoteCharacteristic *g_chHeater = nullptr, *g_chSetpoint = nullptr, *g_chHeaterPower = nullptr, *g_chTemperature = nullptr;
static NimBLERemoteCharacteristic *g_chUnknown3c = nullptr, *g_chUnknown63 = nullptr, *g_chSetpointLimits = nullptr, *g_chVolume = nullptr;
static volatile bool g_bleConnected = false, g_blePaired = false;
static volatile bool g_bleReady = false, g_initSent = false;
static volatile bool g_bleEnabled = true;       // false = link released so the original remote can connect
static volatile bool g_clockSet = false;        // fireplace clock written (with real time) on this connection
static volatile bool g_registersRead = false;   // readFireplaceState() has read the registers on this connection
static volatile bool g_limitsRead = false;
static uint32_t g_lastConnectAttemptMs = 0;
static bool g_haveTarget = false;               // g_targetAddress holds a resolved address
static NimBLEAddress g_targetAddress;           // the fireplace we connect to (pinned or auto-detected)
static char g_detectedAddress[20] = "";         // last auto-detected address, remembered in NVS
static bool g_ntpStarted = false;               // loop() only

// ---- live state of the fireplace (filled by readFireplaceState(), copied to g_shared by publishSnapshot()) ----
static uint8_t g_regControl[2] = {0, 0}, g_regHeater[2] = {0, 0}, g_regHeaterPower = 0;
static uint8_t g_regTemperature[3] = {0, 0, 0}, g_regUnknown3c[3] = {0, 0, 0}, g_regUnknown63[2] = {0, 0};
static uint8_t g_flameLevel = 0, g_volume = 0;
static int g_setpoint = 0;                      // 0x0018 (0 while the heater is off)
static int g_setpointMin = 15, g_setpointMax = 30;   // replaced by the limits read from 0x001e
static String g_message = "";                   // result of the last action; shown on the web page for a few seconds
static uint32_t g_messageTimeMs = 0;
static volatile bool g_messageDirty = false;
static uint32_t g_lastReadMs = 0;
static ScanEntry g_scanEntries[SCAN_MAX]; static int g_scanCount = 0; static bool g_scanning = false; static uint32_t g_scanFinishedMs = 0;
static StatusSample g_history[HISTORY_SIZE]; static int g_historyCount = 0, g_historyHead = 0;
static float g_tempMin = 999, g_tempMax = -999;
static uint32_t g_lastTempChangeMs = 0, g_lastHistoryLogMs = 0;

static void readFireplaceState();

// Remember a one-line result for the web page and the serial log.
static void setMessage(const String &text) { g_message = text; g_messageTimeMs = millis(); g_messageDirty = true; }

// Copy bleTask's private state into the shared snapshot. Called by bleTask whenever something changed.
static void publishSnapshot() {
  char target[20] = "";
  if (g_haveTarget) strlcpy(target, g_targetAddress.toString().c_str(), sizeof(target));   // before taking the lock
  xSemaphoreTake(g_snapshotMutex, portMAX_DELAY);
  SharedState &s = g_shared;
  s.valid = g_registersRead; s.connected = g_bleConnected; s.ready = g_bleReady; s.bleEnabled = g_bleEnabled;
  s.haveTarget = g_haveTarget; strlcpy(s.targetAddress, target, sizeof(s.targetAddress));
  memcpy(s.regControl, g_regControl, 2); memcpy(s.regHeater, g_regHeater, 2); memcpy(s.regTemperature, g_regTemperature, 3);
  memcpy(s.regUnknown3c, g_regUnknown3c, 3); memcpy(s.regUnknown63, g_regUnknown63, 2);
  s.regHeaterPower = g_regHeaterPower; s.flameLevel = g_flameLevel; s.volume = g_volume; s.setpoint = g_setpoint;
  strlcpy(s.configuredAddress, cfg_fireplaceAddress, sizeof(s.configuredAddress)); strlcpy(s.passkeyText, cfg_passkeyText, sizeof(s.passkeyText));
  memcpy(s.scan, g_scanEntries, sizeof(g_scanEntries)); s.scanCount = g_scanCount; s.scanning = g_scanning; s.scanFinishedMs = g_scanFinishedMs;
  s.setpointMin = g_setpointMin; s.setpointMax = g_setpointMax; s.lastReadMs = g_lastReadMs;
  if (g_messageDirty) { strlcpy(s.message, g_message.c_str(), sizeof(s.message)); s.messageTimeMs = g_messageTimeMs; g_messageDirty = false; }
  memcpy(s.history, g_history, sizeof(g_history)); s.historyCount = g_historyCount; s.historyHead = g_historyHead;
  s.tempMin = g_tempMin; s.tempMax = g_tempMax; s.lastTempChangeMs = g_lastTempChangeMs;
  g_sharedVersion = g_sharedVersion + 1;
  xSemaphoreGive(g_snapshotMutex);
}

// =====================================================================================================
// 4. SETTINGS IN FLASH
// =====================================================================================================

// Turn the passkey text into a number (6-digit legacy passkey); falls back to the default for bad input.
static void parsePasskey() {
  unsigned long k = strtoul(cfg_passkeyText, nullptr, 10);
  g_passkeyNumber = (k > 0 && k <= 999999) ? (uint32_t) k : 64259;
}
// Read all settings from flash. A value that was never saved keeps its default from section 1.
static void loadSettings() {
  g_prefs.begin("cfg", true);
  strlcpy(cfg_mqttHost, g_prefs.getString("host", cfg_mqttHost).c_str(), sizeof(cfg_mqttHost));
  strlcpy(cfg_mqttPort, g_prefs.getString("port", cfg_mqttPort).c_str(), sizeof(cfg_mqttPort));
  strlcpy(cfg_mqttUser, g_prefs.getString("user", cfg_mqttUser).c_str(), sizeof(cfg_mqttUser));
  strlcpy(cfg_mqttPassword, g_prefs.getString("pass", cfg_mqttPassword).c_str(), sizeof(cfg_mqttPassword));
  strlcpy(cfg_fireplaceAddress, g_prefs.getString("addr", cfg_fireplaceAddress).c_str(), sizeof(cfg_fireplaceAddress));
  strlcpy(cfg_passkeyText, g_prefs.getString("pkey", cfg_passkeyText).c_str(), sizeof(cfg_passkeyText));
  strlcpy(g_detectedAddress, g_prefs.getString("detaddr", "").c_str(), sizeof(g_detectedAddress));
  g_prefs.end();
  parsePasskey();
}
// Remember (or clear, with nullptr) the auto-detected fireplace address so later boots can skip the scan.
static void rememberDetectedAddress(const char *mac) {
  strlcpy(g_detectedAddress, mac ? mac : "", sizeof(g_detectedAddress));
  g_prefs.begin("cfg", false); g_prefs.putString("detaddr", g_detectedAddress); g_prefs.end();
}
// WiFiManager calls this when 'Save' is pressed in the portal: copy the fields, store them and
// make the bridge look up the fireplace and broker again.
static void onPortalSettingsSaved() {
  strncpy(cfg_mqttHost, param_mqttHost.getValue(), sizeof(cfg_mqttHost));
  strncpy(cfg_mqttPort, param_mqttPort.getValue(), sizeof(cfg_mqttPort));
  strncpy(cfg_mqttUser, param_mqttUser.getValue(), sizeof(cfg_mqttUser));
  strncpy(cfg_mqttPassword, param_mqttPassword.getValue(), sizeof(cfg_mqttPassword));
  strncpy(cfg_fireplaceAddress, param_fireplaceAddress.getValue(), sizeof(cfg_fireplaceAddress));
  strncpy(cfg_passkeyText, param_passkey.getValue(), sizeof(cfg_passkeyText));
  g_prefs.begin("cfg", false);
  g_prefs.putString("host", cfg_mqttHost); g_prefs.putString("port", cfg_mqttPort);
  g_prefs.putString("user", cfg_mqttUser); g_prefs.putString("pass", cfg_mqttPassword);
  g_prefs.putString("addr", cfg_fireplaceAddress); g_prefs.putString("pkey", cfg_passkeyText);
  g_prefs.putString("detaddr", "");   // forget any remembered auto-detect; resolve fresh
  g_prefs.end();
  g_detectedAddress[0] = 0;
  parsePasskey();
  g_haveTarget = false;
  logLine("cfg", "saved: host=%s port=%s user=%s addr=%s",
          cfg_mqttHost, cfg_mqttPort, cfg_mqttUser, strlen(cfg_fireplaceAddress) ? cfg_fireplaceAddress : "(auto)");
}

// =====================================================================================================
// 5. BLE (everything below calls NimBLE and runs in bleTask)
// =====================================================================================================

// NimBLE callbacks. They run in the NimBLE host task, so they only set flags and answer the pairing prompts.
class FireplaceClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override {
    g_bleConnected = true;
    logLine("ble", "connected");
  }
  void onDisconnect(NimBLEClient *, int reason) override {
    logLine("ble", "disconnected (%d)", reason);
    // Only flags here. The characteristic pointers are NOT cleared: bleTask may be in the middle of a read or write, and a
    // pointer that turns null under its hands crashes it (LoadProhibited). bleTask replaces them at the next connection,
    // and every access goes through readRegister()/writeRegister(), which refuse to work while the link is down.
    g_bleConnected = g_blePaired = g_bleReady = g_initSent = false;
    g_clockSet = false; g_registersRead = false; g_limitsRead = false;
  }
  void onPassKeyEntry(NimBLEConnInfo &info) override { NimBLEDevice::injectPassKey(info, g_passkeyNumber); }
  void onConfirmPasskey(NimBLEConnInfo &info, uint32_t) override { NimBLEDevice::injectConfirmPasskey(info, true); }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    logLine("sec", "auth complete enc=%d", info.isEncrypted()); g_blePaired = info.isEncrypted();
  }
};

// Find the characteristics we use by their raw handle (the fireplace has no useful UUID names).
// Returns true when at least the control register 0x0040 exists.
static bool findFireplaceCharacteristics() {
  g_chControl = g_chFlameLevel = g_chClock = g_chInit = nullptr;
  g_chHeater = g_chSetpoint = g_chHeaterPower = g_chTemperature = g_chUnknown3c = g_chUnknown63 = g_chSetpointLimits = g_chVolume = nullptr;
  for (auto *service : g_bleClient->getServices(true))
    for (auto *c : service->getCharacteristics(true)) {
      switch (c->getHandle()) {
        case 0x0040: g_chControl = c; break;          // effect / heater state
        case 0x0042: g_chFlameLevel = c; break;       // flame intensity 1..6
        case 0x0012: g_chClock = c; break;
        case 0x0087: g_chInit = c; break;
        case 0x0014: g_chHeater = c; break;           // heater state byte
        case 0x0018: g_chSetpoint = c; break;
        case 0x0048: g_chHeaterPower = c; break;
        case 0x0055: g_chTemperature = c; break;
        case 0x003c: g_chUnknown3c = c; break;
        case 0x0063: g_chUnknown63 = c; break;
        case 0x001e: g_chSetpointLimits = c; break;
        case 0x0076: g_chVolume = c; break;           // volume (models with a sound effect only)
        default: break;
      }
    }
  return g_chControl != nullptr;
}

// Write `length` bytes to a register (with response). Returns false when the register is unknown, the link is down
// (also when it dropped a moment ago) or the write failed. Never dereferences a null pointer.
static bool writeRegister(NimBLERemoteCharacteristic *characteristic, const uint8_t *data, size_t length) {
  if (!characteristic || !g_bleConnected) return false;
  return characteristic->writeValue(data, length, true);
}

// Write the real local time to the fireplace clock (0x0012), in the format the remote uses:
// century, year, month, day, hour, minute, flag, seconds (plain binary, not BCD).
// Example: 2026-10-14 19:45:04 -> 14 1a 0a 0e 13 2d 01 04. The flag byte is always 01 as written by the remote.
// Returns false when NTP has no valid time yet; nothing is written then, so the fireplace keeps its own time.
static bool writeFireplaceClock() {
  if (!g_chClock) return false;
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;               // not NTP-synced yet
  int year = t.tm_year + 1900;
  uint8_t clock[8] = { (uint8_t)(year / 100), (uint8_t)(year % 100), (uint8_t)(t.tm_mon + 1), (uint8_t) t.tm_mday,
                       (uint8_t) t.tm_hour, (uint8_t) t.tm_min, 0x01, (uint8_t) t.tm_sec };
  if (!writeRegister(g_chClock, clock, sizeof(clock))) return false;
  g_clockSet = true;
  logLine("ble", "fireplace clock set to %04d-%02d-%02d %02d:%02d:%02d",
          year, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return true;
}
// Handshake the remote does after connecting: set the clock and write 0x0087 = 010100, then 000000.
static void sendInitSequence() {
  uint8_t first[] = {0x01, 0x01, 0x00}, second[] = {0x00, 0x00, 0x00};
  if (!writeFireplaceClock()) logLine("ble", "no valid time yet; clock write postponed");
  writeRegister(g_chInit, first, sizeof(first)); delay(120); writeRegister(g_chInit, second, sizeof(second));
  g_initSent = true;
}

// Hang up and stay disconnected, so the original remote can use the fireplace's single BLE link.
static void bleReleaseLink() {
  g_bleEnabled = false;
  if (g_bleClient && g_bleClient->isConnected()) {
    g_bleClient->disconnect();                      // onDisconnect() clears the state flags
    uint32_t start = millis();
    while (g_bleConnected && millis() - start < 2000) delay(20);
  }
  logLine("ble", "released (original remote can connect)");
}
// Allow connecting again; bleTask makes the next connect attempt immediately.
static void bleResumeLink() {
  g_bleEnabled = true;
  g_lastConnectAttemptMs = millis() - (BLE_RETRY_INTERVAL_MS + 1000);
  logLine("ble", "resume requested");
}

// Scan for a fireplace by advertised name. Returns true and fills `found` on the first match.
static bool scanForFireplace(NimBLEAddress &found) {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);                        // active scan -> we get scan-response names
  NimBLEScanResults results = scan->getResults(4000, false);
  bool ok = false;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (String(device->getName().c_str()).indexOf(FIREPLACE_NAME_MATCH) < 0) continue;
    found = device->getAddress(); ok = true; break;
  }
  scan->clearResults();
  return ok;
}
// Scan and build the portal's <select> of found fireplaces (choosing one fills the address field).
static void buildPortalScanList() {
  logLine("ble", "scanning for fireplaces (setup portal)...");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(4000, false);
  String html = "<label for='fpaddr'>Fireplace (from scan)</label>"
                "<select onchange=\"var e=document.getElementById('fpaddr');if(e)e.value=this.value;\" "
                "style='width:100%;padding:5px;margin:2px 0'>"
                "<option value=''>Auto-detect (recommended)</option>";
  int count = 0;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    String name = device->getName().c_str();
    if (name.indexOf(FIREPLACE_NAME_MATCH) < 0) continue;
    String mac = device->getAddress().toString().c_str();
    name.replace("<", "&lt;"); name.replace(">", "&gt;");     // "<Dimplex>" would break the HTML
    html += "<option value='" + mac + "'>" + name + " &mdash; " + mac + "</option>";
    count++;
  }
  html += "</select>";
  scan->clearResults();
  strlcpy(g_portalScanHtml, html.c_str(), sizeof(g_portalScanHtml));
  logLine("ble", "scan found %d fireplace(s)", count);
}

// One connect attempt: decide which fireplace (pinned address, remembered address or a scan), connect and pair.
// May block for seconds (scan 4 s, connect timeout 5 s); that is fine because only bleTask waits.
static void bleConnectToFireplace() {
  logLine("ble", "Connecting");
  if (g_bleConnected) return;
  if (!g_bleClient) {
    g_bleClient = NimBLEDevice::createClient();
    g_bleClient->setClientCallbacks(new FireplaceClientCallbacks(), false);
    g_bleClient->setConnectTimeout(5000);           // while the original remote holds the only link, connect() cannot succeed
  }
  if (!g_haveTarget) {
    if (strlen(cfg_fireplaceAddress) >= 17) {       // pinned address from the portal
      g_targetAddress = NimBLEAddress(cfg_fireplaceAddress, BLE_ADDR_PUBLIC); g_haveTarget = true;
    } else if (strlen(g_detectedAddress) >= 17) {   // remembered from a previous auto-detect
      g_targetAddress = NimBLEAddress(g_detectedAddress, BLE_ADDR_PUBLIC); g_haveTarget = true;
      logLine("ble", "using remembered fireplace %s", g_detectedAddress);
    } else if (scanForFireplace(g_targetAddress)) { // fresh auto-detect by name
      g_haveTarget = true;
      rememberDetectedAddress(g_targetAddress.toString().c_str());
      logLine("ble", "auto-detected fireplace %s (remembered)", g_detectedAddress);
    } else {
      logLine("ble", "no fireplace found in scan (will retry)"); return;
    }
  }
  if (!g_bleClient->connect(g_targetAddress)) {
    logLine("ble", "connect failed");
    if (!strlen(cfg_fireplaceAddress)) {            // auto mode: forget the target (and a remembered address) and re-scan
      g_haveTarget = false;
      if (strlen(g_detectedAddress)) { logLine("ble", "forgetting remembered address; will re-scan"); rememberDetectedAddress(nullptr); }
    }
    return;
  }
  if (!g_bleClient->secureConnection()) { logLine("ble", "secure failed"); return; }
}

// Read up to maxLength bytes of a register into out[]. Returns the number of bytes read (0 = failed).
static size_t readRegister(NimBLERemoteCharacteristic *characteristic, uint8_t *out, size_t maxLength) {
  if (!characteristic || !g_bleConnected) return 0;
  NimBLEAttValue value = characteristic->readValue();
  size_t length = value.length() < maxLength ? value.length() : maxLength;
  for (size_t i = 0; i < length; i++) out[i] = value[i];
  return length;
}
// Write `count` bytes as lowercase hex into dst (2 chars per byte plus a terminator; dst must be big enough).
static void bytesToHex(char *dst, const uint8_t *bytes, size_t count) { for (size_t i = 0; i < count; i++) sprintf(dst + 2 * i, "%02x", bytes[i]); }

// Add a row to the status history when something changed (or at least once a minute), and track min/max temperature.
static void logStatusSample() {
  char text[8]; snprintf(text, sizeof(text), "%u.%u", g_regTemperature[1], g_regTemperature[2] % 10);
  float degrees = g_regTemperature[1] + (g_regTemperature[2] % 10) / 10.0f;
  if (degrees < g_tempMin) g_tempMin = degrees;
  if (degrees > g_tempMax) g_tempMax = degrees;
  StatusSample n = {};
  n.uptimeSec = millis() / 1000; time_t now = time(nullptr); n.epoch = (now > 1700000000) ? (uint32_t) now : 0;
  strlcpy(n.temperatureText, text, sizeof(n.temperatureText)); bytesToHex(n.temperatureRaw, g_regTemperature, 3);
  n.control0 = g_regControl[0]; n.control1 = g_regControl[1]; n.heater0 = g_regHeater[0]; n.heaterPower = g_regHeaterPower;
  n.flameLevel = g_flameLevel; n.setpoint = (uint16_t) g_setpoint; n.unknown3c = (uint16_t)(g_regUnknown3c[1] | (g_regUnknown3c[2] << 8));
  const StatusSample &last = g_history[(g_historyHead + HISTORY_SIZE - 1) % HISTORY_SIZE];
  bool changed = (g_historyCount == 0) || strcmp(last.temperatureText, n.temperatureText) != 0 || last.control0 != n.control0 ||
                 last.control1 != n.control1 || last.heater0 != n.heater0 || last.heaterPower != n.heaterPower ||
                 last.flameLevel != n.flameLevel || last.setpoint != n.setpoint;
  if (changed || millis() - g_lastHistoryLogMs > 60000) {
    g_history[g_historyHead] = n;
    g_historyHead = (g_historyHead + 1) % HISTORY_SIZE; if (g_historyCount < HISTORY_SIZE) g_historyCount++;
    g_lastHistoryLogMs = millis(); if (changed) g_lastTempChangeMs = millis();
  }
}
// Read the registers we use into the g_* state variables. Called every 5 s and after every command.
// Does not publish anything; publishSnapshot() does that.
static void readFireplaceState() {
  if (!g_bleReady || !g_chControl) return;
  uint8_t control[2] = {0, 0}, heater[2] = {0, 0}, setpoint[2] = {0, 0}, power[1] = {0}, temperature[3] = {0, 0, 0};
  size_t nControl = readRegister(g_chControl, control, 2), nHeater = readRegister(g_chHeater, heater, 2);
  size_t nSetpoint = readRegister(g_chSetpoint, setpoint, 2), nPower = readRegister(g_chHeaterPower, power, 1);
  size_t nTemperature = readRegister(g_chTemperature, temperature, 3);
  if (nControl >= 1) { g_regControl[0] = control[0]; g_regControl[1] = nControl > 1 ? control[1] : 0; }
  if (nHeater >= 1)  { g_regHeater[0] = heater[0]; g_regHeater[1] = nHeater > 1 ? heater[1] : 0; }
  if (nSetpoint >= 2) g_setpoint = setpoint[0] | (setpoint[1] << 8);
  if (nPower >= 1) g_regHeaterPower = power[0];
  readRegister(g_chUnknown3c, g_regUnknown3c, 3); readRegister(g_chUnknown63, g_regUnknown63, 2);
  { uint8_t level[1]; if (readRegister(g_chFlameLevel, level, 1) >= 1) g_flameLevel = level[0]; }
  { uint8_t volume[1]; if (readRegister(g_chVolume, volume, 1) >= 1) g_volume = volume[0]; }
  if (!g_limitsRead && g_chSetpointLimits) {         // 0x001e = min, max setpoint (LE16 each), e.g. 0f00 1e00 = 15..30
    uint8_t limits[4];
    if (readRegister(g_chSetpointLimits, limits, 4) == 4) {
      int lo = limits[0] | (limits[1] << 8), hi = limits[2] | (limits[3] << 8);
      if (lo >= 5 && hi <= 35 && lo < hi) { g_setpointMin = lo; g_setpointMax = hi; }
      g_limitsRead = true;
    }
  }
  if (nTemperature >= 3) { memcpy(g_regTemperature, temperature, 3); logStatusSample(); }
  g_registersRead = true;
  g_lastReadMs = millis();
}
// True when the last read showed the flame effect on (0x0040 not 00).
static bool flameEffectIsOn() { return g_registersRead && g_regControl[0] != 0; }
// True when the last read showed the heater on (flame effect on and bit 0x04 of 0x0014 byte 0 clear).
static bool heaterIsOn()      { return g_registersRead && g_regControl[0] != 0 && (g_regHeater[0] & 0x04) == 0; }

// After a write: read the registers back until `condition` is true (the fireplace can need a second or more to report
// its new state) or `timeoutMs` has passed. Without this, the first read-back could still show the old state and
// MQTT would publish it, so a Home Assistant switch flipped back for a moment. The log line shows the real delay.
static void readUntil(bool (*condition)(), uint32_t timeoutMs, const char *what) {
  uint32_t start = millis();
  delay(250);
  readFireplaceState();
  while (!condition() && g_bleConnected && millis() - start < timeoutMs) { delay(300); readFireplaceState(); }
  logLine("ble", "%s: %s after %lu ms", what, condition() ? "state confirmed" : "state NOT confirmed", (unsigned long)(millis() - start));
}
static int g_awaitedSetpoint = 0;                // target of the setpoint command being confirmed (bleTask only)
static int g_awaitedHeaterPower = 0;             // target value of 0x0048 being confirmed (bleTask only)

// ---- control commands (verified in the sniff of the remote, except heater power: see bleSetHeaterPower) ----
// Each one returns true when something was sent (or nothing needed sending) and leaves a message for the web page.

// Check the link is up and the registers are known; otherwise leave a message and return false.
static bool requireBleReady() { if (g_bleReady && g_chControl) return true; setMessage("Not connected to the fireplace"); return false; }

// Flame effect: 0x0040 = 06 (effect on, heater off) or 00 (everything off, which also switches the heater off).
static bool bleSetFlameEffect(bool on) {
  if (!requireBleReady()) return false;
  readFireplaceState();
  if (on && flameEffectIsOn()) { setMessage("Flame effect is already on"); return true; }
  if (!g_initSent) sendInitSequence();
  uint8_t value = on ? 0x06 : 0x00; writeRegister(g_chControl, &value, 1);
  logLine("ble", "flame effect -> %s (0x0040=%02x)", on ? "ON" : "OFF", value);
  setMessage(on ? "Flame effect switched on" : "Fireplace switched off (effect and heater)");
  if (on) readUntil([] { return flameEffectIsOn(); }, 4000, "flame effect on");
  else    readUntil([] { return !flameEffectIsOn(); }, 4000, "fireplace off");
  return true;
}
// Flame intensity: 0x0042 = 1..6 (independent of power).
static bool bleSetFlameLevel(uint8_t level) {
  if (!g_bleReady || !g_chFlameLevel) { setMessage("Not connected to the fireplace"); return false; }
  if (!g_initSent) sendInitSequence();
  if (level < 1) level = 1;
  if (level > 6) level = 6;
  writeRegister(g_chFlameLevel, &level, 1);
  logLine("ble", "flame level -> %u", level);
  return true;
}
// Heater on: the remote writes 0x0040 = 01 and then 0x0014 = 02 00 (about 0.7 s later). Only while the flame effect is on.
// Heater off: 0x0040 = 06 (back to "effect on, heater off").
static bool bleSetHeater(bool on) {
  if (!requireBleReady() || !g_chHeater) return false;
  readFireplaceState();
  if (on) {
    if (!flameEffectIsOn()) { setMessage("Heater not started: switch the flame effect on first"); return false; }
    if (heaterIsOn())       { setMessage("Heater is already on"); return true; }
    uint8_t first = 0x01, second[2] = {0x02, 0x00};
    writeRegister(g_chControl, &first, 1);
    delay(650);
    writeRegister(g_chHeater, second, 2);
    logLine("ble", "heater -> ON (0x0040=01, 0x0014=0200)");
    setMessage("Heater switched on");
    readUntil([] { return heaterIsOn(); }, 4000, "heater on"); return true;
  }
  if (!flameEffectIsOn() || !heaterIsOn()) { setMessage("Heater is already off"); return true; }
  uint8_t value = 0x06; writeRegister(g_chControl, &value, 1);
  logLine("ble", "heater -> OFF (0x0040=06)");
  setMessage("Heater switched off");
  readUntil([] { return !heaterIsOn(); }, 4000, "heater off"); return true;
}
// Setpoint: 0x0018 = LE16 degrees C (remote: 20 -> 14 00). Only while the heater is on, clamped to the limits.
static bool bleSetSetpoint(int degrees) {
  if (!requireBleReady() || !g_chSetpoint) return false;
  readFireplaceState();
  if (!heaterIsOn()) { setMessage("Setpoint not changed: the heater is off"); return false; }
  if (degrees < g_setpointMin) degrees = g_setpointMin;
  if (degrees > g_setpointMax) degrees = g_setpointMax;
  uint8_t bytes[2] = {(uint8_t)(degrees & 0xff), (uint8_t)(degrees >> 8)};
  writeRegister(g_chSetpoint, bytes, 2);
  logLine("ble", "setpoint -> %d C", degrees);
  g_awaitedSetpoint = degrees;
  readUntil([] { return g_setpoint == g_awaitedSetpoint; }, 3000, "setpoint");
  setMessage((g_setpoint == degrees) ? "Setpoint set to " + String(degrees) + " C"
                                     : "Setpoint sent (" + String(degrees) + " C) but the fireplace reports " + String(g_setpoint));
  return true;
}
// Heater power: 0x0048 = 01 (low, 0.9 kW) / 03 (high, 1.8 kW). Confirmed with a power meter; the fireplace remembers it.
static bool bleSetHeaterPower(bool high) {
  if (!requireBleReady() || !g_chHeaterPower) return false;
  readFireplaceState();
  if (!heaterIsOn()) { setMessage("Heater power not changed: the heater is off"); return false; }
  uint8_t value = high ? 0x03 : 0x01;
  bool ok = writeRegister(g_chHeaterPower, &value, 1);
  logLine("ble", "heater power -> %s (0x0048=%02x) write=%d", high ? "HIGH" : "LOW", value, ok);
  g_awaitedHeaterPower = value;
  readUntil([] { return g_regHeaterPower == g_awaitedHeaterPower; }, 3000, "heater power");
  if (g_regHeaterPower == value) setMessage(String("Heater power set to ") + (high ? "high" : "low") + " (read back OK)");
  else setMessage(String("Heater power write sent but 0x0048 reads ") + String(g_regHeaterPower, HEX) + (ok ? " (ignored by the fireplace?)" : " (write rejected)"));
  return true;
}

static int g_awaitedVolume = 0;                  // target of the volume command being confirmed (bleTask only)
// Volume: 0x0076 = 0..6. Only models with a sound effect react to it.
static bool bleSetVolume(uint8_t volume) {
  if (!g_bleReady || !g_chVolume) { setMessage("Not connected to the fireplace"); return false; }
  if (volume > 6) volume = 6;
  writeRegister(g_chVolume, &volume, 1);
  logLine("ble", "volume -> %u", volume);
  g_awaitedVolume = volume;
  readUntil([] { return g_volume == g_awaitedVolume; }, 2000, "volume");
  setMessage("Volume " + String(volume));
  return true;
}

// Scan for fireplaces (4 s) and keep the result for the Settings page. A fireplace that already has a connection
// (to this bridge or to a remote) does not advertise and is therefore not in the list.
static void bleScanForFireplaces() {
  g_scanning = true; g_scanCount = 0; publishSnapshot();           // the page shows "Scanning..."
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(4000, false);
  for (int i = 0; i < results.getCount() && g_scanCount < SCAN_MAX; i++) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (String(device->getName().c_str()).indexOf(FIREPLACE_NAME_MATCH) < 0) continue;
    ScanEntry &entry = g_scanEntries[g_scanCount++];
    strlcpy(entry.name, device->getName().c_str(), sizeof(entry.name));
    strlcpy(entry.address, device->getAddress().toString().c_str(), sizeof(entry.address));
    entry.rssi = device->getRSSI();
  }
  scan->clearResults();
  g_scanning = false; g_scanFinishedMs = millis();
  setMessage(g_scanCount ? "Scan finished: " + String(g_scanCount) + " fireplace(s) found" : "Scan finished: no fireplace found");
}
// True for a BLE address like "00:a0:50:04:75:d9".
static bool isValidBleAddress(const char *text) {
  if (strlen(text) != 17) return false;
  for (int i = 0; i < 17; i++) {
    if (i % 3 == 2) { if (text[i] != ':') return false; }
    else if (!isxdigit((unsigned char) text[i])) return false;
  }
  return true;
}
// Apply the settings staged by the web page: check them, save them in flash, forget the remembered
// fireplace and reconnect with the new address and passkey.
static void bleApplyPendingSettings() {
  char address[24], passkey[16]; bool hasAddress, hasPasskey;
  xSemaphoreTake(g_snapshotMutex, portMAX_DELAY);
  strlcpy(address, g_pendingAddress, sizeof(address)); strlcpy(passkey, g_pendingPasskey, sizeof(passkey));
  hasAddress = g_pendingHasAddress; hasPasskey = g_pendingHasPasskey; g_pendingHasAddress = g_pendingHasPasskey = false;
  xSemaphoreGive(g_snapshotMutex);
  for (char *c = address; *c; c++) *c = tolower((unsigned char) *c);
  if (hasAddress && address[0] && !isValidBleAddress(address)) { setMessage("Not saved: the address must look like 00:a0:50:04:75:d9 (or be empty for auto-detect)"); return; }
  if (hasPasskey) {
    bool ok = strlen(passkey) >= 1 && strlen(passkey) <= 6;
    for (char *c = passkey; *c; c++) if (!isdigit((unsigned char) *c)) ok = false;
    if (!ok) { setMessage("Not saved: the passkey must be 1 to 6 digits"); return; }
  }
  if (!hasAddress && !hasPasskey) return;
  if (hasAddress) strlcpy(cfg_fireplaceAddress, address, sizeof(cfg_fireplaceAddress));
  if (hasPasskey) strlcpy(cfg_passkeyText, passkey, sizeof(cfg_passkeyText));
  g_prefs.begin("cfg", false); g_prefs.putString("addr", cfg_fireplaceAddress); g_prefs.putString("pkey", cfg_passkeyText); g_prefs.end();
  parsePasskey();
  rememberDetectedAddress(nullptr);                 // forget the previously detected fireplace
  g_haveTarget = false;                             // choose the fireplace again at the next connect attempt
  bool wasEnabled = g_bleEnabled;
  bleReleaseLink();                                 // drop the current link ...
  if (wasEnabled) bleResumeLink();                  // ... and reconnect with the new settings (unless the link was released on purpose)
  setMessage(String("Fireplace settings saved: ") + (cfg_fireplaceAddress[0] ? cfg_fireplaceAddress : "auto-detect"));
  logLine("cfg", "fireplace=%s passkey=%s", cfg_fireplaceAddress[0] ? cfg_fireplaceAddress : "(auto)", cfg_passkeyText);
}
// Factory reset: drop the BLE link, erase all saved settings (including the Wi-Fi credentials) and restart.
// After the restart there is no Wi-Fi to join, so the 'Dimplex-Setup' portal opens, like after a physical reset.
static void bleFactoryReset() {
  setMessage("Factory reset: erasing settings and restarting...");
  publishSnapshot();
  bleReleaseLink();
  g_prefs.begin("cfg", false); g_prefs.clear(); g_prefs.end();
  g_wifiManager.resetSettings();
  logLine("cfg", "factory reset done, restarting");
  delay(1500);                                      // let the web page show its message
  restartDevice("factory reset");
}

// Short name of a command for the "last step" breadcrumb.
static const char *commandName(CommandType type) {
  switch (type) {
    case CMD_FLAME_EFFECT: return "flame effect";   case CMD_HEATER: return "heater";
    case CMD_HEATER_POWER: return "heater power";   case CMD_SETPOINT: return "setpoint";
    case CMD_SETPOINT_DELTA: return "setpoint +/-"; case CMD_FLAME_LEVEL: return "flame level";
    case CMD_VOLUME: return "volume";               case CMD_BLE_RELEASE: return "bluetooth release";
    case CMD_BLE_RESUME: return "bluetooth resume"; case CMD_SCAN: return "scan";
    case CMD_APPLY_SETTINGS: return "apply settings"; case CMD_FACTORY_RESET: return "factory reset";
  }
  return "?";
}
// Execute one queued command (web or MQTT) and log its result message.
static void executeCommand(const Command &c) {
  char step[STEP_TEXT_LENGTH]; snprintf(step, sizeof(step), "command: %s (%d)", commandName(c.type), c.argument); setBleStep(step);
  switch (c.type) {
    case CMD_FLAME_EFFECT: bleSetFlameEffect(c.argument != 0); break;
    case CMD_HEATER:       bleSetHeater(c.argument != 0); break;
    case CMD_HEATER_POWER: bleSetHeaterPower(c.argument != 0); break;
    case CMD_SETPOINT:     bleSetSetpoint(c.argument); break;
    case CMD_SETPOINT_DELTA:                                   // +/- buttons: relative to the value read now, so quick clicks add up
      readFireplaceState();
      if (heaterIsOn()) bleSetSetpoint(g_setpoint + c.argument); else setMessage("Setpoint not changed: the heater is off");
      break;
    case CMD_FLAME_LEVEL:  if (bleSetFlameLevel((uint8_t) c.argument)) { setMessage("Flame level F" + String(c.argument)); delay(200); readFireplaceState(); } break;
    case CMD_VOLUME:       bleSetVolume((uint8_t) c.argument); break;
    case CMD_SCAN:         bleScanForFireplaces(); break;
    case CMD_APPLY_SETTINGS: bleApplyPendingSettings(); break;
    case CMD_FACTORY_RESET:  bleFactoryReset(); break;
    case CMD_BLE_RELEASE:  bleReleaseLink(); setMessage("Bluetooth released"); break;
    case CMD_BLE_RESUME:   bleResumeLink(); setMessage("Connecting..."); break;
  }
  if (g_messageDirty) logLine(c.source == SOURCE_MQTT ? "mqtt" : "web", "%s", g_message.c_str());
}

// BLE task: connects (and reconnects) to the fireplace, runs the queued commands in order and
// reads the registers every FIREPLACE_POLL_INTERVAL_MS. Updates the shared snapshot after every change.
static void bleTask(void *) {
  uint32_t lastPollMs = 0, lastClockTryMs = 0;
  bool lastReady = false, lastConnected = false, lastEnabled = true;
  for (;;) {
    setBleStep("idle");
    if (g_bleEnabled && !g_bleConnected && millis() - g_lastConnectAttemptMs > BLE_RETRY_INTERVAL_MS) {
      g_lastConnectAttemptMs = millis();
      setBleStep("connecting to the fireplace");
      bleConnectToFireplace();
    }
    if (g_bleConnected && g_blePaired && !g_bleReady) {
      setBleStep("finding the registers");
      bool found = findFireplaceCharacteristics();         // takes seconds; the link can drop meanwhile
      if (found && g_bleConnected && g_blePaired) {
        g_bleReady = true;
        setBleStep("handshake and first read");
        sendInitSequence();
        readFireplaceState();
        lastPollMs = millis();
        g_bleReadyCount = g_bleReadyCount + 1;             // tells the MQTT task to republish everything
        publishSnapshot();
        logLine("ble", "ready");
      }
    }
    if (g_bleReady && !g_clockSet && millis() - lastClockTryMs > 2000) { lastClockTryMs = millis(); writeFireplaceClock(); }  // retry until NTP has a valid time

    Command c;
    if (xQueueReceive(g_commandQueue, &c, pdMS_TO_TICKS(100)) == pdTRUE) {      // also the 100 ms yield of this loop
      g_commandRunning = true;
      executeCommand(c);
      publishSnapshot();
      g_commandsCompleted++;                                  // after the snapshot, so MQTT never sees "done" before the new state
      g_commandRunning = false;
      lastPollMs = millis();
    }
    if (g_bleReady && millis() - lastPollMs > FIREPLACE_POLL_INTERVAL_MS) { lastPollMs = millis(); setBleStep("polling the registers"); readFireplaceState(); publishSnapshot(); }
    if (lastReady != g_bleReady || lastConnected != g_bleConnected || lastEnabled != g_bleEnabled) {   // link state changed (callbacks run in the NimBLE host task)
      lastReady = g_bleReady; lastConnected = g_bleConnected; lastEnabled = g_bleEnabled;
      publishSnapshot();
    }
    g_bleStackFree = uxTaskGetStackHighWaterMark(nullptr);
  }
}

// =====================================================================================================
// 6. MQTT (everything in this section runs in mqttTask)
// =====================================================================================================
// What was last published, so only changes are sent. -1 / empty = unknown (publish at the next chance).
static int g_publishedBleEnabled = -1;       // 0/1
static int g_publishedPower = -1, g_publishedFlameLevel = -1, g_publishedHeater = -1;
static int g_publishedHeaterPower = -1;        // 0 = low, 1 = high, 2 = unknown value
static int g_publishedSetpoint = -1;           // degrees; SETPOINT_NONE = "unknown" was published (heater off)
static int g_publishedTemperatureTenths = -1;  // tenths of a degree (243 = 24.3); SETPOINT_NONE = "unknown" was published
static const int SETPOINT_NONE = -2;           // marker for "published as unknown"
static const int TEMPERATURE_DEADBAND_TENTHS = 2;   // the temperature is only published when it moved by at least 0.2 degrees
static char g_publishedRaw[120] = "";
static bool g_brokerKnown = false;             // the broker address has been resolved
static bool g_diagnosticsDue = true;           // publish the diagnostics at the next chance (after connecting, then every 60 s)
static uint32_t g_lastDiagnosticsMs = 0;

// ---- immediate echo of MQTT commands ----
// A command takes about a second to execute on the fireplace. Home Assistant switches wait for the state topic and
// flip back to the old position when it does not change quickly ("on, off, on" flicker). So when a command arrives we
// publish the expected new state at once, and hold back older values for the controlled entities until bleTask has
// finished that command (or 8 s have passed). If the command failed, the real state is published after the hold.
static uint32_t g_holdUntilCompleted = 0;      // 0 = no hold; otherwise wait until g_commandsCompleted reaches this
static uint32_t g_holdDeadlineMs = 0;
// True while the controlled entities (power, heater, setpoint, Bluetooth link) must not be overwritten by older values.
static bool holdingForCommand() {
  return g_holdUntilCompleted != 0 && g_commandsCompleted.load() < g_holdUntilCompleted && (int32_t)(g_holdDeadlineMs - millis()) > 0;
}
// Tell Home Assistant the setpoint is unknown (shown as "unknown" instead of an old value). Used while the heater is off,
// because the fireplace only has a setpoint while the heater runs.
static void publishSetpointUnknown() {
  if (g_publishedSetpoint == SETPOINT_NONE) return;
  g_mqttClient.publish(TOPIC_SETPOINT_STATE, "None", true); g_publishedSetpoint = SETPOINT_NONE;
}
// Publish the state we expect after a command, and start holding back older values.
static void echoExpectedState(CommandType type, int argument) {
  bool echoed = false;
  const SharedState &s = g_mqttSnapshot;                 // last known real state
  auto publishOnOff = [&](const char *topic, int &published, bool on) { g_mqttClient.publish(topic, on ? "ON" : "OFF", true); published = on; echoed = true; };
  switch (type) {
    case CMD_FLAME_EFFECT:
      publishOnOff(TOPIC_POWER_STATE, g_publishedPower, argument != 0);
      if (!argument) { publishOnOff(TOPIC_HEATER_STATE, g_publishedHeater, false); publishSetpointUnknown(); }       // everything off includes the heater
      break;
    case CMD_HEATER:
      if (!argument) { publishOnOff(TOPIC_HEATER_STATE, g_publishedHeater, false); publishSetpointUnknown(); }
      else if (isFlameEffectOn(s)) publishOnOff(TOPIC_HEATER_STATE, g_publishedHeater, true);   // only works while the effect is on
      break;
    case CMD_SETPOINT:
      if (isHeaterOn(s)) {
        int deg = argument < s.setpointMin ? s.setpointMin : (argument > s.setpointMax ? s.setpointMax : argument);
        char text[12]; snprintf(text, sizeof(text), "%d", deg); g_mqttClient.publish(TOPIC_SETPOINT_STATE, text, true); g_publishedSetpoint = deg; echoed = true;
      }
      break;
    case CMD_BLE_RESUME:  publishOnOff(TOPIC_BLE_STATE, g_publishedBleEnabled, true); break;
    case CMD_BLE_RELEASE: publishOnOff(TOPIC_BLE_STATE, g_publishedBleEnabled, false); break;
    default: break;                                      // flame level, heater power: not an on/off switch
  }
  if (echoed) { g_holdUntilCompleted = g_commandsQueued.load(); g_holdDeadlineMs = millis() + 8000; }
}
// Queue a command that came from MQTT and, when it was accepted, echo the expected state.
static void queueMqttCommand(CommandType type, int argument) {
  if (queueCommand(type, argument, SOURCE_MQTT)) echoExpectedState(type, argument);
}

// Forget what was published, so the next publishChanges() sends everything again.
static void forgetPublishedValues() {
  g_publishedBleEnabled = -1;
  g_publishedPower = g_publishedFlameLevel = g_publishedHeater = g_publishedHeaterPower = g_publishedSetpoint = -1;
  g_publishedTemperatureTenths = -1; g_publishedRaw[0] = 0;
  g_diagnosticsDue = true;
}

// Publish one Home Assistant discovery config: homeassistant/<component>/dimplex/<objectId>/config.
// `body` is the JSON fields of the entity without braces; the shared device block is added here.
static void publishEntity(const char *component, const char *objectId, const String &body) {
  static const char *deviceBlock = "\"dev\":{\"ids\":[\"dimplex_control\"],\"name\":\"Dimplex Control\",\"mf\":\"Dimplex\",\"mdl\":\"Opti-myst\"}";
  String topic = String("homeassistant/") + component + "/dimplex/" + objectId + "/config";
  String payload = "{" + body + "," + deviceBlock + "}";
  g_mqttClient.publish(topic.c_str(), payload.c_str(), true);
}
// Remove an old discovery entity (retained empty payload).
static void removeEntity(const char *component, const char *objectId) {
  String topic = String("homeassistant/") + component + "/dimplex/" + objectId + "/config";
  g_mqttClient.publish(topic.c_str(), "", true);
}
// One diagnostic sensor that reads a key of the JSON on dimplex/diag/state. `extra` = more discovery fields (unit, class, icon).
static void publishDiagnosticSensor(const char *objectId, const char *name, const char *jsonKey, const char *extra) {
  publishEntity("sensor", objectId, String("\"name\":\"") + name + "\",\"uniq_id\":\"dimplex_" + objectId + "\",\"stat_t\":\"" + TOPIC_DIAG_STATE +
                "\",\"val_tpl\":\"{{ value_json." + jsonKey + " }}\",\"ent_cat\":\"diagnostic\"," + extra +
                ",\"avty_t\":\"" + TOPIC_AVAILABILITY + "\"");
}
// The diagnostic sensors: why the bridge restarted, how long it has been running, memory, signal strength and counters.
static void publishDiagnosticDiscovery() {
  publishDiagnosticSensor("diag_restart_reason", "Last restart reason", "restart_reason", "\"ic\":\"mdi:restart-alert\"");
  publishDiagnosticSensor("diag_restart_count", "Restart count", "restart_count", "\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:counter\"");
  publishDiagnosticSensor("diag_uptime", "Uptime", "uptime", "\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"stat_cla\":\"measurement\"");
  publishDiagnosticSensor("diag_free_heap", "Free memory", "free_heap", "\"unit_of_meas\":\"B\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:memory\"");
  publishDiagnosticSensor("diag_min_heap", "Lowest free memory", "min_heap", "\"unit_of_meas\":\"B\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:memory\"");
  publishDiagnosticSensor("diag_wifi", "Wi-Fi signal", "wifi_rssi", "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\"");
  publishDiagnosticSensor("diag_ble_connects", "Bluetooth connections", "ble_connects", "\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:bluetooth-connect\"");
  publishDiagnosticSensor("diag_mqtt_failures", "MQTT connect failures", "mqtt_failures", "\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:lan-disconnect\"");
  publishDiagnosticSensor("diag_stack_ble", "Free stack BLE task", "stack_ble", "\"unit_of_meas\":\"B\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:memory\"");
  publishDiagnosticSensor("diag_stack_mqtt", "Free stack MQTT task", "stack_mqtt", "\"unit_of_meas\":\"B\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:memory\"");
}
// Publish all Home Assistant entities and remove the ones that no longer exist (water sensor, volume,
// and the old read-only heater and setpoint sensors that are now switch / number).
static void publishHomeAssistantDiscovery() {
  setMqttStep("publishing discovery");
  String avail = String(",\"avty_t\":\"") + TOPIC_AVAILABILITY + "\"";
  removeEntity("binary_sensor", "water");
  removeEntity("number", "volume");
  removeEntity("binary_sensor", "heater");     // replaced by the heater switch
  removeEntity("sensor", "setpoint");          // replaced by the setpoint number
  // ---- controls ----
  publishEntity("switch", "power", String("\"name\":\"Fireplace\",\"uniq_id\":\"dimplex_power\",\"cmd_t\":\"") + TOPIC_POWER_SET +
                "\",\"stat_t\":\"" + TOPIC_POWER_STATE + "\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"ic\":\"mdi:fireplace\"" + avail);
  publishEntity("number", "level", String("\"name\":\"Flame intensity\",\"uniq_id\":\"dimplex_level\",\"cmd_t\":\"") + TOPIC_FLAME_LEVEL_SET +
                "\",\"stat_t\":\"" + TOPIC_FLAME_LEVEL_STATE + "\",\"min\":1,\"max\":6,\"step\":1,\"mode\":\"slider\",\"ic\":\"mdi:fire\"" + avail);
  publishEntity("switch", "heater_switch", String("\"name\":\"Heater\",\"uniq_id\":\"dimplex_heater_switch\",\"cmd_t\":\"") + TOPIC_HEATER_SET +
                "\",\"stat_t\":\"" + TOPIC_HEATER_STATE + "\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"ic\":\"mdi:radiator\"" + avail);
  publishEntity("number", "setpoint_number", String("\"name\":\"Setpoint\",\"uniq_id\":\"dimplex_setpoint_number\",\"cmd_t\":\"") + TOPIC_SETPOINT_SET +
                "\",\"stat_t\":\"" + TOPIC_SETPOINT_STATE + "\",\"min\":15,\"max\":30,\"step\":1,\"mode\":\"box\",\"unit_of_meas\":\"°C\","
                "\"dev_cla\":\"temperature\",\"ic\":\"mdi:thermometer-check\"" + avail);
  publishEntity("switch", "ble", String("\"name\":\"Bluetooth link\",\"uniq_id\":\"dimplex_ble_link\",\"cmd_t\":\"") + TOPIC_BLE_SET +
                "\",\"stat_t\":\"" + TOPIC_BLE_STATE + "\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"ent_cat\":\"config\",\"ic\":\"mdi:bluetooth\"" + avail);
  // ---- read-only ----
  publishEntity("sensor", "level", String("\"name\":\"Flame intensity state\",\"uniq_id\":\"dimplex_levelstate\",\"stat_t\":\"") + TOPIC_FLAME_LEVEL_STATE +
                "\",\"ic\":\"mdi:fire\"" + avail);
  publishEntity("sensor", "heater_power", String("\"name\":\"Heater power\",\"uniq_id\":\"dimplex_heater_power\",\"stat_t\":\"") + TOPIC_HEATER_POWER_STATE +
                "\",\"ic\":\"mdi:flash\"" + avail);
  publishEntity("sensor", "temperature", String("\"name\":\"Internal temperature\",\"uniq_id\":\"dimplex_temperature\",\"stat_t\":\"") + TOPIC_TEMPERATURE_STATE +
                "\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\"" + avail);
  publishEntity("sensor", "raw", String("\"name\":\"Raw registers\",\"uniq_id\":\"dimplex_raw\",\"stat_t\":\"") + TOPIC_RAW_STATE +
                "\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:code-braces\"" + avail);
  publishDiagnosticDiscovery();
  logLine("mqtt", "discovery published");
}

// Incoming MQTT message: turn it into a queued command and echo the expected state (see echoExpectedState). No BLE in here.
static void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  char text[16]; unsigned n = length < sizeof(text) - 1 ? length : sizeof(text) - 1; memcpy(text, payload, n); text[n] = 0;
  bool isOn = strcmp(text, "ON") == 0;
  if (!strcmp(topic, TOPIC_POWER_SET))           queueMqttCommand(CMD_FLAME_EFFECT, isOn ? 1 : 0);                  // flame effect on/off
  else if (!strcmp(topic, TOPIC_BLE_SET))        queueMqttCommand(isOn ? CMD_BLE_RESUME : CMD_BLE_RELEASE, 0);         // Bluetooth link on/off
  else if (!strcmp(topic, TOPIC_HEATER_SET))     queueMqttCommand(CMD_HEATER, isOn ? 1 : 0);                        // heater on/off
  else if (!strcmp(topic, TOPIC_SETPOINT_SET))   queueMqttCommand(CMD_SETPOINT, (int)(atof(text) + 0.5));          // the BLE side clamps to the limits
  else if (!strcmp(topic, TOPIC_FLAME_LEVEL_SET)) {                                                                // flame intensity 1..6
    int level = atoi(text); if (level < 1) level = 1; if (level > 6) level = 6;
    queueMqttCommand(CMD_FLAME_LEVEL, level);
  }
}

// Resolve the configured broker and point PubSubClient at it: numeric IP, *.local (mDNS) or plain DNS name.
// The mDNS lookup can block for 2 s; only mqttTask waits.
static void resolveMqttBroker() {
  IPAddress ip;
  String host = cfg_mqttHost;
  if (ip.fromString(host)) { g_mqttClient.setServer(ip, (uint16_t) atoi(cfg_mqttPort)); g_brokerKnown = true; return; }
  if (host.endsWith(".local")) {
    IPAddress resolved = MDNS.queryHost(host.substring(0, host.length() - 6), 2000);
    if ((uint32_t) resolved != 0) {
      logLine("mdns", "%s -> %s", cfg_mqttHost, resolved.toString().c_str());
      g_mqttClient.setServer(resolved, (uint16_t) atoi(cfg_mqttPort)); g_brokerKnown = true;
    } else {
      logLine("mdns", "could not resolve %s (retrying)", cfg_mqttHost);
      g_brokerKnown = false;
    }
    return;
  }
  g_mqttClient.setServer(cfg_mqttHost, (uint16_t) atoi(cfg_mqttPort)); g_brokerKnown = true;
}
// One connect attempt, at most every `wait` ms; `wait` doubles after each failure (3 s ... 30 s).
// On success: announce online, subscribe to the command topics and publish discovery. After 3 failures
// the broker address is resolved again. Returns true when connected.
static bool tryMqttConnect() {
  static uint32_t lastAttemptMs = 0, waitMs = 3000;
  static uint8_t failures = 0;
  if (millis() - lastAttemptMs < waitMs) return false;
  lastAttemptMs = millis();
  if (g_mqttClient.connect("dimplex-control", cfg_mqttUser, cfg_mqttPassword, TOPIC_AVAILABILITY, 0, true, "offline")) {
    logLine("mqtt", "connected");
    g_mqttClient.publish(TOPIC_AVAILABILITY, "online", true);
    g_mqttClient.subscribe(TOPIC_POWER_SET); g_mqttClient.subscribe(TOPIC_FLAME_LEVEL_SET);
    g_mqttClient.subscribe(TOPIC_HEATER_SET); g_mqttClient.subscribe(TOPIC_SETPOINT_SET); g_mqttClient.subscribe(TOPIC_BLE_SET);
    publishHomeAssistantDiscovery();
    forgetPublishedValues();                       // republish everything after (re)connecting
    waitMs = 3000; failures = 0;
    return true;
  }
  g_mqttFailCount = g_mqttFailCount + 1;
  logLine("mqtt", "connect failed rc=%d (next try in %lu s)", g_mqttClient.state(), (unsigned long)(waitMs * 2 > 30000 ? 30 : waitMs * 2 / 1000));
  waitMs = waitMs * 2 > 30000 ? 30000 : waitMs * 2;
  if (++failures >= 3) { failures = 0; g_brokerKnown = false; }   // the broker IP may have changed
  return false;
}
// Publish every value that differs from what was published last.
static void publishChanges(const SharedState &s) {
  bool hold = holdingForCommand();                          // older values of the controlled entities are skipped while true
  if (!hold && (int) s.bleEnabled != g_publishedBleEnabled) {       // known even while the link is released
    g_mqttClient.publish(TOPIC_BLE_STATE, s.bleEnabled ? "ON" : "OFF", true); g_publishedBleEnabled = s.bleEnabled;
    logLine("pub", "ble link enabled=%d", s.bleEnabled);
  }
  if (!s.valid) {                                           // no reading yet (e.g. after a restart): do not show old retained values
    publishSetpointUnknown();
    if (g_publishedTemperatureTenths != SETPOINT_NONE) { g_mqttClient.publish(TOPIC_TEMPERATURE_STATE, "None", true); g_publishedTemperatureTenths = SETPOINT_NONE; }
    return;
  }
  bool effectOn = (s.regControl[0] != 0);
  if (!hold && (int) effectOn != g_publishedPower) { g_mqttClient.publish(TOPIC_POWER_STATE, effectOn ? "ON" : "OFF", true); g_publishedPower = effectOn; logLine("pub", "power=%d", effectOn); }
  if (s.flameLevel >= 1 && s.flameLevel != g_publishedFlameLevel) {          // 0x0042 = flame intensity (independent of power)
    char text[4]; snprintf(text, sizeof(text), "%u", s.flameLevel); g_mqttClient.publish(TOPIC_FLAME_LEVEL_STATE, text, true); g_publishedFlameLevel = s.flameLevel;
    logLine("pub", "level=%u", s.flameLevel);
  }
  bool heaterOn = isHeaterOn(s);
  if (!hold && (int) heaterOn != g_publishedHeater) { g_mqttClient.publish(TOPIC_HEATER_STATE, heaterOn ? "ON" : "OFF", true); g_publishedHeater = heaterOn; logLine("pub", "heater=%d", heaterOn); }
  int powerCode = (s.regHeaterPower == 0x01) ? 0 : (s.regHeaterPower == 0x03 ? 1 : 2);
  if (powerCode != g_publishedHeaterPower) {
    char text[12]; if (powerCode == 2) snprintf(text, sizeof(text), "raw %02x", s.regHeaterPower); else snprintf(text, sizeof(text), "%s", powerCode ? "high" : "low");
    g_mqttClient.publish(TOPIC_HEATER_POWER_STATE, text, true); g_publishedHeaterPower = powerCode;
  }
  if (!hold) {
    if (heaterOn && s.setpoint > 0) {
      if (s.setpoint != g_publishedSetpoint) { char text[12]; snprintf(text, sizeof(text), "%d", s.setpoint); g_mqttClient.publish(TOPIC_SETPOINT_STATE, text, true); g_publishedSetpoint = s.setpoint; }
    } else if (!heaterOn) {
      publishSetpointUnknown();                              // heater off: no setpoint
    }
  }
  int temperatureTenths = s.regTemperature[1] * 10 + s.regTemperature[2] % 10;
  int temperatureChange = temperatureTenths - g_publishedTemperatureTenths;
  if (g_publishedTemperatureTenths < 0 || temperatureChange >= TEMPERATURE_DEADBAND_TENTHS || -temperatureChange >= TEMPERATURE_DEADBAND_TENTHS) {
    char temperature[10]; snprintf(temperature, sizeof(temperature), "%d.%d", temperatureTenths / 10, temperatureTenths % 10);
    g_mqttClient.publish(TOPIC_TEMPERATURE_STATE, temperature, true); g_publishedTemperatureTenths = temperatureTenths;
  }
  char hexControl[6], hexHeater[6], hexSetpoint[6], hexPower[4], hexTemperature[8], hex3c[8], hex63[6], raw[120];
  uint8_t setpointBytes[2] = {(uint8_t)(s.setpoint & 0xff), (uint8_t)(s.setpoint >> 8)};
  bytesToHex(hexControl, s.regControl, 2); bytesToHex(hexHeater, s.regHeater, 2); bytesToHex(hexSetpoint, setpointBytes, 2);
  bytesToHex(hexPower, &s.regHeaterPower, 1); bytesToHex(hexTemperature, s.regTemperature, 3); bytesToHex(hex3c, s.regUnknown3c, 3); bytesToHex(hex63, s.regUnknown63, 2);
  snprintf(raw, sizeof(raw), "40=%s 14=%s 18=%s 48=%s 55=%s 3c=%s 63=%s", hexControl, hexHeater, hexSetpoint, hexPower, hexTemperature, hex3c, hex63);
  if (strcmp(raw, g_publishedRaw) != 0) { g_mqttClient.publish(TOPIC_RAW_STATE, raw, true); strlcpy(g_publishedRaw, raw, sizeof(g_publishedRaw)); }
}

// Publish the health values as one JSON message (retained). Called after connecting and then every 60 s.
static void publishDiagnostics() {
  char json[420];
  snprintf(json, sizeof(json),
           "{\"restart_reason\":\"%s\",\"restart_count\":%lu,\"uptime\":%lu,\"free_heap\":%lu,\"min_heap\":%lu,\"wifi_rssi\":%d,"
           "\"ble_connects\":%lu,\"mqtt_failures\":%lu,\"stack_ble\":%lu,\"stack_mqtt\":%lu}",
           g_restartReason, (unsigned long) g_bootCount, (unsigned long) (esp_timer_get_time() / 1000000ULL), (unsigned long) ESP.getFreeHeap(),
           (unsigned long) ESP.getMinFreeHeap(), (int) WiFi.RSSI(), (unsigned long) g_bleReadyCount, (unsigned long) g_mqttFailCount,
           (unsigned long) g_bleStackFree, (unsigned long) g_mqttStackFree);
  g_mqttClient.publish(TOPIC_DIAG_STATE, json, true);
  g_diagnosticsDue = false; g_lastDiagnosticsMs = millis();
}

// MQTT task: keeps the broker connection (with backoff), handles incoming commands and publishes
// state changes from the snapshot. Waits only in its own calls, never blocks the web page or BLE.
static void mqttTask(void *) {
  uint32_t lastVersion = 0xFFFFFFFF, lastBleReadyCount = 0, lastResolveMs = 0;
  g_mqttClient.setCallback(onMqttMessage);
  g_mqttClient.setBufferSize(768);
  g_mqttClient.setSocketTimeout(3);                // seconds; a silent broker must not hold this task for 15 s
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { g_mqttUp = false; vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if (!g_brokerKnown) {
      if (lastResolveMs == 0 || millis() - lastResolveMs > 4000) { lastResolveMs = millis(); setMqttStep("resolving the broker"); resolveMqttBroker(); }
    } else if (!g_mqttClient.connected()) {
      g_mqttUp = false;
      setMqttStep("connecting to the broker");
      if (tryMqttConnect()) { lastVersion = 0xFFFFFFFF; lastBleReadyCount = g_bleReadyCount; }   // force a full publish
    } else {
      g_mqttUp = true;
      setMqttStep("client loop");
      g_mqttClient.loop();
      if (g_holdUntilCompleted != 0 && !holdingForCommand()) { g_holdUntilCompleted = 0; lastVersion = 0xFFFFFFFF; }   // hold over: publish the real state
      if (lastBleReadyCount != g_bleReadyCount) { lastBleReadyCount = g_bleReadyCount; forgetPublishedValues(); lastVersion = 0xFFFFFFFF; }   // BLE link (re)established
      if (lastVersion != g_sharedVersion) { lastVersion = g_sharedVersion; copySnapshot(g_mqttSnapshot); setMqttStep("publishing changes"); publishChanges(g_mqttSnapshot); }
      if (g_diagnosticsDue || millis() - g_lastDiagnosticsMs > 60000) { setMqttStep("publishing diagnostics"); publishDiagnostics(); }
    }
    g_mqttStackFree = uxTaskGetStackHighWaterMark(nullptr);
    vTaskDelay(pdMS_TO_TICKS(10));                 // feeds the watchdog and leaves Wi-Fi/BLE CPU time on core 0
  }
}

// =====================================================================================================
// 7. WEB PAGES (run in loop(), core 1; never call BLE or MQTT)
// =====================================================================================================
// Three pages, reachable through the menu at the top:
//   /          Main      Bluetooth link, flame effect, heater, setpoint (+ the internal temperature)
//   /settings  Settings  flame level, heater power, volume, fireplace selection, factory reset
//   /debug     Debug     raw registers, temperature table, status log download, system information
// Each page is a fixed frame around one "live" block. The page script fetches /live?p=<page> every few seconds and
// replaces that block, so values follow the fireplace without pressing a button. Forms are placed outside the
// live block, so a refresh never clears what you are typing.
static const char *PAGE_MAIN = "main", *PAGE_SETTINGS = "settings", *PAGE_DEBUG = "debug";
static const char *g_renderPage = PAGE_MAIN;   // page being rendered: buttons return to the same page after a press
static String g_webMessage = "";               // message generated by the web layer itself
static uint32_t g_webMessageTimeMs = 0;

// Address of a /set action; adds "&r=<page>" so the redirect after the action returns to the page you were on.
static String setHref(const String &query) {
  String href = "/set?" + query;
  if (strcmp(g_renderPage, PAGE_MAIN) != 0) href += String("&r=") + g_renderPage;
  return href;
}
// ---- feedback for pressed buttons ----
// After a button press the requested choice is shown in red ("wait") until the fireplace really reports it; only then it turns
// orange ("on", the current state). Only the web layer (loop(), core 1) touches this table.
static AwaitedValue g_awaited[AW_COUNT];
static const uint32_t AWAIT_SETTLE_MS = 2000;     // after all commands finished, wait this long for the state to show up
static const uint32_t AWAIT_GIVE_UP_MS = 15000;   // never show "waiting" longer than this

// Remember that the user asked for `target` on `control` (choices: on = 1 / off = 0, level number, degrees...).
static void awaitControl(AwaitedControl control, int target) { g_awaited[control] = { true, target, millis() }; }
// True when the fireplace state in `s` already equals the requested `target` of `control`.
static bool awaitedTargetReached(AwaitedControl control, int target, const SharedState &s) {
  switch (control) {
    case AW_BLE:          return target ? (s.bleEnabled && s.ready) : !s.bleEnabled;
    case AW_EFFECT:       return (isFlameEffectOn(s) ? 1 : 0) == target;
    case AW_HEATER:       return (isHeaterOn(s) ? 1 : 0) == target;
    case AW_HEATER_POWER: return s.regHeaterPower == (target ? 0x03 : 0x01);
    case AW_SETPOINT:     return s.setpoint == target;
    case AW_FLAME_LEVEL:  return s.flameLevel == target;
    case AW_VOLUME:       return s.volume == target;
    default:              return true;
  }
}
// Clear the entries that are confirmed, failed (nothing pending any more) or too old. Called before each live block is built.
static void updateAwaited(const SharedState &s, uint32_t pending) {
  for (int i = 0; i < AW_COUNT; i++) {
    AwaitedValue &a = g_awaited[i];
    if (!a.active) continue;
    uint32_t age = millis() - a.sinceMs;
    if (awaitedTargetReached((AwaitedControl) i, a.target, s) || age > AWAIT_GIVE_UP_MS || (pending == 0 && age > AWAIT_SETTLE_MS)) a.active = false;
  }
}
static bool anyAwaited() { for (int i = 0; i < AW_COUNT; i++) if (g_awaited[i].active) return true; return false; }

// A choice button. Colours: green = current state "on" (or any selected value), red = current state "off",
// orange = this choice was requested but the fireplace has not confirmed it yet, grey = not selected.
// While a request for this control is open, the old state is not coloured, so only one button has a colour.
static String choiceButton(const String &label, const String &href, bool isCurrent, AwaitedControl control, int choice) {
  const AwaitedValue &a = g_awaited[control];
  bool isOffChoice = (choice == 0) && (control == AW_BLE || control == AW_EFFECT || control == AW_HEATER);
  const char *style = a.active ? (a.target == choice ? " wait" : "") : (isCurrent ? (isOffChoice ? " off" : " on") : "");
  return "<a class='b" + String(style) + "' href='" + href + "'>" + label + "</a> ";
}
// An <a> styled as a button; `highlighted` = the current choice.
static String linkButton(const String &label, const String &href, bool highlighted) {
  return "<a class='b" + String(highlighted ? " on" : "") + "' href='" + href + "'>" + label + "</a> ";
}
// A row of numbered buttons from `first` to `last` that all send /set?<parameter>=<number>.
static String linkButtonRow(const char *parameter, AwaitedControl control, int current, int first, int last, const char *labelPrefix = "") {
  String html;
  for (int i = first; i <= last; i++) html += choiceButton(String(labelPrefix) + String(i), setHref(String(parameter) + "=" + i), i == current, control, i);
  return html;
}
// Escape text for use inside HTML ("<Dimplex>" in a device name would otherwise vanish).
static String htmlEscape(const char *text) {
  String out;
  for (const char *p = text; *p; p++) {
    if (*p == '<') out += "&lt;"; else if (*p == '>') out += "&gt;"; else if (*p == '&') out += "&amp;"; else if (*p == '\'') out += "&#39;"; else out += *p;
  }
  return out;
}
// Seconds -> "h:mm:ss".
static String formatUptime(uint32_t seconds) {
  char text[16]; snprintf(text, sizeof(text), "%lu:%02lu:%02lu", (unsigned long)(seconds / 3600), (unsigned long)((seconds / 60) % 60), (unsigned long)(seconds % 60));
  return text;
}
// Short description of a history row for the page table, e.g. "heater high 21&deg;C F3".
static String describeSample(const StatusSample &x) {
  if (x.control0 == 0) return "off";
  bool heating = (x.heater0 & 0x04) == 0;
  if (!heating) return "effect F" + String(x.flameLevel);
  return String("heater ") + (x.heaterPower == 0x03 ? "high" : (x.heaterPower == 0x01 ? "low" : "?")) + " " + String(x.setpoint) + "&deg;C F" + String(x.flameLevel);
}
// Number of commands that are queued or running.
static uint32_t pendingCommands() { return uxQueueMessagesWaiting(g_commandQueue) + (g_commandRunning ? 1 : 0); }
static String formatMs(uint32_t us) { return String(us / 1000.0, 1) + " ms"; }
// Show a message from the web layer on the next page refresh(es).
static void setWebMessage(const String &text) { g_webMessage = text; g_webMessageTimeMs = millis(); }

// Top of every live block: a hidden marker that makes the script refresh faster while something is going on,
// the result messages of the last actions, and the "working" line.
static String buildLiveHeader(const SharedState &s, uint32_t pending) {
  String h;
  if (pending || anyAwaited() || s.scanning || (s.bleEnabled && !s.ready)) h += "<span data-busy hidden></span>";
  if (g_webMessage.length() && millis() - g_webMessageTimeMs < MESSAGE_SHOW_TIME_MS) h += "<p class=msg>" + g_webMessage + "</p>";
  if (s.message[0] && millis() - s.messageTimeMs < MESSAGE_SHOW_TIME_MS) h += "<p class=msg>" + htmlEscape(s.message) + "</p>";
  if (pending) h += "<p class=dim>Working&hellip; (" + String(pending) + " command" + (pending > 1 ? "s" : "") + " in progress)</p>";
  return h;
}

// Live block of the Main page: Bluetooth link, flame effect, heater, setpoint and the internal temperature.
static String buildMainLive(const SharedState &s) {
  bool ready = s.ready && s.valid;
  bool effectOn = ready && isFlameEffectOn(s), heaterOn = ready && isHeaterOn(s);
  String h;
  h += "<h3>Bluetooth</h3>" + choiceButton("Connected", setHref("ble=on"), s.bleEnabled, AW_BLE, 1) + choiceButton("Released (use remote)", setHref("ble=off"), !s.bleEnabled, AW_BLE, 0);
  h += "<p class=dim>";
  if (!s.bleEnabled)  h += "Released: the original remote can connect. Values below are the last known.";
  else if (s.ready)   h += "Connected and paired to the fireplace.";
  else                h += "Connecting to the fireplace&hellip;";
  h += "</p>";

  h += "<h3>Flame effect</h3>" + choiceButton("On", setHref("fx=on"), effectOn, AW_EFFECT, 1) + choiceButton("Off (all off)", setHref("fx=off"), ready && !effectOn, AW_EFFECT, 0);

  h += "<h3>Heater</h3>" + choiceButton("On", setHref("heater=on"), heaterOn, AW_HEATER, 1) + choiceButton("Off", setHref("heater=off"), ready && effectOn && !heaterOn, AW_HEATER, 0);
  h += "<p class=dim>The heater can only be started while the flame effect is on.</p>";

  h += "<h3>Setpoint</h3>";
  if (heaterOn) {
    h += linkButton("&minus;1", setHref("spd=-1"), false) + "<b class='big" + String(g_awaited[AW_SETPOINT].active ? " pending" : "") + "'>" + String(g_awaited[AW_SETPOINT].active ? g_awaited[AW_SETPOINT].target : s.setpoint) + "&deg;C</b> " + linkButton("+1", setHref("spd=1"), false);
    h += "<form action='/set' style='margin-top:6px'>Set: <input type=number name=sp min=" + String(s.setpointMin) + " max=" + String(s.setpointMax) +
         " value=" + String(s.setpoint) + "> <button class=b>Apply</button> <span class=dim>(" + String(s.setpointMin) + "&ndash;" + String(s.setpointMax) + ")</span></form>";
  } else h += "<p class=dim>Only available while the heater is on (the fireplace starts at 21&deg;C).</p>";

  h += "<h3>Temperature <span class=dim>(sensor inside the fireplace, not the room)</span></h3>";
  if (ready) h += "<span class=big>" + String(s.regTemperature[1]) + "." + String(s.regTemperature[2] % 10) + "&deg;C</span> <span class=dim>read " + String((millis() - s.lastReadMs) / 1000) + " s ago</span>";
  else h += "<p class=dim>No data yet.</p>";
  return h;
}

// Live block of the Settings page: flame level, heater power, volume and the fireplace selection (with the scan result).
static String buildSettingsLive(const SharedState &s) {
  bool ready = s.ready && s.valid;
  int level = s.flameLevel < 1 ? 1 : s.flameLevel;
  String h;
  h += "<h3>Flame level</h3>" + linkButtonRow("flame", AW_FLAME_LEVEL, ready ? level : -1, 1, 6, "F");

  h += "<h3>Heater power</h3>";
  h += choiceButton("Low (0.9 kW)", setHref("hpwr=low"), ready && s.regHeaterPower == 0x01, AW_HEATER_POWER, 0) + choiceButton("High (1.8 kW)", setHref("hpwr=high"), ready && s.regHeaterPower == 0x03, AW_HEATER_POWER, 1);
  h += "<p class=dim>Can only be changed while the heater is on. The fireplace remembers the setting.</p>";
  if (ready && s.regHeaterPower != 0x01 && s.regHeaterPower != 0x03) h += "<p class=dim>Register 0x0048 = " + String(s.regHeaterPower, HEX) + "</p>";

  h += "<h3>Volume</h3>" + linkButtonRow("vol", AW_VOLUME, ready ? s.volume : -1, 0, 6);
  h += "<p class=dim>Only for models with a sound effect; has no effect on fireplaces without sound.</p>";

  h += "<h3>Fireplace</h3><p class=dim>Selected: <b>" + String(s.configuredAddress[0] ? s.configuredAddress : "auto-detect") + "</b> &middot; passkey " + htmlEscape(s.passkeyText);
  h += "<br>Connected to: " + String(s.ready ? s.targetAddress : "(not connected)") + "</p>";
  h += linkButton("&#128269; Scan for fireplaces", setHref("scan=1"), false);
  if (s.scanning) h += "<p class=dim>Scanning&hellip; (4 s)</p>";
  else if (s.scanFinishedMs) {
    h += "<p class=dim>Scan result (" + String((millis() - s.scanFinishedMs) / 1000) + " s ago). A fireplace that is connected to this bridge or to a remote does not advertise and is not listed.</p>";
    if (s.scanCount == 0) h += "<p class=dim>No fireplace found.</p>";
    for (int i = 0; i < s.scanCount; i++) {
      h += "<p>" + htmlEscape(s.scan[i].name) + " &mdash; " + s.scan[i].address + " <span class=dim>(" + String(s.scan[i].rssi) + " dBm)</span> " +
           linkButton("Use", setHref(String("fpaddr=") + s.scan[i].address), strcmp(s.scan[i].address, s.configuredAddress) == 0) + "</p>";
    }
  }
  h += "<h3>More settings</h3><p class=dim>Weekly program and temperature limits are not available yet.</p>";
  return h;
}

// Live block of the Debug page: raw registers, connection and system information, temperature table, log download.
static String buildDebugLive(const SharedState &s, uint32_t pending) {
  bool ready = s.ready && s.valid;
  String h;
  h += "<h3>Status log</h3><p><a class=b href='/csv?download=1'>&#11015; Download CSV</a> <a class=b href='/csv'>View in browser</a></p>";
  h += "<p class=dim>One row per change, or at least once a minute; the last " + String(HISTORY_SIZE) + " rows are kept in memory (lost at restart).</p>";

  h += "<h3>Raw registers</h3>";
  if (ready) {
    char control[6], heater[6], temperature[8], unknown3c[8], unknown63[6];
    bytesToHex(control, s.regControl, 2); bytesToHex(heater, s.regHeater, 2); bytesToHex(temperature, s.regTemperature, 3);
    bytesToHex(unknown3c, s.regUnknown3c, 3); bytesToHex(unknown63, s.regUnknown63, 2);
    h += "<table><tr><th>handle</th><th>value</th><th>meaning</th></tr>";
    h += String("<tr><td>0x0040</td><td>") + control + "</td><td>main status (effect / heater)</td></tr>";
    h += String("<tr><td>0x0014</td><td>") + heater + "</td><td>heater status</td></tr>";
    h += "<tr><td>0x0018</td><td>" + String(s.setpoint) + "</td><td>setpoint &deg;C</td></tr>";
    h += "<tr><td>0x0048</td><td>" + String(s.regHeaterPower, HEX) + "</td><td>heater power</td></tr>";
    h += "<tr><td>0x0042</td><td>" + String(s.flameLevel) + "</td><td>flame level</td></tr>";
    h += "<tr><td>0x0076</td><td>" + String(s.volume) + "</td><td>volume</td></tr>";
    h += String("<tr><td>0x0055</td><td>") + temperature + "</td><td>internal temperature</td></tr>";
    h += String("<tr><td>0x003c</td><td>") + unknown3c + "</td><td>unknown measurement</td></tr>";
    h += String("<tr><td>0x0063</td><td>") + unknown63 + "</td><td>effect intensity</td></tr></table>";
    h += "<p class=dim>Read " + String((millis() - s.lastReadMs) / 1000) + " s ago &middot; limits " + String(s.setpointMin) + "&ndash;" + String(s.setpointMax) + " &deg;C</p>";
  } else h += "<p class=dim>No data (not connected).</p>";

  h += "<h3>Temperature history <span class=dim>(internal sensor, newest first)</span></h3>";
  if (ready && s.historyCount > 0) {
    h += "<p class=dim>min " + String(s.tempMin, 1) + " / max " + String(s.tempMax, 1) + " since boot &middot; last change " + String((millis() - s.lastTempChangeMs) / 1000) + " s ago</p>";
    h += "<table><tr><th>uptime</th><th>&deg;C</th><th>state</th></tr>";
    for (int i = 0; i < s.historyCount && i < 20; i++) {
      const StatusSample &x = s.history[(s.historyHead - 1 - i + 2 * HISTORY_SIZE) % HISTORY_SIZE];
      h += "<tr><td>" + formatUptime(x.uptimeSec) + "</td><td>" + x.temperatureText + "</td><td>" + describeSample(x) + "</td></tr>";
    }
    h += "</table>";
  } else h += "<p class=dim>No data yet.</p>";

  h += "<h3>Connections</h3><p class=dim>Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected") +
       " &middot; MQTT: " + (g_mqttUp ? "connected" : "<b>not connected</b>") + " (" + String((uint32_t) g_mqttFailCount) + " failed attempts)" +
       " &middot; Bluetooth: " + (s.ready ? "ready" : (s.connected ? "connected, pairing" : (s.bleEnabled ? "connecting" : "released"))) + "</p>";
  h += "<h3>System</h3><p class=dim>Commands waiting: " + String(pending);
  if (g_commandsDropped) h += " &middot; dropped (queue full): " + String((uint32_t) g_commandsDropped);
  h += "<br>Longest gap between two loop() passes: " + formatMs(g_loopGapMaxUs) + " &middot; longest web request: " + formatMs(g_webRequestMaxUs) +
       " <a href='/set?diag=reset&r=debug'>reset</a>";
  h += "<br>Free stack (bytes) ble/mqtt: " + String((uint32_t) g_bleStackFree) + " / " + String((uint32_t) g_mqttStackFree) +
       " &middot; free heap: " + String((uint32_t) ESP.getFreeHeap()) + " (lowest " + String((uint32_t) ESP.getMinFreeHeap()) + ")";
  h += "<br>Uptime: " + formatUptime(millis() / 1000) + " &middot; <a href='/set?restart=1'>restart device</a></p>";
  h += "<h3>LED</h3><p class=dim>Fast blink = setup portal open &middot; 1 flash = no Wi-Fi &middot; 2 flashes = no MQTT &middot; 3 flashes = no fireplace &middot; slow blink = Bluetooth released &middot; steady = all connected</p>";
  return h;
}

// Build the live block of one page (also used inside the full page). Takes a fresh snapshot first.
static String buildLiveHtml(const char *page) {
  g_renderPage = page;
  copySnapshot(g_webSnapshot);
  const SharedState &s = g_webSnapshot;
  uint32_t pending = pendingCommands();
  updateAwaited(s, pending);
  String h = buildLiveHeader(s, pending);
  if (page == PAGE_SETTINGS)   h += buildSettingsLive(s);
  else if (page == PAGE_DEBUG) h += buildDebugLive(s, pending);
  else                         h += buildMainLive(s);
  return h;
}
// Page name from the "p" argument of /live.
static const char *pageFromName(const String &name) {
  if (name == "settings") return PAGE_SETTINGS;
  if (name == "debug") return PAGE_DEBUG;
  return PAGE_MAIN;
}

// Send a complete page: menu, live block, `afterLive` (static content such as forms) and the refresh script.
static void sendPage(const char *page, const String &afterLive) {
  char step[STEP_TEXT_LENGTH]; snprintf(step, sizeof(step), "page: %s", page); setWebStep(step);
  String live = buildLiveHtml(page);
  auto navLink = [&](const char *target, const char *href, const char *label) { return String("<a class='nav") + (page == target ? " on" : "") + "' href='" + href + "'>" + label + "</a>"; };
  String h = "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<noscript><meta http-equiv=refresh content=5></noscript>"
             "<title>Dimplex Control</title><style>body{font-family:sans-serif;max-width:520px;margin:18px auto;"
             "padding:0 12px;color:#222}h2{color:#e25822;margin:0 0 8px}h3{margin:16px 0 4px}.b{display:inline-block;min-width:34px;text-align:center;"
             "padding:10px 12px;margin:3px;background:#eee;border-radius:8px;text-decoration:none;color:#222;border:0;font-size:100%}"
             ".b.on{background:#2e9e4f;color:#fff}.b.off,.b.warn{background:#c0392b;color:#fff}.b.wait{background:#e68a00;color:#fff}.pending{color:#e68a00}.msg{background:#fff3cd;padding:8px;border-radius:6px}"
             ".nav{display:inline-block;padding:8px 14px;margin-right:4px;border-radius:8px 8px 0 0;background:#eee;text-decoration:none;color:#222}"
             ".nav.on{background:#e25822;color:#fff}nav{border-bottom:3px solid #e25822;margin-bottom:8px}"
             ".big{font-size:230%;font-weight:bold}table{border-collapse:collapse}td,th{padding:2px 8px;text-align:left}"
             ".dim{color:#888;font-size:90%}input[type=number]{width:70px;padding:9px;font-size:100%}input[type=text]{padding:9px;font-size:100%;width:12em}</style></head><body>"
             "<h2>&#128293; Dimplex Control</h2><nav>" + navLink(PAGE_MAIN, "/", "Main") + navLink(PAGE_SETTINGS, "/settings", "Settings") + navLink(PAGE_DEBUG, "/debug", "Debug") +
             "</nav><div id=live>" + live + "</div>" + afterLive;
  h += String("<script>var box=document.getElementById('live');"
              "function tick(){fetch('/live?p=") + page + "',{cache:'no-store'}).then(function(r){return r.text()}).then(function(t){"
       "var a=document.activeElement;if(!(a&&a.tagName=='INPUT'))box.innerHTML=t;"
       "setTimeout(tick,t.indexOf('data-busy')>=0?" + String(PAGE_REFRESH_BUSY_MS) + ":" + String(PAGE_REFRESH_MS) + ");"
       "}).catch(function(){setTimeout(tick,5000)});}"
       "setTimeout(tick," + String(PAGE_REFRESH_MS) + ");</script></body></html>";
  g_webServer.send(200, "text/html", h);
}

// Main page.
static void handleMainPage() { sendPage(PAGE_MAIN, ""); }
// Settings page: the live block plus the (static) fireplace form and the factory reset link.
static void handleSettingsPage() {
  buildLiveHtml(PAGE_SETTINGS);                                  // sets g_renderPage and the snapshot used below
  const SharedState &s = g_webSnapshot;
  String form = "<form action='/set' style='margin-top:8px'>Address: <input type=text name=fpaddr value='" + htmlEscape(s.configuredAddress) +
                "' placeholder='auto-detect' maxlength=17> Passkey: <input type=text name=pkey value='" + htmlEscape(s.passkeyText) +
                "' maxlength=6 size=6 style='width:6em'><input type=hidden name=r value=settings> <button class=b>Save</button>"
                "<br><span class=dim>Leave the address empty to find the fireplace automatically (by its name). The bridge reconnects after saving.</span></form>"
                "<h3>Factory reset</h3><p class=dim>Erases all settings (Wi-Fi, MQTT, fireplace and passkey) and restarts the bridge in setup mode, like a physical reset button.</p>"
                "<a class='b warn' href='/reset'>Factory reset&hellip;</a>";
  sendPage(PAGE_SETTINGS, form);
}
// Debug page.
// Debug page: the live block plus the static "Last restart" section (why the bridge started, what the previous session
// was doing and its last log lines).
static void handleDebugPage() {
  String h = "<h3>Last restart</h3><p class=dim>Start #" + String((unsigned long) g_bootCount) + " &middot; reason: <b>" + htmlEscape(g_restartReason) + "</b></p>";
  if (g_previousLogValid) {
    h += "<p class=dim>The previous session was last doing: Bluetooth: <i>" + htmlEscape(g_previousBleStep) + "</i> &middot; MQTT: <i>" + htmlEscape(g_previousMqttStep) +
         "</i> &middot; web: <i>" + htmlEscape(g_previousWebStep) + "</i></p>"
         "<details><summary>Last log lines of the previous session</summary><pre style='font-size:80%;white-space:pre-wrap'>" + htmlEscape(g_previousLog) + "</pre></details>";
  } else h += "<p class=dim>No log of a previous session (power was off, or this is the first start).</p>";
  h += "<p class=dim><a href='/log'>Full log: previous and current session (text)</a></p>";
  sendPage(PAGE_DEBUG, h);
}
// The restart-proof log as plain text: the previous session first, then the latest lines of this one.
static void handleLog() {
  setWebStep("log");
  char current[LOG_RING_LINES * (LOG_RING_LINE_LENGTH + 1) + 1];
  xSemaphoreTake(g_logMutex, portMAX_DELAY); joinRingLines(g_rtcLog, current, sizeof(current)); xSemaphoreGive(g_logMutex);
  String out = String("Start #") + String((unsigned long) g_bootCount) + ", reason: " + g_restartReason + "\n\n=== previous session (last lines";
  out += g_previousLogValid ? String("), last steps: BLE: ") + g_previousBleStep + " / MQTT: " + g_previousMqttStep + " / web: " + g_previousWebStep + " ===\n" + g_previousLog
                            : String(") ===\n(none)\n");
  out += String("\n=== this session (last lines) ===\n") + current;
  g_webServer.send(200, "text/plain", out);
}
// Confirmation page for the factory reset (nothing is erased until the red button is pressed).
static void handleResetPage() {
  g_webServer.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>Factory reset</title></head>"
    "<body style='font-family:sans-serif;max-width:520px;margin:18px auto;padding:0 12px'><h2>Factory reset</h2>"
    "<p>This erases the Wi-Fi, MQTT, fireplace and passkey settings. The bridge restarts and opens the Wi-Fi network <b>Dimplex-Setup</b> (password dimplex123) "
    "so you can set it up again.</p><p><a href='/set?factory=confirm' style='background:#c0392b;color:#fff;padding:12px 16px;border-radius:8px;text-decoration:none'>Yes, erase everything</a> "
    "&nbsp; <a href='/settings'>Cancel</a></p></body></html>");
}
// Serve only the live block of one page (fetched by the page script).
static void handleLive() {
  setWebStep("live block");
  g_webServer.sendHeader("Cache-Control", "no-store");
  g_webServer.send(200, "text/html", buildLiveHtml(pageFromName(g_webServer.arg("p"))));
}
// Serve the complete status log as text, oldest first; one row per change (or per minute).
// With ?download=1 the browser saves it as a file.
static void handleCsv() {
  setWebStep("csv");
  copySnapshot(g_webSnapshot);
  const SharedState &s = g_webSnapshot;
  String out = "uptime_s;time;temp_c;temp_raw;effect;heater;heater_power;setpoint_c;flame_level;reg0040;reg0014;reg003c\n";
  for (int i = s.historyCount - 1; i >= 0; i--) {
    const StatusSample &x = s.history[(s.historyHead - 1 - i + 2 * HISTORY_SIZE) % HISTORY_SIZE];
    char clockText[24] = "";
    if (x.epoch) { time_t e = x.epoch; struct tm t; localtime_r(&e, &t); strftime(clockText, sizeof(clockText), "%Y-%m-%d %H:%M:%S", &t); }
    bool heating = x.control0 != 0 && (x.heater0 & 0x04) == 0;
    char power[10]; if (x.heaterPower == 0x01) strcpy(power, "low"); else if (x.heaterPower == 0x03) strcpy(power, "high"); else snprintf(power, sizeof(power), "raw%02x", x.heaterPower);
    char reg40[6]; snprintf(reg40, sizeof(reg40), "%02x%02x", x.control0, x.control1);
    char reg14[4]; snprintf(reg14, sizeof(reg14), "%02x", x.heater0);
    out += String(x.uptimeSec) + ";" + clockText + ";" + x.temperatureText + ";" + x.temperatureRaw + ";" + (x.control0 ? "on" : "off") + ";" +
           (heating ? "on" : "off") + ";" + power + ";" + String(x.setpoint) + ";" + String(x.flameLevel) + ";" + reg40 + ";" + reg14 + ";" + String(x.unknown3c) + "\n";
  }
  if (g_webServer.hasArg("download")) g_webServer.sendHeader("Content-Disposition", "attachment; filename=dimplex-status-log.csv");
  g_webServer.send(200, g_webServer.hasArg("download") ? "text/csv" : "text/plain", out);
}
// Handle a button press or form: only parse the request and queue the command(s), then redirect back to the page
// it came from (argument r). The result appears in the live block once the BLE task has finished.
static void handleSet() {
  setWebStep("set (button or form)");
  bool queued = true;
  String back = g_webServer.arg("r");
  if (g_webServer.hasArg("diag")) { g_loopGapMaxUs = 0; g_webRequestMaxUs = 0; setWebMessage("Timing maxima reset"); }
  // queueCommand() returns false when the queue is full; only a queued request is shown as "waiting" (orange button).
  auto request = [&](CommandType type, int argument, AwaitedControl control, int target) {
    bool ok = queueCommand(type, argument, SOURCE_WEB);
    if (ok) awaitControl(control, target); else queued = false;
  };
  const SharedState &now = g_webSnapshot;               // last state shown to the page (for the +/- setpoint buttons)
  auto clampSetpoint = [&](int degrees) { return degrees < now.setpointMin ? now.setpointMin : (degrees > now.setpointMax ? now.setpointMax : degrees); };
  if (g_webServer.hasArg("ble"))    { bool on = g_webServer.arg("ble") != "off"; request(on ? CMD_BLE_RESUME : CMD_BLE_RELEASE, 0, AW_BLE, on); }
  if (g_webServer.hasArg("fx"))     { bool on = g_webServer.arg("fx") == "on"; request(CMD_FLAME_EFFECT, on, AW_EFFECT, on); }
  if (g_webServer.hasArg("heater")) { bool on = g_webServer.arg("heater") == "on"; request(CMD_HEATER, on, AW_HEATER, on); }
  if (g_webServer.hasArg("hpwr"))   { bool high = g_webServer.arg("hpwr") == "high"; request(CMD_HEATER_POWER, high, AW_HEATER_POWER, high); }
  if (g_webServer.hasArg("sp"))     { int degrees = g_webServer.arg("sp").toInt(); request(CMD_SETPOINT, degrees, AW_SETPOINT, clampSetpoint(degrees)); }
  if (g_webServer.hasArg("spd"))    { int delta = g_webServer.arg("spd").toInt(); request(CMD_SETPOINT_DELTA, delta, AW_SETPOINT, clampSetpoint((g_awaited[AW_SETPOINT].active ? g_awaited[AW_SETPOINT].target : now.setpoint) + delta)); }
  if (g_webServer.hasArg("flame"))  { int n = g_webServer.arg("flame").toInt(); n = n < 1 ? 1 : (n > 6 ? 6 : n); request(CMD_FLAME_LEVEL, n, AW_FLAME_LEVEL, n); }
  if (g_webServer.hasArg("vol"))    { int n = g_webServer.arg("vol").toInt();   n = n < 0 ? 0 : (n > 6 ? 6 : n); request(CMD_VOLUME, n, AW_VOLUME, n); }
  if (g_webServer.hasArg("scan"))   queued &= queueCommand(CMD_SCAN, 0, SOURCE_WEB);
  if (g_webServer.hasArg("fpaddr") || g_webServer.hasArg("pkey")) {            // fireplace selection: the BLE task validates and applies it
    String address = g_webServer.arg("fpaddr"); address.trim(); if (address == "auto") address = "";
    String passkey = g_webServer.arg("pkey"); passkey.trim();
    stagePendingSettings(address, g_webServer.hasArg("fpaddr"), passkey, g_webServer.hasArg("pkey") && passkey.length() > 0);
    queued &= queueCommand(CMD_APPLY_SETTINGS, 0, SOURCE_WEB);
  }
  if (g_webServer.arg("factory") == "confirm") {
    queued &= queueCommand(CMD_FACTORY_RESET, 0, SOURCE_WEB);
    g_webServer.send(200, "text/html", "<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:520px;margin:18px auto;padding:0 12px'>"
                     "<h2>Resetting&hellip;</h2><p>All settings are being erased and the bridge restarts. Join the Wi-Fi network <b>Dimplex-Setup</b> "
                     "(password dimplex123) to set it up again.</p></body></html>");
    return;
  }
  if (g_webServer.hasArg("restart")) {
    g_webServer.send(200, "text/html", "<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:520px;margin:18px auto;padding:0 12px'>"
                     "<h2>Restarting&hellip;</h2><p>Reload <a href='/'>the page</a> in about 10 seconds.</p></body></html>");
    delay(300); restartDevice("restart button"); return;
  }
  if (!queued) setWebMessage("Busy: too many commands waiting, try again in a moment");
  g_webServer.sendHeader("Location", back == "settings" ? "/settings" : (back == "debug" ? "/debug" : "/")); g_webServer.send(303);
}
// Register the page handlers and start the web server.
static void startWebServer() {
  g_webServer.on("/", handleMainPage); g_webServer.on("/settings", handleSettingsPage); g_webServer.on("/debug", handleDebugPage);
  g_webServer.on("/reset", handleResetPage); g_webServer.on("/live", handleLive); g_webServer.on("/set", handleSet); g_webServer.on("/csv", handleCsv); g_webServer.on("/log", handleLog);
  g_webServer.begin(); g_webStarted = true;
}

// =====================================================================================================
// 8. STATUS LEDS (ledTask)
// =====================================================================================================

// Switch one LED (pin < 0 = not fitted).
static void setLed(int pin, bool on) { if (pin >= 0) digitalWrite(pin, (on == LED_ACTIVE_HIGH) ? HIGH : LOW); }
// "N short flashes, then a pause" pattern: returns whether the LED is on at time `nowMs`.
static bool flashPattern(uint32_t nowMs, int flashes) {
  uint32_t period = flashes * 300 + 1200, position = nowMs % period;
  if (position >= (uint32_t) flashes * 300) return false;
  return (position % 300) < 150;
}
// Decide the combined status LED from the connection flags (see the table at the top of this file).
static bool combinedLedState(uint32_t nowMs) {
  if (g_portalOpen) return (nowMs / 100) % 2 == 0;                 // setup portal: fast blink
  if (WiFi.status() != WL_CONNECTED) return flashPattern(nowMs, 1);
  if (!g_mqttUp) return flashPattern(nowMs, 2);
  if (!g_bleEnabled) return (nowMs / 1000) % 2 == 0;               // released for the original remote: slow blink
  if (!g_bleReady) return flashPattern(nowMs, 3);
  return true;                                                      // everything connected
}
// LED task: sets the pins every 50 ms. The combined LED follows combinedLedState(); the optional external
// LEDs show one connection each: on = connected, blinking = trying, off = down or released.
static void ledTask(void *) {
  for (;;) {
    uint32_t now = millis();
    bool blink = (now / 500) % 2 == 0;
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    setLed(LED_PIN_STATUS, combinedLedState(now));
    setLed(LED_PIN_WIFI, wifiUp ? true : (g_portalOpen ? blink : false));
    setLed(LED_PIN_MQTT, g_mqttUp ? true : (wifiUp ? blink : false));
    setLed(LED_PIN_BLE,  g_bleReady ? true : (g_bleEnabled ? blink : false));
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
// Configure the LED pins and start the LED task (before the blocking Wi-Fi setup so the portal can blink).
static void startLeds() {
  const int pins[4] = { LED_PIN_STATUS, LED_PIN_WIFI, LED_PIN_MQTT, LED_PIN_BLE };
  for (int pin : pins) if (pin >= 0) { pinMode(pin, OUTPUT); setLed(pin, false); }
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 1);
}

// =====================================================================================================
// 9. SETUP PORTAL, setup() AND loop()
// =====================================================================================================

// Connect to the saved Wi-Fi, or open the 'Dimplex-Setup' portal (forced = opened by the button, or first boot).
// Blocks until Wi-Fi is up or the portal times out (180 s).
static void runSetupPortal(bool forced) {
  if (g_webStarted) { g_webServer.stop(); g_webStarted = false; }   // free port 80 for the AP portal
  g_wifiManager.setConfigPortalTimeout(180);
  g_wifiManager.setSaveParamsCallback(onPortalSettingsSaved);
  param_mqttHost.setValue(cfg_mqttHost, 40); param_mqttPort.setValue(cfg_mqttPort, 6);
  param_mqttUser.setValue(cfg_mqttUser, 32); param_mqttPassword.setValue(cfg_mqttPassword, 40);
  param_fireplaceAddress.setValue(cfg_fireplaceAddress, 19); param_passkey.setValue(cfg_passkeyText, 11);
  // scan for nearby fireplaces to fill the picker, but only when the portal will show (avoids delaying normal boots)
  if (forced || !g_wifiManager.getWiFiIsSaved()) buildPortalScanList();
  if (forced) logLine("cfg", "opening setup portal (join WiFi 'Dimplex-Setup' / pass 'dimplex123')");
  else        logLine("wifi", "connecting to saved network (opens 'Dimplex-Setup' portal only if none saved / connect fails)");
  g_portalOpen = forced || !g_wifiManager.getWiFiIsSaved();         // for the LED; autoConnect only opens the portal when it has to
  if (forced) g_wifiManager.startConfigPortal("Dimplex-Setup", "dimplex123");
  else        g_wifiManager.autoConnect("Dimplex-Setup", "dimplex123");
  g_portalOpen = false;
  loadSettings();            // pick up any changes
  g_brokerKnown = false;     // look up the broker again with the (possibly new) host
}

// Start-up: create the queue and mutex, load settings, start BLE and the LED task, connect Wi-Fi (or open the
// portal), then start bleTask and mqttTask on core 0. The web server starts in loop() once Wi-Fi is up.
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  startDiagnosticsSession();               // saves the previous session's log and starts a new one (before the first log line)
  logLine("boot", "Dimplex Control bridge (NimBLE + setup portal, task layout) starting...");
  logLine("boot", "start #%lu, reason: %s", (unsigned long) g_bootCount, g_restartReason);

  g_commandQueue = xQueueCreate(8, sizeof(Command));
  g_snapshotMutex = xSemaphoreCreateMutex();
  startLeds();

  loadSettings();
  logLine("cfg", "mqtt host=%s port=%s user=%s", cfg_mqttHost, cfg_mqttPort, cfg_mqttUser);

  NimBLEDevice::init("dimplex-control");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(false, true, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);

  g_wifiManager.addParameter(&param_scanList); g_wifiManager.addParameter(&param_fireplaceAddress); g_wifiManager.addParameter(&param_passkey);
  g_wifiManager.addParameter(&param_mqttHost); g_wifiManager.addParameter(&param_mqttPort);
  g_wifiManager.addParameter(&param_mqttUser); g_wifiManager.addParameter(&param_mqttPassword);
  g_wifiManager.setHostname("dimplex-control");          // reachable as dimplex-control.local

  runSetupPortal(false);

  publishSnapshot();                       // initial snapshot (nothing connected yet)
  // Both worker tasks run on core 0 below the Wi-Fi/BT system tasks; loop() (web) keeps core 1 to itself.
  xTaskCreatePinnedToCore(bleTask,  "ble",  8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(mqttTask, "mqtt", 8192, nullptr, 1, nullptr, 0);
}

// Main loop (core 1): starts NTP, mDNS and the web server once Wi-Fi is up, then only serves web requests.
// It records the longest gap between two passes, which shows on the page as a responsiveness check.
void loop() {
  static uint32_t lastPassUs = 0;
  uint32_t nowUs = micros();
  if (lastPassUs && nowUs - lastPassUs > g_loopGapMaxUs) g_loopGapMaxUs = nowUs - lastPassUs;
  lastPassUs = nowUs;
  if (WiFi.status() == WL_CONNECTED) {
    if (!g_ntpStarted) {            // Dutch time zone incl. summer time; change the TZ string if you live elsewhere
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
      g_ntpStarted = true;
    }
    if (!g_webStarted) {
      MDNS.begin("dimplex-control");
      MDNS.addService("http", "tcp", 80);
      startWebServer();
      logLine("web", "control UI at http://dimplex-control.local/  (or http://%s/)", WiFi.localIP().toString().c_str());
      lastPassUs = 0; g_loopGapMaxUs = 0;     // do not count the one-time start-up work
    }
    uint32_t startUs = micros();
    g_webServer.handleClient();
    uint32_t durationUs = micros() - startUs;
    if (durationUs > g_webRequestMaxUs) g_webRequestMaxUs = durationUs;
  }
  delay(1);                          // everything else runs in bleTask, mqttTask and ledTask
}
