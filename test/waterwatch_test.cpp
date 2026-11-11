// Tests for Dimplex-Control/waterwatch.h (plain C++, no Arduino needed).
// Build and run:  g++ -std=c++17 -Wall -Wextra -I Dimplex-Control test/waterwatch_test.cpp -o /tmp/waterwatch_test && /tmp/waterwatch_test
#include <cstdio>
#include "waterwatch.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static void reg(uint8_t out[3], int value) { out[0] = 0x01; out[1] = value & 0xff; out[2] = value >> 8; }

// Feed one reading at time t (ms).
static bool feed(WaterWatch &w, bool effectOn, int value, uint32_t t) { uint8_t r[3]; reg(r, value); return waterWatchUpdate(w, effectOn, r, t); }

int main() {
  // --- readings ---
  uint8_t r[3];
  reg(r, 1050); CHECK(waterReading(r) == 1050);
  reg(r, 3);    CHECK(waterReading(r) == 3);
  reg(r, 0);    CHECK(waterReading(r) == 0);
  reg(r, 0xffff); CHECK(waterReading(r) == -1);
  r[0] = 0x00; r[1] = 0x10; r[2] = 0x04; CHECK(waterReading(r) == -1);       // does not start with 01

  // --- effect on with water: never low water ---
  WaterWatch w; waterWatchReset(w);
  for (uint32_t t = 0; t < 120000; t += 5000) CHECK(!feed(w, true, 1000 + (int)(t / 1000 % 80), t));

  // --- the test of 10 Nov (15:30 UTC): dry at :03, :08, :19, :23, :29, wet :33 :39, dry :43 :50, wet from :54 ---
  waterWatchReset(w);
  uint32_t t0 = 100000;                                                        // effect has been on for a long time
  feed(w, true, 1060, t0 - 20000);
  CHECK(!feed(w, true, 3, t0 + 0));                                            // first dry reading
  CHECK(!feed(w, true, 3, t0 + 5000));                                         // 2nd: held 5 s, not yet
  CHECK(feed(w, true, 3, t0 + 10000));                                         // held 10 s: low water
  CHECK(feed(w, true, 3, t0 + 20000));
  CHECK(feed(w, true, 1053, t0 + 30000));                                      // wet again, but not for long enough
  CHECK(feed(w, true, 1042, t0 + 36000));
  CHECK(feed(w, true, 3, t0 + 40000));                                         // lifted out again: still low water
  CHECK(feed(w, true, 3, t0 + 47000));
  CHECK(feed(w, true, 1062, t0 + 51000));                                      // wet from here
  CHECK(feed(w, true, 1056, t0 + 56000));
  CHECK(feed(w, true, 1083, t0 + 61000));
  CHECK(feed(w, true, 1064, t0 + 65000));                                      // 14 s wet: not yet
  CHECK(!feed(w, true, 1051, t0 + 66000));                                     // 15 s wet: cleared

  // --- a single odd reading does nothing ---
  waterWatchReset(w); feed(w, true, 1050, 0);
  CHECK(!feed(w, true, 1050, 30000)); CHECK(!feed(w, true, 3, 35000)); CHECK(!feed(w, true, 1050, 40000)); CHECK(!feed(w, true, 3, 45000));
  CHECK(!feed(w, true, 1050, 50000));

  // --- invalid readings change nothing: a dry period interrupted by ffff keeps counting ---
  waterWatchReset(w); feed(w, true, 1050, 0);
  CHECK(!feed(w, true, 3, 30000)); CHECK(!feed(w, true, 0xffff, 35000)); CHECK(!feed(w, true, 3, 38000));
  CHECK(feed(w, true, 3, 40000));
  CHECK(feed(w, true, 0xffff, 45000));                                         // stays on through an invalid reading

  // --- grace period after the effect is switched on ---
  waterWatchReset(w);
  CHECK(!feed(w, true, 0, 1000)); CHECK(!feed(w, true, 3, 6000)); CHECK(!feed(w, true, 3, 11000)); CHECK(!feed(w, true, 3, 16000));
  CHECK(!feed(w, true, 3, 21000));                                             // 20 s after start: first counted reading
  CHECK(!feed(w, true, 3, 26000));
  CHECK(feed(w, true, 3, 31000));                                              // and 10 s later: low water

  // --- effect off (standby, or a switch-off by the user): state cleared, and the grace period starts again ---
  CHECK(!feed(w, false, 0, 40000));
  CHECK(!feed(w, true, 3, 50000)); CHECK(!feed(w, true, 3, 60000)); CHECK(!feed(w, true, 3, 69000));   // effect on again: grace
  CHECK(!feed(w, true, 3, 71000)); CHECK(!feed(w, true, 3, 76000));
  CHECK(feed(w, true, 3, 81000));

  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
