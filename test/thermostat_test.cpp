// Tests for Dimplex-Control/thermostat.h (plain C++, no Arduino needed).
// Build and run:  g++ -std=c++17 -Wall -Wextra -I Dimplex-Control test/thermostat_test.cpp -o /tmp/thermostat_test && /tmp/thermostat_test
#include <cstdio>
#include "thermostat.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static ThermostatInput make(ControlMode mode, ThermostatState prev, bool start, bool fresh, int tenths, int setpoint) {
  ThermostatInput in = { mode, prev, start, fresh, tenths, setpoint, THERMOSTAT_DEFAULT_SETTINGS };
  return in;
}

int main() {
  // --- room sensor (default margins +0.3 / -0.3, setpoint 21) ---
  CHECK(decideThermostat(make(CONTROL_EXTERNAL, THERMO_INACTIVE, true, true, 200, 21)).heaterOn);          // start below setpoint: heat
  CHECK(!decideThermostat(make(CONTROL_EXTERNAL, THERMO_INACTIVE, true, true, 210, 21)).heaterOn);         // start at setpoint: stay off
  CHECK(decideThermostat(make(CONTROL_EXTERNAL, THERMO_HEATING, false, true, 212, 21)).heaterOn);          // heating, below 21.3
  CHECK(!decideThermostat(make(CONTROL_EXTERNAL, THERMO_HEATING, false, true, 213, 21)).heaterOn);         // reaches 21.3: off
  CHECK(!decideThermostat(make(CONTROL_EXTERNAL, THERMO_SATISFIED, false, true, 208, 21)).heaterOn);       // satisfied, 20.8 > 20.7: stay off
  CHECK(decideThermostat(make(CONTROL_EXTERNAL, THERMO_SATISFIED, false, true, 207, 21)).heaterOn);        // drops to 20.7: on again
  ThermostatDecision d = decideThermostat(make(CONTROL_EXTERNAL, THERMO_HEATING, false, true, 200, 21));
  CHECK(d.offAboveTenths == 213 && d.onBelowTenths == 207);

  // --- internal sensor (+1.0 / -0.2): off at 22.0, on again at 20.8 ---
  CHECK(decideThermostat(make(CONTROL_INTERNAL, THERMO_HEATING, false, true, 219, 21)).heaterOn);
  CHECK(!decideThermostat(make(CONTROL_INTERNAL, THERMO_HEATING, false, true, 220, 21)).heaterOn);
  CHECK(!decideThermostat(make(CONTROL_INTERNAL, THERMO_SATISFIED, false, true, 209, 21)).heaterOn);
  CHECK(decideThermostat(make(CONTROL_INTERNAL, THERMO_SATISFIED, false, true, 208, 21)).heaterOn);

  // --- fail-safe: no usable temperature means heater off, and recovery uses the start rule ---
  d = decideThermostat(make(CONTROL_EXTERNAL, THERMO_HEATING, false, false, 0, 21));
  CHECK(!d.heaterOn && d.state == THERMO_NO_SENSOR);
  d = decideThermostat(make(CONTROL_EXTERNAL, THERMO_NO_SENSOR, false, true, 205, 21));
  CHECK(d.heaterOn && d.state == THERMO_HEATING);                                                          // value is back, below setpoint
  d = decideThermostat(make(CONTROL_EXTERNAL, THERMO_NO_SENSOR, false, true, 215, 21));
  CHECK(!d.heaterOn && d.state == THERMO_SATISFIED);                                                       // value is back, above setpoint

  // --- settings validation ---
  CHECK(thermostatSettingsValid(3, 3, 10, 2, 15));
  CHECK(!thermostatSettingsValid(0, 1, 10, 2, 15));     // band of 0.1 only
  CHECK(!thermostatSettingsValid(51, 3, 10, 2, 15));    // margin above 5.0
  CHECK(!thermostatSettingsValid(3, 3, 10, 2, 0));      // time-out too short
  CHECK(!thermostatSettingsValid(3, 3, 10, 2, 241));    // time-out too long

  // --- parsing of MQTT payloads ---
  int t = 0;
  CHECK(parseTemperatureTenths("21.3", t) && t == 213);
  CHECK(parseTemperatureTenths(" 19 \n", t) && t == 190);
  CHECK(parseTemperatureTenths("-5.5", t) && t == -55);
  CHECK(!parseTemperatureTenths("unavailable", t));
  CHECK(!parseTemperatureTenths("unknown", t));
  CHECK(!parseTemperatureTenths("", t));
  CHECK(!parseTemperatureTenths("21,5", t));
  CHECK(!parseTemperatureTenths("21.5 C", t));
  CHECK(!parseTemperatureTenths("nan", t));
  CHECK(!parseTemperatureTenths("999", t));

  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
