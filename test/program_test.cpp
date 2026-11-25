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
  // clock line on the Program page: weekday, date, time and the NTP status
  { handleProgramPage(); struct tm t; getLocalTime(&t, 0); char want[40]; strftime(want, sizeof(want), "%A %Y-%m-%d %H:%M:%S", &t);
    CK(g_webServer.lastBody.find(std::string("<b>Bridge time:</b> ") + want + " &middot; NTP: synchronized") != std::string::npos);
    g_lastNtpSync = 1790000000; handleProgramPage(); char sync[40]; time_t e = 1790000000; struct tm l; localtime_r(&e, &l); strftime(sync, sizeof(sync), ", last sync %Y-%m-%d %H:%M:%S", &l);
    CK(g_webServer.lastBody.find(sync) != std::string::npos); g_lastNtpSync = 0; }
  // off-only rule: no on time, form field left empty. Overlap with another rule is refused, "already off" does nothing.
  { uint32_t k; int w; localMinuteKey(k, w); int t0 = (int)(k % 1440) + 2; char offA[8], insideT[8], dayA[3] = { 'd', (char)('0' + w), 0 };
    snprintf(offA, 8, "%02d:%02d", t0 / 60 % 24, t0 % 60); snprintf(insideT, 8, "%02d:%02d", (t0 + 1) / 60 % 24, (t0 + 1) % 60);
    g_webServer.args = {{dayA,"1"},{"on",""},{"off",offA}}; handleProgramSet(); CK(g_ruleCount == 1 && g_rules[0].onMinute == SCHEDULE_NO_TIME);
    g_webServer.args = {{dayA,"1"},{"on",""},{"off",offA}}; handleProgramSet(); CK(g_ruleCount == 1);                  // same minute: refused
    g_webServer.args = {{dayA,"1"},{"on",""},{"off",""}}; handleProgramSet(); CK(g_ruleCount == 1);                    // nothing filled in
    g_webServer.args = {{dayA,"1"},{"on","00:00"},{"off","23:59"}}; handleProgramSet(); CK(g_ruleCount == 1);          // a block around it: refused
    handleProgramPage(); CK(g_webServer.lastBody.find("&ndash;") != std::string::npos);
    loadProgram(); CK(g_ruleCount == 1 && g_rules[0].onMinute == SCHEDULE_NO_TIME);                                      // stored in flash
    while (queued()) pop();
    g_registersRead = true; g_regControl[0] = 0; g_bleConnected = true; publishSnapshot();                             // fireplace is off
    scheduleReset(g_scheduleState); run(1); run(130); CK(queued() == 0);                                               // moment passes: nothing sent
    CK(g_programPending.action == SCHED_NONE);
    g_regControl[0] = 6; publishSnapshot();                                                                            // fireplace is on
    uint32_t k2; int w2; localMinuteKey(k2, w2); int t1 = (int)(k2 % 1440) + 2; char offB[8]; snprintf(offB, 8, "%02d:%02d", t1 / 60 % 24, t1 % 60);
    g_rules[0].offMinute = (uint16_t) t1; scheduleReset(g_scheduleState); run(1); run(130);
    { bool off = false; while (queued()) { Command x = pop(); if (x.type == CMD_FLAME_EFFECT && x.argument == 0 && x.source == SOURCE_SCHEDULE) off = true; } CK(off); } }
  g_ruleCount = 0;
  printf("harness: %d failed\n", fails); return fails;
}
