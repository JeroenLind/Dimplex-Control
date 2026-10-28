/*
 * Dimplex Control: thermostat types and decision logic.
 *
 * Everything in this file is plain C++ without Arduino, Bluetooth or MQTT calls, so it can be compiled and tested on a PC
 * (see test/thermostat_test.cpp). The sketch (Dimplex-Control.ino) does the actual switching; this file only decides.
 *
 * TEMPERATURE CONTROL IN THE BRIDGE
 *   The fireplace's own thermostat does not work reliably (see docs/experiments.md), so the bridge can regulate itself.
 *   There are three modes:
 *     CONTROL_FIREPLACE  no control in the bridge: the heater is just switched on or off, the fireplace regulates.
 *     CONTROL_EXTERNAL   the bridge regulates on a room temperature that arrives over MQTT (a sensor in Home Assistant).
 *     CONTROL_INTERNAL   the bridge regulates on the sensor inside the fireplace (register 0x0055).
 *   In the two bridge modes the heater is switched off when the temperature reaches the setpoint plus a margin, and on
 *   again when it has dropped to the setpoint minus a margin (hysteresis). The margins are in tenths of a degree.
 *
 * All temperatures in this file are in TENTHS of a degree Celsius (21.3 degrees = 213).
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

// Which temperature control is used. The numbers are stored in flash (key "ctlmode"), do not renumber.
enum ControlMode : uint8_t { CONTROL_FIREPLACE = 0, CONTROL_EXTERNAL = 1, CONTROL_INTERNAL = 2 };

// What the thermostat is doing (only meaningful in the two bridge modes).
enum ThermostatState : uint8_t {
  THERMO_INACTIVE = 0,    // the user has not asked for heating (or the fireplace is off)
  THERMO_HEATING,         // the heater should be on
  THERMO_SATISFIED,       // temperature reached: the heater is off until the temperature has dropped enough
  THERMO_NO_SENSOR        // fail-safe: no (recent) room temperature: the heater is off until a new value arrives
};

// Thresholds and time-out, set on the Settings page and stored in flash.
struct ThermostatSettings {
  uint8_t externalOffAbove, externalOnBelow;   // room sensor: heater off at setpoint + this, on again at setpoint - that
  uint8_t internalOffAbove, internalOnBelow;   // the same for the sensor inside the fireplace
  uint16_t timeoutMinutes;                     // room temperature older than this: heater off (fail-safe)
};
// Defaults: room sensor +/- 0.3 degrees; internal sensor off at +1.0 and on at -0.2; time-out 15 minutes.
#define THERMOSTAT_DEFAULT_SETTINGS { 3, 3, 10, 2, 15 }

#define THERMOSTAT_MAX_MARGIN_TENTHS   50     // largest margin that can be set: 5.0 degrees
#define THERMOSTAT_MIN_BAND_TENTHS      2     // off-margin plus on-margin must be at least 0.2 degrees, or the heater would flutter
#define THERMOSTAT_MIN_TIMEOUT_MIN      1
#define THERMOSTAT_MAX_TIMEOUT_MIN    240
#define ROOM_TEMPERATURE_MIN_TENTHS  (-300)   // values outside -30 .. +60 degrees are rejected as nonsense
#define ROOM_TEMPERATURE_MAX_TENTHS    600

// Name of a mode, as shown on the web page and in Home Assistant.
static inline const char *controlModeText(uint8_t mode) {
  switch (mode) {
    case CONTROL_EXTERNAL: return "Room sensor (MQTT)";
    case CONTROL_INTERNAL: return "Internal sensor";
    default:               return "Fireplace thermostat";
  }
}
// Text for the thermostat state, as shown in Home Assistant (mode CONTROL_FIREPLACE: the bridge does not regulate).
static inline const char *thermostatStateText(uint8_t mode, uint8_t state) {
  if (mode == CONTROL_FIREPLACE) return "not used";
  switch (state) {
    case THERMO_HEATING:   return "heating";
    case THERMO_SATISFIED: return "target reached";
    case THERMO_NO_SENSOR: return "no room temperature";
    default:               return "off";
  }
}

// True when the five values may be saved: margins 0 to 5.0 degrees, at least 0.2 degrees of band, time-out 1 to 240 minutes.
static inline bool thermostatSettingsValid(int externalOff, int externalOn, int internalOff, int internalOn, int timeoutMinutes) {
  auto marginOk = [](int tenths) { return tenths >= 0 && tenths <= THERMOSTAT_MAX_MARGIN_TENTHS; };
  return marginOk(externalOff) && marginOk(externalOn) && marginOk(internalOff) && marginOk(internalOn) &&
         externalOff + externalOn >= THERMOSTAT_MIN_BAND_TENTHS && internalOff + internalOn >= THERMOSTAT_MIN_BAND_TENTHS &&
         timeoutMinutes >= THERMOSTAT_MIN_TIMEOUT_MIN && timeoutMinutes <= THERMOSTAT_MAX_TIMEOUT_MIN;
}

// Turn the text of an MQTT message such as "21.3" into tenths of a degree (213). Returns false for anything that is not a plain
// number: "unavailable", "unknown", an empty text, "21,5", "21.5 C" and values outside the plausible range. A rejected
// value must not count as a fresh reading.
static inline bool parseTemperatureTenths(const char *text, int &tenths) {
  char *end = nullptr;
  double value = strtod(text, &end);
  if (end == text) return false;                         // no number at all
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
  if (*end != 0) return false;                           // something after the number
  if (!(value * 10.0 >= ROOM_TEMPERATURE_MIN_TENTHS && value * 10.0 <= ROOM_TEMPERATURE_MAX_TENTHS)) return false;   // also rejects NaN
  tenths = (int) lround(value * 10.0);
  return true;
}

// What decideThermostat() needs to know.
struct ThermostatInput {
  ControlMode mode;                // CONTROL_EXTERNAL or CONTROL_INTERNAL
  ThermostatState previousState;   // the state after the previous decision (gives the hysteresis its memory)
  bool startRule;                  // the user has just switched the heating on (or the mode changed)
  bool temperatureFresh;           // there is a usable temperature (room sensor: recent enough; internal: read successfully)
  int temperatureTenths;           // that temperature
  int setpointDegrees;             // setpoint of the bridge in whole degrees
  ThermostatSettings settings;
};
// What the thermostat decided.
struct ThermostatDecision {
  ThermostatState state;           // THERMO_HEATING, THERMO_SATISFIED or THERMO_NO_SENSOR
  bool heaterOn;                   // should the heater be on?
  int offAboveTenths, onBelowTenths;   // the two switching points that apply (for display)
};

// The hysteresis, in one place.
//   - No usable temperature: the heater must be off (fail-safe). Only the room sensor gets here; the caller does not ask
//     when the internal sensor could not be read for a moment.
//   - Start (the user switches on, the mode changed, or the temperature comes back after a time-out): heat while the
//     temperature is below the setpoint.
//   - While heating: keep heating until the temperature reaches setpoint + off-margin.
//   - While satisfied: stay off until the temperature has dropped to setpoint - on-margin.
static inline ThermostatDecision decideThermostat(const ThermostatInput &in) {
  bool external = in.mode == CONTROL_EXTERNAL;
  int setpointTenths = in.setpointDegrees * 10;
  ThermostatDecision d;
  d.offAboveTenths = setpointTenths + (external ? in.settings.externalOffAbove : in.settings.internalOffAbove);
  d.onBelowTenths  = setpointTenths - (external ? in.settings.externalOnBelow  : in.settings.internalOnBelow);
  if (!in.temperatureFresh) { d.state = THERMO_NO_SENSOR; d.heaterOn = false; return d; }
  bool heat;
  if (in.startRule || in.previousState == THERMO_NO_SENSOR) heat = in.temperatureTenths < setpointTenths;
  else if (in.previousState == THERMO_SATISFIED)            heat = in.temperatureTenths <= d.onBelowTenths;
  else                                                      heat = in.temperatureTenths < d.offAboveTenths;   // heating, or a running heater that was just taken over
  d.state = heat ? THERMO_HEATING : THERMO_SATISFIED;
  d.heaterOn = heat;
  return d;
}
