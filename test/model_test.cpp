// Functional test of the fireplace identification and the "has sound" setting in the sketch, with the PC stubs.
// Build and run:  g++ -std=c++17 -w -I test/stubs -I Dimplex-Control test/model_test.cpp -o /tmp/model_test && /tmp/model_test
#include "../Dimplex-Control/Dimplex-Control.ino"
int g_simRestarts = 0; EspStub ESP; WiFiStub WiFi; uint32_t g_simMillis = 0; SerialStub Serial; MdnsStub MDNS; std::vector<Published> g_simPublished;
FireplaceSim g_sim; void FireplaceSim::write(int h, const uint8_t *d, size_t n) { if (failWrites > 0) { failWrites--; return; } writes++; writeLog.push_back(h); regs[h] = std::vector<uint8_t>(d, d + n); } NimBLEClient::NimBLEClient() {}
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
  handleSettingsPage(); CK(g_webServer.lastBody.find("My fireplace has a sound effect") != std::string::npos && g_webServer.lastBody.find("<h3>Volume</h3>") == std::string::npos);   // the volume is not on Settings
  handleMainPage(); CK(g_webServer.lastBody.find("<h3>Volume</h3>") != std::string::npos);                       // but on the Main page
  g_hasSound = false; publishSnapshot(); handleMainPage(); CK(g_webServer.lastBody.find("<h3>Volume</h3>") == std::string::npos);
  handleSettingsPage(); CK(g_webServer.lastBody.find("My fireplace has a sound effect") != std::string::npos);   // the setting itself is always there
  handleDebugPage(); CK(g_webServer.lastBody.find("EP4120") != std::string::npos && g_webServer.lastBody.find("F000B5E0") != std::string::npos && g_webServer.lastBody.find("0x76 / 0x87") != std::string::npos);
  // A failed connect logs the error code, its meaning and the target, in lines that fit the restart-proof log
  g_rtcLog.magic = RTC_LOG_MAGIC; g_rtcLog.head = 0; g_rtcLog.count = 0;
  g_bleConnected = false; g_haveTarget = false; strlcpy(cfg_fireplaceAddress, "24:6f:28:aa:bb:cc", sizeof(cfg_fireplaceAddress));
  g_bleClient->failConnect = true; g_bleClient->lastError = 0x23e;
  bleConnectToFireplace();
  std::string log; for (int i = 0; i < g_rtcLog.count; i++) { log += g_rtcLog.lines[i]; log += "\n"; }
  CK(log.find("error 574 (0x23e)") != std::string::npos && log.find("not reachable, or busy with another central") != std::string::npos);
  CK(log.find("target 24:6f:28:aa:bb:cc (pinned in Settings), scanning=0") != std::string::npos);
  for (int i = 0; i < g_rtcLog.count; i++) CK(strlen(g_rtcLog.lines[i]) < LOG_RING_LINE_LENGTH - 1);   // nothing is cut off
  publishSnapshot(); g_bleEnabled = true; handleMainPage();                                              // and the Main page shows it while connecting
  CK(g_webServer.lastBody.find("Last attempt failed (1x since the last success): error 574 (0x23e)") != std::string::npos);
  // Other failing calls are logged too: secure failure, a write that fails, a write while the link is down
  g_rtcLog.head = 0; g_rtcLog.count = 0; g_bleClient->failConnect = false; g_bleClient->failSecure = true; g_bleClient->lastError = 0x205; g_bleConnected = false; g_haveTarget = true;
  bleConnectToFireplace();
  { std::string l2; for (int i = 0; i < g_rtcLog.count; i++) { l2 += g_rtcLog.lines[i]; l2 += "\n"; } CK(l2.find("secure failed, error 517 (0x205)") != std::string::npos); }
  g_bleClient->failSecure = false;
  g_rtcLog.head = 0; g_rtcLog.count = 0; g_bleConnected = true; NimBLERemoteCharacteristic::failWrite = true; uint8_t one = 1;
  CK(!writeRegister(g_chControl, &one, 1)); CK(!writeRegister(nullptr, &one, 1)); g_bleConnected = false; CK(!writeRegister(g_chControl, &one, 1));
  NimBLERemoteCharacteristic::failWrite = false;
  { std::string l3; for (int i = 0; i < g_rtcLog.count; i++) { l3 += g_rtcLog.lines[i]; l3 += "\n"; CK(strlen(g_rtcLog.lines[i]) < LOG_RING_LINE_LENGTH - 1); }
    CK(l3.find("write 0x0040 failed, error 517") != std::string::npos && l3.find("write skipped: register not found") != std::string::npos && l3.find("write 0x0040 skipped: link is down") != std::string::npos); }
  CK(bleErrorText(2) != nullptr && !strcmp(bleErrorText(0), "no error recorded") && strlen(bleErrorText(15)) < 60);
  handleDebugPage(); CK(g_webServer.lastBody.find("<h3>Versions</h3>") != std::string::npos && g_webServer.lastBody.find("NimBLE-Arduino ") != std::string::npos && g_webServer.lastBody.find("ESP32-STUB rev 3") != std::string::npos);
  // ---- Auto heater power (docs/design.md 5.8) ----
  { static NimBLERemoteCharacteristic chHeater(0x0014, 0), chPower(0x0048, 0), chTemp(0x0055, 0);
    g_chHeater = &chHeater; g_chHeaterPower = &chPower; g_chTemperature = &chTemp;
    g_bleConnected = true; g_bleReady = true; g_registersRead = true; g_bleClient->failConnect = false; g_bleClient->failSecure = false;
    g_sim.regs[0x0040] = {0x06, 0x00}; g_sim.regs[0x0014] = {0x00, 0x00}; g_sim.regs[0x0048] = {0x01};         // effect on, element running, low power
    auto setTemp = [&](int whole, int tenth) { g_sim.regs[0x0055] = {0x00, (uint8_t) whole, (uint8_t) tenth}; readFireplaceState(); };
    g_rtcLog.head = 0; g_rtcLog.count = 0;
    g_controlMode = CONTROL_INTERNAL; g_bridgeSetpoint = 21; g_autoPower = { true, 11, 10 }; g_thermostatWanted = false;
    setTemp(19, 6); runThermostat();                                                                         // shortfall 1.4 -> high
    CK(g_thermostatWanted && g_regHeaterPower == 0x03);
    setTemp(20, 0); runThermostat(); CK(g_regHeaterPower == 0x01);                                           // shortfall 1.0 -> low
    setTemp(19, 9); runThermostat(); CK(g_regHeaterPower == 0x03);                                           // shortfall 1.1 -> high (boundary)
    g_autoPower = { true, 15, 5 };                                                                           // a wide band: stays as it is inside it
    setTemp(20, 0); runThermostat(); CK(g_regHeaterPower == 0x03);                                           // shortfall 1.0, between 0.5 and 1.5
    setTemp(20, 6); runThermostat(); CK(g_regHeaterPower == 0x01);                                           // shortfall 0.4 -> low
    setTemp(20, 0); runThermostat(); CK(g_regHeaterPower == 0x01);                                           // inside the band again: no change
    int writesBefore = g_sim.writes; setTemp(20, 0); runThermostat(); runThermostat(); CK(g_sim.writes == writesBefore);   // nothing is written when nothing changes
    // the snapshot shows it, the manual buttons are replaced
    g_autoPower = { true, 11, 10 }; setTemp(19, 6); runThermostat(); publishSnapshot();
    handleSettingsPage(); CK(g_webServer.lastBody.find("<b>Automatic</b>: high (1.8 kW)") != std::string::npos && g_webServer.lastBody.find("shortfall 1.4") != std::string::npos);
    CK(g_webServer.lastBody.find("hpwr=low") == std::string::npos && g_webServer.lastBody.find("Auto heater power</h3>") != std::string::npos);
    // a manual command is ignored while Auto is active
    { Command c = { CMD_HEATER_POWER, 0, SOURCE_WEB }; executeCommand(c); CK(g_regHeaterPower == 0x03); }
    // a write that the fireplace ignores: logged, retried after 30 s
    setTemp(20, 6); g_sim.failWrites = 1; runThermostat(); CK(g_regHeaterPower == 0x03);
    g_simMillis += 10000; g_sim.failWrites = 0; runThermostat(); CK(g_regHeaterPower == 0x03);                // too early to retry
    g_simMillis += 25000; runThermostat(); CK(g_regHeaterPower == 0x01);
    // the element is off (target reached): nothing is written; Auto corrects the power when it runs again
    g_sim.regs[0x0014] = {0x04, 0x00}; setTemp(22, 5); g_autoPowerRetryAtMs = 0; size_t logBefore = g_sim.writeLog.size(); g_sim.writes += 0; runThermostat(); { bool power = false; for (size_t k = logBefore; k < g_sim.writeLog.size(); k++) if (g_sim.writeLog[k] == 0x0048) power = true; CK(!power); }   // the thermostat may switch the element, Auto does not write the power
    // back to a running element with the power high (for the next step)
    g_sim.regs[0x0014] = {0x00, 0x00}; g_sim.regs[0x0048] = {0x03}; readFireplaceState();
    // Auto is not active in the mode Fireplace thermostat: manual works, page greyed out
    g_controlMode = CONTROL_FIREPLACE; publishSnapshot();
    { Command c = { CMD_HEATER_POWER, 0, SOURCE_WEB }; executeCommand(c); CK(g_regHeaterPower == 0x01); }
    handleSettingsPage(); CK(g_webServer.lastBody.find("hpwr=low") != std::string::npos && g_webServer.lastBody.find("Needs a temperature sensor mode") != std::string::npos && g_webServer.lastBody.find("name=aphi step=0.1 min=0.1 max=5 value=1.1 disabled") != std::string::npos);
    g_controlMode = CONTROL_INTERNAL;
    // settings: refused entries are not saved, valid ones are saved and survive a restart
    g_webServer.args = {{"apon","1"},{"aphi","1.0"},{"aplo","1.0"},{"r","settings"}}; handleSet(); { Command c; xQueueReceive(g_commandQueue, &c, 0); CK(c.type == CMD_APPLY_AUTOPOWER); executeCommand(c); }
    CK(g_autoPower.highLimit == 11 && g_autoPower.lowLimit == 10);                                            // unchanged
    g_webServer.args = {{"apon","1"},{"aphi","2.5"},{"aplo","0.7"},{"r","settings"}}; handleSet(); { Command c; xQueueReceive(g_commandQueue, &c, 0); executeCommand(c); }
    CK(g_autoPower.enabled && g_autoPower.highLimit == 25 && g_autoPower.lowLimit == 7);
    g_autoPower = AutoPowerSettings AUTOPOWER_DEFAULT_SETTINGS; loadSettings(); CK(g_autoPower.enabled && g_autoPower.highLimit == 25 && g_autoPower.lowLimit == 7);
    g_webServer.args = {{"apon","0"},{"aphi","1.1"},{"aplo","1.0"},{"r","settings"}}; handleSet(); { Command c; xQueueReceive(g_commandQueue, &c, 0); executeCommand(c); }
    CK(!g_autoPower.enabled);
    std::string pl; for (int i = 0; i < g_rtcLog.count; i++) { pl += g_rtcLog.lines[i]; pl += "\n"; if (strstr(g_rtcLog.lines[i], "[power]")) CK(strlen(g_rtcLog.lines[i]) < LOG_RING_LINE_LENGTH - 1); }   // the power lines fit
    CK(pl.find("auto: high, shortfall 1.4 (19.6 / 21)") != std::string::npos && pl.find("manual power ignored: Auto is on") != std::string::npos);
    CK(pl.find("auto: write failed, retry in 30 s") != std::string::npos && pl.find("auto settings refused") != std::string::npos && pl.find("auto settings: on, high 2.5, low 0.7") != std::string::npos);
  }
  printf("model test: %d failed\n", fails); return fails;
}
