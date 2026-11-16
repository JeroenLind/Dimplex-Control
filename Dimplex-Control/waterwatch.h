/*
 * Dimplex Control: low water detection.
 *
 * Plain C++ without Arduino, Bluetooth or MQTT calls, so it can be compiled and tested on a PC (test/waterwatch_test.cpp).
 *
 * WHAT IS MEASURED
 *   Register 0x003c (`01` + LE16) is, as far as we know, a measurement of the load on the ultrasonic mist maker (see
 *   docs/experiments.md sections 9 and 10). With the disc under water and the effect running it reads about 950 to 1150 (it climbs
 *   from ~990 to ~1130 in the first 2 to 3 hours). With the disc out of the water (empty tank, or lifted out by hand) it falls to
 *   2 or 3 in one step. Other values:
 *     0x0000 and 0xffff   no measurement: they alternate in standby and during the ~50 s start-up of the effect (while 0x0063 is
 *                         still 0). They are NOT dry readings.
 *   The fireplace switches the effect off by itself when it is dry (0x0063 goes to 0 within ~10 s). Byte 2 of 0x0040 follows only
 *   after about 70 s, so 0x003c is the signal to use.
 *
 * WHEN IS THERE "LOW WATER"
 *   The reading must be below WATER_DRY_BELOW for WATER_DRY_HOLD_MS (and at least 2 readings) to switch low water on, and at or
 *   above it for WATER_WET_HOLD_MS (and at least 2 readings) to switch it off again. A single odd reading, a bubble on the disc or
 *   a short lift while the disc is put back therefore does not flip the sensor. Readings are ignored (neither dry nor wet) while:
 *     - the flame effect is off (and the state is cleared: without the effect nothing can be measured),
 *     - the effect has been on for less than WATER_GRACE_MS (start-up),
 *     - the reading is 0 or 0xffff (no measurement) or does not start with 0x01.
 */
#pragma once
#include <stdint.h>

#define WATER_DRY_BELOW     500      // reading below this = disc dry (dry reads 3, wet reads 1000 to 1100)
#define WATER_DRY_HOLD_MS   10000    // dry for this long before "low water" is reported
#define WATER_WET_HOLD_MS   15000    // wet for this long before it is cleared (longer: a badly seated disc may flicker)
#define WATER_GRACE_MS      20000    // ignore readings this long after the effect has been switched on

struct WaterWatch {
  bool lowWater;
  uint8_t candidate;                 // 0 none, 1 dry, 2 wet: what the last readings say
  uint8_t candidateCount;            // consecutive readings of that kind
  uint32_t candidateSinceMs;         // when the first of them was read
  bool effectWasOn;
  uint32_t effectOnSinceMs;
};

static inline void waterWatchReset(WaterWatch &w) {
  w.lowWater = false; w.candidate = 0; w.candidateCount = 0; w.candidateSinceMs = 0; w.effectWasOn = false; w.effectOnSinceMs = 0;
}

// Reading of 0x003c as a number, or -1 when it is not a usable measurement.
static inline int waterReading(const uint8_t reg[3]) {
  if (reg[0] != 0x01) return -1;
  int v = reg[1] | (reg[2] << 8);
  return (v == 0xffff || v == 0) ? -1 : v;               // 0 and 0xffff alternate when nothing is measured (standby, effect start-up)
}

// Call once for every successful read of 0x003c. effectOn = the flame effect is switched on (0x0040 byte 0 is not 0).
// Returns the (possibly changed) low water state.
static inline bool waterWatchUpdate(WaterWatch &w, bool effectOn, const uint8_t reg3c[3], uint32_t nowMs) {
  if (!effectOn) {                                   // standby: nothing can be measured, so nothing can be reported
    w.lowWater = false; w.candidate = 0; w.candidateCount = 0; w.effectWasOn = false;
    return w.lowWater;
  }
  if (!w.effectWasOn) { w.effectWasOn = true; w.effectOnSinceMs = nowMs; w.candidate = 0; w.candidateCount = 0; }
  if (nowMs - w.effectOnSinceMs < WATER_GRACE_MS) return w.lowWater;    // the effect is still starting
  int v = waterReading(reg3c);
  if (v < 0) return w.lowWater;                      // not a measurement: change nothing
  uint8_t kind = v < WATER_DRY_BELOW ? 1 : 2;
  if (kind != w.candidate) { w.candidate = kind; w.candidateCount = 1; w.candidateSinceMs = nowMs; }
  else if (w.candidateCount < 255) w.candidateCount++;
  if (w.candidateCount >= 2) {
    if (kind == 1 && !w.lowWater && nowMs - w.candidateSinceMs >= WATER_DRY_HOLD_MS) w.lowWater = true;
    if (kind == 2 && w.lowWater && nowMs - w.candidateSinceMs >= WATER_WET_HOLD_MS) w.lowWater = false;
  }
  return w.lowWater;
}
