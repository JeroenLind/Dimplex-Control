// Functional test of the fireplace identification and the "has sound" setting in the sketch, with the PC stubs.
// Build and run:  g++ -std=c++17 -w -I test/stubs -I Dimplex-Control test/model_test.cpp -o /tmp/model_test && /tmp/model_test
#include "../Dimplex-Control/Dimplex-Control.ino"
int g_simRestarts = 0; EspStub ESP; WiFiStub WiFi; uint32_t g_simMillis = 0; SerialStub Serial; MdnsStub MDNS; std::vector<Published> g_simPublished;
FireplaceSim g_sim; void FireplaceSim::write(int, const uint8_t *, size_t) {} NimBLEClient::NimBLEClient() {}
static int fails = 0;
#define CK(c) do { if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static std::vector<uint8_t> bytes(const char *t) { return std::vector<uint8_t>(t, t + strlen(t)); }
// What the last published discovery message for an entity says ("" = removed or never published).
static std::string lastPayload(const char *topic) { std::string r; for (auto &p : g_simPublished) if (p.topic == topic) r = p.payload; return r; }

int main() {
  g_commandQueue = xQueueCreate(8, sizeof(Command)); g_snapshotMutex = xSemaphoreCreateMutex(); g_logMutex = xSemaphoreCreateMutex();
  loadSettings(); CK(!g_hasSound);                                                   // default: no sound
  // A fireplace like ours: volume at 0x0076, handshake at 0x0087
  g_bleClient = new NimBLEClient(); g_bleConnected = true;
  static NimBLERemoteCharacteristic ch[] = { {0x0003, 0x2a00}, {0x0040, 0}, {0x0076, 0x4010}, {0x0087, 0x2001}, {0x0095, 0x200a}, {0x009b, 0x200d}, {0x0093, 0x2009}, {0x0097, 0x200b} };
  for (auto &c : ch) g_bleClient->service.chars.push_back(&c);
  g_sim.regs[0x0003] = bytes("FI7539<Dimplex>"); g_sim.regs[0x0095] = bytes("EP4120"); g_sim.regs[0x009b] = bytes("F000B5E0");
  g_sim.regs[0x0093] = {0x05}; g_sim.regs[0x0097] = {0x8e, 0x16};
  CK(findFireplaceCharacteristics());
  CK(g_ident.known && !strcmp(g_ident.name, "FI7539") && !strcmp(g_ident.firmware, "EP4120") && !strcmp(g_ident.serial, "F000B5E0"));
  CK(g_ident.hardware == 5 && g_ident.reg200b[0] == 0x8e && g_ident.reg200b[1] == 0x16);
  CK(g_ident.volumeHandle == 0x0076 && g_ident.initHandle == 0x0087);
  // A unit with the handles shifted by two is found too
  ch[2].handle = 0x0074; ch[3].handle = 0x0085; g_sim.regs.clear();
  CK(findFireplaceCharacteristics() && g_ident.volumeHandle == 0x0074 && g_ident.initHandle == 0x0085 && g_ident.name[0] == 0);   // no name register content: empty, no crash
  ch[2].handle = 0x0076; ch[3].handle = 0x0087;
  g_sim.regs[0x0003] = bytes("FI7539<Dimplex>"); g_sim.regs[0x0095] = bytes("EP4120"); g_sim.regs[0x009b] = bytes("F000B5E0"); findFireplaceCharacteristics();

  // Home Assistant: device block with model and firmware, Volume and Sound only with the setting
  g_shared.ident = g_ident; buildDeviceBlock(g_ident);
  CK(g_deviceBlock.indexOf("Opti-myst (FI7539)") >= 0 && g_deviceBlock.indexOf("\"sw\":\"EP4120\"") >= 0);
  g_simPublished.clear(); publishHomeAssistantDiscovery();
  CK(lastPayload("homeassistant/number/dimplex/volume/config") == "" && lastPayload("homeassistant/switch/dimplex/sound/config") == "");
  CK(lastPayload("homeassistant/switch/dimplex/power/config").find("EP4120") != std::string::npos);
  CK(identKeyOf(g_shared).indexOf("nosound") >= 0);
  // Switch the setting on through the web handler and the command
  g_webServer.args = {{"sound", "on"}, {"r", "settings"}}; handleSet();
  Command c; CK(xQueueReceive(g_commandQueue, &c, 0) && c.type == CMD_SET_HAS_SOUND && c.argument == 1);
  executeCommand(c); CK(g_hasSound);
  g_prefs.begin("cfg", true); CK(g_prefs.getBool("sound", false)); g_prefs.end();
  g_simPublished.clear(); publishHomeAssistantDiscovery();
  CK(lastPayload("homeassistant/number/dimplex/volume/config").find("Volume") != std::string::npos);
  CK(lastPayload("homeassistant/switch/dimplex/sound/config").find("Sound") != std::string::npos);
  CK(identKeyOf(g_shared).indexOf("|sound") >= 0);                       // the key changes, so discovery is published again
  // MQTT volume commands are accepted only with the setting
  char t1[] = "dimplex/volume/set"; byte p1[] = {'4'};
  onMqttMessage(t1, p1, 1); CK(xQueueReceive(g_commandQueue, &c, 0) && c.type == CMD_VOLUME && c.argument == 4);
  g_hasSound = false; onMqttMessage(t1, p1, 1); CK(!xQueueReceive(g_commandQueue, &c, 0));
  // Pages
  g_hasSound = true; publishSnapshot();
  handleSettingsPage(); CK(g_webServer.lastBody.find("My fireplace has a sound effect") != std::string::npos && g_webServer.lastBody.find("<h3>Volume</h3>") != std::string::npos);
  g_hasSound = false; publishSnapshot(); handleSettingsPage(); CK(g_webServer.lastBody.find("<h3>Volume</h3>") == std::string::npos);
  handleDebugPage(); CK(g_webServer.lastBody.find("EP4120") != std::string::npos && g_webServer.lastBody.find("F000B5E0") != std::string::npos && g_webServer.lastBody.find("0x76 / 0x87") != std::string::npos);
  printf("model test: %d failed\n", fails); return fails;
}
