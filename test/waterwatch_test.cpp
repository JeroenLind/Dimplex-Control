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
  reg(r, 0);    CHECK(waterReading(r) == -1);                                   // 0 is no measurement (standby, start-up)
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
  CHECK(!feed(w, true, 3, 1000)); CHECK(!feed(w, true, 3, 6000)); CHECK(!feed(w, true, 3, 11000)); CHECK(!feed(w, true, 3, 16000));
  CHECK(!feed(w, true, 3, 21000));                                             // 20 s after start: first counted reading
  CHECK(!feed(w, true, 3, 26000));
  CHECK(feed(w, true, 3, 31000));                                              // and 10 s later: low water

  // --- start-up of the effect (12 Nov 16:18:49 and 13 Nov 05:32:34): for about 50 s the register alternates 0 and 0xffff, then it
  //     reads ~950. This used to give a false low water of about 30 s because 0 counted as dry. ---
  waterWatchReset(w);
  const int startup[] = {0xffff, 0xffff, 0, 0, 0xffff, 0, 0, 0xffff, 0xffff, 0, 0xffff, 0, 0xffff, 951, 967, 972, 981, 1000, 977};
  for (int i = 0; i < 19; i++) CHECK(!feed(w, true, startup[i], 1000 + 5000u * i));

  // --- the real run-out of 13 Nov 11:14:42: ~1120 in one step to 3, restart ~50 s after the water is back ---
  waterWatchReset(w); feed(w, true, 1110, 0);
  for (uint32_t t = 25000; t < 60000; t += 5000) CHECK(!feed(w, true, 1100 + (int)(t % 30), t));
  CHECK(!feed(w, true, 3, 60000)); CHECK(!feed(w, true, 3, 65000)); CHECK(feed(w, true, 3, 70000));        // on 10 s after the step
  for (uint32_t t = 75000; t < 600000; t += 5000) CHECK(feed(w, true, 3, t));                                // stays on while dry
  CHECK(feed(w, true, 974, 600000)); CHECK(feed(w, true, 991, 605000)); CHECK(feed(w, true, 983, 610000)); CHECK(!feed(w, true, 989, 615000));

  // --- effect off (standby, or a switch-off by the user): state cleared, and the grace period starts again ---
  CHECK(!feed(w, false, 0, 40000));
  CHECK(!feed(w, true, 3, 50000)); CHECK(!feed(w, true, 3, 60000)); CHECK(!feed(w, true, 3, 69000));   // effect on again: grace
  CHECK(!feed(w, true, 3, 71000)); CHECK(!feed(w, true, 3, 76000));
  CHECK(feed(w, true, 3, 81000));

  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
