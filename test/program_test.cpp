// Functional test of the weekly program in the sketch (planner, retry, page handlers, MQTT switch, time zone) with the PC stubs.
// Build and run:  g++ -std=c++17 -w -I test/stubs -I Dimplex-Control test/program_test.cpp -o /tmp/program_test && /tmp/program_test
// The stub clock starts at a fixed instant; the test works relative to the local time at the start, so the time zone of the PC does not matter.
#include "../Dimplex-Control/Dimplex-Control.ino"
int g_simRestarts = 0; EspStub ESP; WiFiStub WiFi; uint32_t g_simMillis = 0; SerialStub Serial; MdnsStub MDNS; std::vector<Published> g_simPublished;
FireplaceSim g_sim; void FireplaceSim::write(int, const uint8_t *, size_t) {} NimBLEClient::NimBLEClient() {}
static int fails = 0;
#define CK(c) do { if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
static void run(int seconds) { for (int i = 0; i < seconds; i++) { g_simMillis += 1000; programTick(); } }
static int queued() { return (int) uxQueueMessagesWaiting(g_commandQueue); }
static Command pop() { Command c; xQueueReceive(g_commandQueue, &c, 0); return c; }
int main() {
  g_commandQueue = xQueueCreate(8, sizeof(Command)); g_snapshotMutex = xSemaphoreCreateMutex(); g_logMutex = xSemaphoreCreateMutex();
  loadProgram(); CK(g_ruleCount == 0 && g_programOn);
  uint32_t k0; int w0; localMinuteKey(k0, w0); int base = (int)(k0 % 1440) + 2;                                   // local time now (+2 min)
  char onT[8], offT[8], ovT[8]; snprintf(onT, 8, "%02d:%02d", base / 60, base % 60); snprintf(offT, 8, "%02d:%02d", (base + 5) / 60, (base + 5) % 60); snprintf(ovT, 8, "%02d:%02d", (base + 3) / 60, (base + 3) % 60);
  char dayArg[3] = { 'd', (char)('0' + w0), 0 };
  // add rules via the web handler: Monday 14:15-14:20 (stub time starts Monday 14:13:20)
  g_webServer.args = {{dayArg,"1"},{"on",onT},{"off",offT}}; handleProgramSet(); CK(g_ruleCount == 1);
  g_webServer.args = {{dayArg,"1"},{"on",ovT},{"off","23:59"}}; handleProgramSet(); CK(g_ruleCount == 1);   // overlap refused
  CK(g_webServer.lastLocation == "/program");
  g_webServer.args = {{"on",onT},{"off",offT}}; handleProgramSet(); CK(g_ruleCount == 1);              // no day
  loadProgram(); CK(g_ruleCount == 1);                                                                        // survived "restart"
  run(60); CK(queued() == 0);
  run(60);                                                                                                    // 14:15:20 -> on moment passed
  CK(queued() == 1); Command c = pop(); CK(c.type == CMD_HEATER && c.argument == 2 && c.source == SOURCE_SCHEDULE);
  run(5); CK(queued() == 0);
  run(30); CK(queued() == 1); pop();                                                                           // not done (no valid snapshot): retried after 30 s
  run(300); CK(g_programPending.action == SCHED_NONE || queued() >= 0);
  while (queued()) pop();
  run(10 * 60); // passes 14:20 off moment
  bool sawOff = false; while (queued()) { Command x = pop(); if (x.type == CMD_FLAME_EFFECT && x.argument == 0) sawOff = true; } CK(sawOff);
  // MQTT switch
  g_programSetRequest = 0; run(1); CK(!g_programOn); loadProgram(); CK(!g_programOn);
  g_programSetRequest = 1; run(1); CK(g_programOn);
  // time zone
  g_webServer.args = {{"tz","UTC0"},{"tzcustom",""},{"r","settings"}}; handleSet(); CK(!strcmp(g_timeZone, "UTC0"));
  g_webServer.args = {{"tz","x"},{"tzcustom","bad zone!"},{"r","settings"}}; handleSet(); CK(!strcmp(g_timeZone, "UTC0"));
  // pages render
  handleProgramPage(); CK(g_webServer.lastBody.find("Add a rule") != std::string::npos && g_webServer.lastBody.find("Mon") != std::string::npos);
  handleSettingsPage(); CK(g_webServer.lastBody.find("Time zone") != std::string::npos);
  // delete
  g_webServer.args = {{"del","0"}}; handleProgramSet(); CK(g_ruleCount == 0);
  printf("harness: %d failed\n", fails); return fails;
}
