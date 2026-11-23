// Tests for Dimplex-Control/schedule.h (plain C++, no Arduino needed).
// Build and run:  g++ -std=c++17 -Wall -Wextra -I Dimplex-Control test/schedule_test.cpp -o /tmp/schedule_test && /tmp/schedule_test
#include <cstdio>
#include "schedule.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

enum { MON = 1, TUE = 2, WED = 4, THU = 8, FRI = 16, SAT = 32, SUN = 64 };
static ScheduleRule rule(int days, int onH, int onM, int offH, int offM, bool en = true) { ScheduleRule r = { (uint8_t) days, (uint16_t)(onH * 60 + onM), (uint16_t)(offH * 60 + offM), en }; return r; }
// Minute key for day number `day` (0 = a Monday) at hh:mm; weekday = day % 7.
static uint32_t key(int day, int h, int m) { return (uint32_t) day * 1440 + h * 60 + m; }
static ScheduleAction due(const ScheduleRule *r, int n, ScheduleState &s, int day, int h, int m) { return scheduleDue(r, n, s, key(day, h, m), day % 7); }

int main() {
  // --- validation ---
  ScheduleRule list[SCHEDULE_MAX_RULES]; int n = 0;
  list[n++] = rule(MON | TUE, 7, 0, 9, 0);
  CHECK(scheduleCheckRule(list, n, rule(MON, 18, 0, 22, 0), -1) == RULE_OK);
  CHECK(scheduleCheckRule(list, n, rule(0, 18, 0, 22, 0), -1) == RULE_NO_DAY);
  CHECK(scheduleCheckRule(list, n, rule(MON, 18, 0, 18, 0), -1) == RULE_SAME_TIME);
  { ScheduleRule bad = rule(MON, 18, 0, 22, 0); bad.onMinute = 1440; CHECK(scheduleCheckRule(list, n, bad, -1) == RULE_BAD_TIME); }
  CHECK(scheduleCheckRule(list, n, rule(MON, 8, 0, 10, 0), -1) == RULE_OVERLAP);               // overlaps 07:00 to 09:00
  CHECK(scheduleCheckRule(list, n, rule(TUE, 6, 0, 7, 30), -1) == RULE_OVERLAP);
  CHECK(scheduleCheckRule(list, n, rule(MON, 9, 0, 10, 0), -1) == RULE_OVERLAP);               // touching: off 09:00 = on 09:00
  CHECK(scheduleCheckRule(list, n, rule(MON, 6, 0, 7, 0), -1) == RULE_OVERLAP);                // touching the other way
  CHECK(scheduleCheckRule(list, n, rule(MON, 9, 1, 10, 0), -1) == RULE_OK);
  CHECK(scheduleCheckRule(list, n, rule(WED, 7, 0, 9, 0), -1) == RULE_OK);                     // other day, same times
  CHECK(scheduleCheckRule(list, n, rule(MON | TUE, 7, 0, 9, 0), 0) == RULE_OK);                // replacing rule 0 by itself
  // midnight crossing and the week wrap
  list[0] = rule(SUN, 22, 0, 1, 0);                                                            // Sunday 22:00 to Monday 01:00
  CHECK(scheduleCheckRule(list, 1, rule(MON, 0, 30, 2, 0), -1) == RULE_OVERLAP);
  CHECK(scheduleCheckRule(list, 1, rule(MON, 1, 1, 2, 0), -1) == RULE_OK);
  CHECK(scheduleCheckRule(list, 1, rule(SUN, 21, 0, 22, 0), -1) == RULE_OVERLAP);
  CHECK(scheduleCheckRule(list, 1, rule(SAT, 23, 0, 23, 59), -1) == RULE_OK);
  list[0] = rule(MON, 20, 0, 6, 0, false);                                                     // a disabled rule still reserves its time
  CHECK(scheduleCheckRule(list, 1, rule(TUE, 5, 0, 7, 0), -1) == RULE_OVERLAP);
  // full list
  n = 0; for (int i = 0; i < SCHEDULE_MAX_RULES; i++) list[n++] = rule(1 << (i % 7), (i / 7) * 8, 0, (i / 7) * 8 + 1, 0);
  CHECK(scheduleCheckRule(list, n, rule(MON, 3, 0, 3, 30), -1) == RULE_FULL);
  CHECK(scheduleCheckRule(list, n, rule(MON, 3, 0, 3, 30), 0) == RULE_OK);

  // --- off-only rules: no on time, one single minute ---
  { ScheduleRule night = rule(MON | TUE, 0, 0, 23, 0); night.onMinute = SCHEDULE_NO_TIME;                                // Monday and Tuesday 23:00 off
    ScheduleRule l2[3]; int c2 = 0; l2[c2++] = rule(MON, 18, 0, 22, 0);
    CHECK(scheduleCheckRule(l2, c2, night, -1) == RULE_OK);
    CHECK(scheduleCheckRule(l2, c2, rule(MON, 22, 1, 23, 0), -1) == RULE_OK);
    l2[c2++] = night;
    CHECK(scheduleCheckRule(l2, c2, night, -1) == RULE_OVERLAP);                                                         // same minute twice
    CHECK(scheduleCheckRule(l2, c2, rule(MON, 22, 1, 23, 0), -1) == RULE_OVERLAP);                                       // touches the off-only minute
    CHECK(scheduleCheckRule(l2, c2, rule(MON, 22, 1, 22, 59), -1) == RULE_OK);
    CHECK(scheduleCheckRule(l2, c2, rule(MON, 22, 0, 1, 0), -1) == RULE_OVERLAP);                                        // block over midnight around it
    { ScheduleRule inside = night; inside.offMinute = 19 * 60; CHECK(scheduleCheckRule(l2, 1, inside, -1) == RULE_OVERLAP); }   // inside 18:00-22:00
    { ScheduleRule bad = night; bad.offMinute = 1440; CHECK(scheduleCheckRule(l2, 1, bad, -1) == RULE_BAD_TIME); }
    { ScheduleRule noDay = night; noDay.days = 0; CHECK(scheduleCheckRule(l2, 1, noDay, -1) == RULE_NO_DAY); }
    // what is due: off-if-on on its own days only (not the day after), nothing at other times
    ScheduleState st; scheduleReset(st);
    CHECK(due(&night, 1, st, 0, 22, 59) == SCHED_NONE);
    CHECK(due(&night, 1, st, 0, 23, 0) == SCHED_OFF_IF_ON);                                                              // Monday
    CHECK(due(&night, 1, st, 1, 23, 0) == SCHED_OFF_IF_ON);                                                              // Tuesday
    CHECK(due(&night, 1, st, 2, 23, 0) == SCHED_NONE);                                                                   // Wednesday: not a day of the rule
    { ScheduleRule early = night; early.offMinute = 30; scheduleReset(st); due(&early, 1, st, 1, 0, 29); CHECK(due(&early, 1, st, 1, 0, 30) == SCHED_OFF_IF_ON);
      scheduleReset(st); due(&early, 1, st, 2, 0, 29); CHECK(due(&early, 1, st, 2, 0, 30) == SCHED_NONE); }               // not the day after the rule's day
    { ScheduleRule off = night; off.enabled = false; scheduleReset(st); due(&off, 1, st, 0, 22, 59); CHECK(due(&off, 1, st, 0, 23, 0) == SCHED_NONE); }
    // flash format
    uint8_t blob[SCHEDULE_BLOB_SIZE]; ScheduleRule out[SCHEDULE_MAX_RULES];
    scheduleEncode(l2, c2, blob); CHECK(scheduleDecode(blob, sizeof(blob), out) == 2);
    CHECK(out[1].onMinute == SCHEDULE_NO_TIME && out[1].offMinute == 1380 && out[1].days == (MON | TUE));
  }

  // --- flash format ---
  { ScheduleRule a[2] = { rule(MON | FRI, 6, 30, 7, 45), rule(SUN, 22, 0, 1, 0, false) }; uint8_t blob[SCHEDULE_BLOB_SIZE]; ScheduleRule out[SCHEDULE_MAX_RULES];
    scheduleEncode(a, 2, blob); CHECK(scheduleDecode(blob, sizeof(blob), out) == 2);
    CHECK(out[0].days == (MON | FRI) && out[0].onMinute == 390 && out[0].offMinute == 465 && out[0].enabled);
    CHECK(out[1].days == SUN && out[1].onMinute == 1320 && out[1].offMinute == 60 && !out[1].enabled);
    CHECK(scheduleDecode(blob, 5, out) == 0);                                                  // too short
    blob[0] = 99; CHECK(scheduleDecode(blob, sizeof(blob), out) == 0);                         // bad count
    scheduleEncode(a, 2, blob); blob[1 + 6 + 1] = 0x00; blob[1 + 6 + 2] = 0x00; blob[1 + 6 + 3] = 0x00; blob[1 + 6 + 4] = 0x00; // 2nd rule on = off
    CHECK(scheduleDecode(blob, sizeof(blob), out) == 1);                                       // damaged rule ends the list
  }

  // --- due moments ---
  ScheduleRule r1[2] = { rule(MON | WED, 7, 0, 9, 0), rule(FRI, 22, 0, 1, 0) };               // Fri 22:00 to Sat 01:00
  ScheduleState s; scheduleReset(s);
  CHECK(due(r1, 2, s, 0, 6, 59) == SCHED_NONE);                                                // first call stores the time
  CHECK(due(r1, 2, s, 0, 7, 0) == SCHED_ON);
  CHECK(due(r1, 2, s, 0, 7, 0) == SCHED_NONE);                                                 // same minute again: only once
  CHECK(due(r1, 2, s, 0, 7, 1) == SCHED_NONE);
  CHECK(due(r1, 2, s, 0, 9, 0) == SCHED_OFF);
  CHECK(due(r1, 2, s, 1, 7, 0) == SCHED_NONE);                                                 // Tuesday: nothing
  CHECK(due(r1, 2, s, 2, 7, 0) == SCHED_ON);                                                   // Wednesday
  CHECK(due(r1, 2, s, 4, 22, 0) == SCHED_ON);                                                  // Friday 22:00
  CHECK(due(r1, 2, s, 5, 1, 0) == SCHED_OFF);                                                  // Saturday 01:00 ends the Friday rule
  CHECK(due(r1, 2, s, 6, 1, 0) == SCHED_NONE);                                                 // Sunday 01:00: nothing
  CHECK(due(r1, 2, s, 7, 1, 0) == SCHED_NONE);                                                 // Monday 01:00: nothing
  { ScheduleRule dis[1] = { rule(MON, 7, 0, 9, 0, false) }; scheduleReset(s); due(dis, 1, s, 0, 6, 59); CHECK(due(dis, 1, s, 0, 7, 0) == SCHED_NONE); }

  // --- no catch-up after a restart ---
  scheduleReset(s); CHECK(due(r1, 2, s, 0, 8, 0) == SCHED_NONE);                               // started in the middle of a period
  CHECK(due(r1, 2, s, 0, 8, 1) == SCHED_NONE);
  CHECK(due(r1, 2, s, 0, 9, 0) == SCHED_OFF);

  // --- a short stall: all missed minutes are handled, the last action wins ---
  scheduleReset(s); due(r1, 2, s, 0, 6, 55);
  CHECK(due(r1, 2, s, 0, 7, 3) == SCHED_ON);                                                   // 07:00 was within the missed minutes
  CHECK(due(r1, 2, s, 0, 7, 4) == SCHED_NONE);
  { ScheduleRule two[2] = { rule(MON, 7, 0, 7, 2), rule(MON, 7, 3, 8, 0) };
    scheduleReset(s); due(two, 2, s, 0, 6, 59); CHECK(due(two, 2, s, 0, 7, 5) == SCHED_ON); }  // on, off, on: last is on
  // a long jump (a clock change or a stall of more than SCHEDULE_MAX_GAP minutes) does not replay the minutes in between
  scheduleReset(s); due(r1, 2, s, 0, 6, 0); CHECK(due(r1, 2, s, 0, 7, 30) == SCHED_NONE);

  // --- clock changes (local time) ---
  { ScheduleRule night[1] = { rule(SUN, 2, 30, 4, 0) };                                        // 02:30 does not exist on the spring day
    scheduleReset(s); due(night, 1, s, 6, 1, 59); CHECK(due(night, 1, s, 6, 3, 0) == SCHED_NONE);          // 01:59 -> 03:00 (skipped hour)
    CHECK(due(night, 1, s, 6, 4, 0) == SCHED_OFF);
    scheduleReset(s); due(night, 1, s, 6, 2, 29);                                              // autumn: 02:00 to 02:59 is lived twice
    CHECK(due(night, 1, s, 6, 2, 30) == SCHED_ON);
    CHECK(due(night, 1, s, 6, 2, 59) == SCHED_NONE);
    CHECK(due(night, 1, s, 6, 2, 0) == SCHED_NONE);                                            // clock back to 02:00
    CHECK(due(night, 1, s, 6, 2, 30) == SCHED_NONE);                                           // second 02:30: already executed
    CHECK(due(night, 1, s, 6, 3, 0) == SCHED_NONE);
    CHECK(due(night, 1, s, 6, 4, 0) == SCHED_OFF); }

  // --- day numbers ---
  CHECK(scheduleDayNumber(1970, 1, 1) == 0);
  CHECK(scheduleDayNumber(2026, 11, 17) == 20774);
  CHECK(scheduleDayNumber(2026, 3, 1) - scheduleDayNumber(2026, 2, 28) == 1);
  CHECK(scheduleDayNumber(2028, 3, 1) - scheduleDayNumber(2028, 2, 28) == 2);                 // leap year
  CHECK(scheduleDayNumber(2027, 1, 1) - scheduleDayNumber(2026, 12, 31) == 1);

  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
