// Minimal stand-ins for the Arduino / ESP32 / NimBLE / PubSubClient / WebServer APIs used by Dimplex-Control.ino, so the sketch
// can be compiled and tested on a PC. Time is simulated: delay() advances the clock, nothing sleeps.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <functional>
#include <atomic>

#define PROGMEM
#define RTC_NOINIT_ATTR
#define HEX 16
typedef uint8_t byte;

// ---- simulated clock ----
extern uint32_t g_simMillis;
static inline uint32_t millis() { return g_simMillis; }
static inline uint32_t micros() { return g_simMillis * 1000u; }
static inline void delay(uint32_t ms) { g_simMillis += ms; }

// ---- String ----
class String {
 public:
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &c) : s(c) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(long long v) : s(std::to_string(v)) {}
  String(unsigned long long v) : s(std::to_string(v)) {}
  String(uint8_t v) : s(std::to_string((int) v)) {}
  String(double v, int decimals = 2) { char f[16], b[48]; snprintf(f, sizeof f, "%%.%df", decimals); snprintf(b, sizeof b, f, v); s = b; }
  String(float v, int decimals = 2) : String((double) v, decimals) {}
  String(int v, int base) { char b[32]; snprintf(b, sizeof b, base == 16 ? "%x" : "%d", v); s = b; }
  String(unsigned v, int base) : String((int) v, base) {}
  String(uint8_t v, int base) : String((int) v, base) {}
  const char *c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  int toInt() const { return atoi(s.c_str()); }
  float toFloat() const { return (float) atof(s.c_str()); }
  void trim() { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n"); s = a == std::string::npos ? "" : s.substr(a, b - a + 1); }
  void replace(const char *from, const char *to) { size_t p = 0; std::string f = from, t = to; while ((p = s.find(f, p)) != std::string::npos) { s.replace(p, f.size(), t); p += t.size(); } }
  int indexOf(const char *x) const { size_t p = s.find(x); return p == std::string::npos ? -1 : (int) p; }
  bool endsWith(const char *x) const { std::string e = x; return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
  String substring(unsigned from, unsigned to) const { return String(s.substr(from, to - from)); }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *o) { s += o; return *this; }
  String &operator+=(char o) { s += o; return *this; }
  bool operator==(const char *o) const { return s == o; }
  bool operator!=(const char *o) const { return s != o; }
  bool operator==(const String &o) const { return s == o.s; }
  bool operator!=(const String &o) const { return s != o.s; }
  operator const char *() const { return s.c_str(); }
};
static inline String operator+(const String &a, const String &b) { return String(a.s + b.s); }
static inline String operator+(const String &a, const char *b) { return String(a.s + b); }
static inline String operator+(const char *a, const String &b) { return String(std::string(a) + b.s); }
static inline String operator+(const String &a, char b) { return String(a.s + b); }
static inline String operator+(const String &a, int b) { return String(a.s + std::to_string(b)); }
static inline String operator+(const String &a, unsigned b) { return String(a.s + std::to_string(b)); }
static inline String operator+(const String &a, unsigned long b) { return String(a.s + std::to_string(b)); }

// ---- serial, pins ----
struct SerialStub { void begin(int) {} void println() {} void println(const char *t) { if (getenv("SKETCH_LOG")) puts(t); } };
extern SerialStub Serial;
#define OUTPUT 1
#define HIGH 1
#define LOW 0
static inline void pinMode(int, int) {}
static inline void digitalWrite(int, int) {}

// ---- FreeRTOS (single threaded: queues are deques, mutexes do nothing, tasks are never started) ----
typedef int BaseType_t; typedef uint32_t TickType_t;
struct QueueObj { std::deque<std::vector<uint8_t>> items; size_t itemSize; };
typedef QueueObj *QueueHandle_t; typedef void *SemaphoreHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)
static inline QueueHandle_t xQueueCreate(int, size_t itemSize) { auto *q = new QueueObj(); q->itemSize = itemSize; return q; }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t) { if (q->items.size() >= 8) return 0; q->items.emplace_back((const uint8_t *) item, (const uint8_t *) item + q->itemSize); return 1; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t) { if (q->items.empty()) return 0; memcpy(item, q->items.front().data(), q->itemSize); q->items.pop_front(); return 1; }
static inline uint32_t uxQueueMessagesWaiting(QueueHandle_t q) { return (uint32_t) q->items.size(); }
static inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void *) 1; }
static inline void xSemaphoreTake(SemaphoreHandle_t, TickType_t) {}
static inline void xSemaphoreGive(SemaphoreHandle_t) {}
static inline BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, int, void *, int, void *, int) { return 1; }
static inline void vTaskDelay(TickType_t ms) { g_simMillis += ms; }
static inline uint32_t uxTaskGetStackHighWaterMark(void *) { return 4000; }

// ---- ESP system ----
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT } esp_reset_reason_t;
static inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
static inline int64_t esp_timer_get_time() { return (int64_t) g_simMillis * 1000; }
struct EspStub { void restart() { extern int g_simRestarts; g_simRestarts++; } uint32_t getFreeHeap() { return 150000; } uint32_t getMinFreeHeap() { return 120000; } };
extern EspStub ESP;
#define ESP_PWR_LVL_P9 9
#define BLE_HS_IO_KEYBOARD_DISPLAY 4
#define BLE_ADDR_PUBLIC 0

// ---- time ----
static inline void configTzTime(const char *, const char *, const char *) {}
static inline bool getLocalTime(struct tm *t, uint32_t = 0) { time_t n = 1790000000 + g_simMillis / 1000; localtime_r(&n, t); return true; }

// ---- Wi-Fi, mDNS ----
#define WL_CONNECTED 3
#define WL_DISCONNECTED 6
struct IPAddress {
  uint32_t v = 0;
  bool fromString(const String &t) { unsigned a, b, c, d; if (sscanf(t.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) { v = a | b << 8 | c << 16 | d << 24; return true; } return false; }
  String toString() const { char x[20]; snprintf(x, sizeof x, "%u.%u.%u.%u", v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24); return x; }
  operator uint32_t() const { return v; }
};
struct WiFiStub { int st = WL_CONNECTED; int status() { return st; } IPAddress localIP() { IPAddress i; i.v = 0x0101A8C0; return i; } int RSSI() { return -60; }
                  void setAutoReconnect(bool) {} void reconnect() {} void disconnect() {} void begin() {} };
extern WiFiStub WiFi;
struct WiFiClient {};
struct MdnsStub { bool begin(const char *) { return true; } void addService(const char *, const char *, int) {} IPAddress queryHost(const String &, int) { return IPAddress(); } };
extern MdnsStub MDNS;

// ---- Preferences (in-memory flash, survives "restarts" within one test run) ----
class Preferences {
  std::string ns_;
  static std::map<std::string, std::map<std::string, std::string>> &store() { static std::map<std::string, std::map<std::string, std::string>> m; return m; }
 public:
  bool begin(const char *ns, bool = false) { ns_ = ns; return true; }
  void end() {}
  void clear() { store()[ns_].clear(); }
  String getString(const char *k, const String &d = String()) { auto &m = store()[ns_]; return m.count(k) ? String(m[k]) : d; }
  size_t putString(const char *k, const String &v) { store()[ns_][k] = v.s; return v.length(); }
  uint8_t getUChar(const char *k, uint8_t d = 0) { auto &m = store()[ns_]; return m.count(k) ? (uint8_t) atoi(m[k].c_str()) : d; }
  size_t putUChar(const char *k, uint8_t v) { store()[ns_][k] = std::to_string(v); return 1; }
  uint16_t getUShort(const char *k, uint16_t d = 0) { auto &m = store()[ns_]; return m.count(k) ? (uint16_t) atoi(m[k].c_str()) : d; }
  size_t putUShort(const char *k, uint16_t v) { store()[ns_][k] = std::to_string(v); return 2; }
  uint32_t getUInt(const char *k, uint32_t d = 0) { auto &m = store()[ns_]; return m.count(k) ? (uint32_t) strtoul(m[k].c_str(), nullptr, 10) : d; }
  size_t putUInt(const char *k, uint32_t v) { store()[ns_][k] = std::to_string(v); return 4; }
  bool has(const char *k) { return store()[ns_].count(k) > 0; }
};

// ---- WiFiManager ----
struct WiFiManagerParameter {
  std::string value;
  WiFiManagerParameter(const char *) {}
  WiFiManagerParameter(const char *, const char *, const char *def, int) : value(def) {}
  const char *getValue() { return value.c_str(); }
  void setValue(const char *v, int) { value = v; }
};
struct WiFiManager {
  void addParameter(WiFiManagerParameter *) {} void setHostname(const char *) {} void setConfigPortalTimeout(int) {}
  void setSaveParamsCallback(void (*)()) {} bool getWiFiIsSaved() { return true; } void startConfigPortal(const char *, const char *) {}
  void autoConnect(const char *, const char *) {} void resetSettings() {}
};

// ---- PubSubClient: records every publish, lets a test deliver incoming messages ----
struct Published { std::string topic, payload; bool retained; };
extern std::vector<Published> g_simPublished;
struct PubSubClient {
  void (*callback)(char *, byte *, unsigned int) = nullptr;
  PubSubClient(WiFiClient &) {}
  void setCallback(void (*cb)(char *, byte *, unsigned int)) { callback = cb; }
  void setBufferSize(int) {} void setSocketTimeout(int) {} void setServer(IPAddress, uint16_t) {} void setServer(const char *, uint16_t) {}
  bool connected() { return true; } int state() { return 0; } void loop() {}
  bool connect(const char *, const char *, const char *, const char *, int, bool, const char *) { return true; }
  bool subscribe(const char *) { return true; }
  bool publish(const char *t, const char *p, bool r = false) { g_simPublished.push_back({t, p, r}); return true; }
};

// ---- WebServer ----
struct WebServer {
  std::map<std::string, std::string> args; std::string lastBody, lastLocation; int lastCode = 0;
  WebServer(int) {}
  void on(const char *, void (*)()) {} void begin() {} void stop() {} void handleClient() {}
  bool hasArg(const char *k) { return args.count(k) > 0; }
  String arg(const char *k) { return args.count(k) ? String(args[k]) : String(); }
  void sendHeader(const char *n, const char *v) { if (!strcmp(n, "Location")) lastLocation = v; }
  void send(int code, const char *, const String &body) { lastCode = code; lastBody = body.s; }
  void send(int code, const char *, const char *body) { lastCode = code; lastBody = body; }
  void send(int code) { lastCode = code; }
  void send_P(int code, const char *, const char *, size_t) { lastCode = code; }
};

// ---- NimBLE ----
struct NimBLEConnInfo { bool isEncrypted() const { return true; } };
struct NimBLEAddress {
  std::string a;
  NimBLEAddress() {} NimBLEAddress(const char *x, int) : a(x) {}
  std::string toString() const { return a; }
};
struct NimBLEAttValue {
  std::vector<uint8_t> d;
  size_t length() const { return d.size(); }
  uint8_t operator[](size_t i) const { return d[i]; }
};
// The simulated fireplace behind the characteristics (see test/sketch_simulation_test.cpp).
struct FireplaceSim {
  std::map<int, std::vector<uint8_t>> regs;
  std::deque<int> writeLog;                      // handles written, in order
  int writes = 0, failWrites = 0;                // failWrites > 0: writes are ignored (fireplace does not react)
  void reset();
  void write(int handle, const uint8_t *data, size_t n);
};
extern FireplaceSim g_sim;
struct NimBLERemoteCharacteristic {
  int handle;
  explicit NimBLERemoteCharacteristic(int h) : handle(h) {}
  int getHandle() const { return handle; }
  NimBLEAttValue readValue() { NimBLEAttValue v; v.d = g_sim.regs[handle]; return v; }
  bool writeValue(const uint8_t *data, size_t n, bool) { g_sim.write(handle, data, n); return true; }
};
struct NimBLERemoteService {
  std::vector<NimBLERemoteCharacteristic *> chars;
  std::vector<NimBLERemoteCharacteristic *> getCharacteristics(bool) { return chars; }
};
struct NimBLEClient;
struct NimBLEClientCallbacks {
  virtual ~NimBLEClientCallbacks() {}
  virtual void onConnect(NimBLEClient *) {}
  virtual void onDisconnect(NimBLEClient *, int) {}
  virtual void onPassKeyEntry(NimBLEConnInfo &) {}
  virtual void onConfirmPasskey(NimBLEConnInfo &, uint32_t) {}
  virtual void onAuthenticationComplete(NimBLEConnInfo &) {}
};
struct NimBLEClient {
  NimBLEClientCallbacks *cb = nullptr; bool connectedFlag = false;
  NimBLERemoteService service;
  NimBLEClient();
  void setClientCallbacks(NimBLEClientCallbacks *c, bool) { cb = c; }
  void setConnectTimeout(int) {}
  bool connect(const NimBLEAddress &) { connectedFlag = true; if (cb) cb->onConnect(this); return true; }
  bool secureConnection() { return true; }
  bool isConnected() { return connectedFlag; }
  void disconnect() { connectedFlag = false; if (cb) cb->onDisconnect(this, 0); }
  std::vector<NimBLERemoteService *> getServices(bool) { return { &service }; }
};
struct NimBLEAdvertisedDevice { std::string getName() const { return ""; } NimBLEAddress getAddress() const { return NimBLEAddress(); } int getRSSI() const { return 0; } };
struct NimBLEScanResults { int getCount() const { return 0; } const NimBLEAdvertisedDevice *getDevice(int) const { return nullptr; } };
struct NimBLEScan { void setActiveScan(bool) {} NimBLEScanResults getResults(int, bool) { return NimBLEScanResults(); } void clearResults() {} };
struct NimBLEDevice {
  static void init(const char *) {} static void setPower(int) {} static void setSecurityAuth(bool, bool, bool) {} static void setSecurityIOCap(int) {}
  static NimBLEClient *createClient() { return new NimBLEClient(); }
  static NimBLEScan *getScan() { static NimBLEScan s; return &s; }
  static void injectPassKey(NimBLEConnInfo &, uint32_t) {} static void injectConfirmPasskey(NimBLEConnInfo &, bool) {}
};
