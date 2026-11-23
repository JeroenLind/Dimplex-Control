/*
 * Dimplex Control: weekly program.
 *
 * Plain C++ without Arduino, Bluetooth or MQTT calls, so it can be compiled and tested on a PC (test/schedule_test.cpp).
 * Design: docs/design.md section 5.7.
 *
 * A rule has weekdays (bit 0 = Monday ... bit 6 = Sunday), an on time and an off time in minutes since midnight. When the off time
 * is not later than the on time the rule crosses midnight: it is on until the off time of the NEXT day.
 * A rule with onMinute == SCHEDULE_NO_TIME is an "off only" rule: at its off time everything is switched off, but only when the
 * fireplace is on (SCHED_OFF_IF_ON). It occupies that single minute in the overlap check.
 */
#pragma once
#include <stdint.h>
#include <string.h>

#define SCHEDULE_MAX_RULES   20
#define SCHEDULE_MAX_GAP     10          // handle at most this many missed minutes (a bigger jump is a clock change: no catch-up)
#define SCHEDULE_NO_TIME     0xFFFF      // onMinute of an "off only" rule
#define SCHEDULE_WEEK_MIN    10080
#define SCHEDULE_RECORD_SIZE 6           // bytes per rule in flash
#define SCHEDULE_BLOB_SIZE   (1 + SCHEDULE_MAX_RULES * SCHEDULE_RECORD_SIZE)

struct ScheduleRule { uint8_t days; uint16_t onMinute; uint16_t offMinute; bool enabled; };

enum ScheduleAction : uint8_t { SCHED_NONE = 0, SCHED_ON = 1, SCHED_OFF = 2, SCHED_OFF_IF_ON = 3 };
enum RuleCheck : uint8_t { RULE_OK, RULE_NO_DAY, RULE_BAD_TIME, RULE_SAME_TIME, RULE_OVERLAP, RULE_FULL };

static inline const char *ruleCheckText(RuleCheck r) {
  switch (r) {
    case RULE_OK:        return "ok";
    case RULE_NO_DAY:    return "choose at least one day";
    case RULE_BAD_TIME:  return "invalid time";
    case RULE_SAME_TIME: return "on time and off time are the same";
    case RULE_OVERLAP:   return "overlaps with another rule";
    case RULE_FULL:      return "the list is full";
  }
  return "?";
}

static inline bool scheduleOffOnly(const ScheduleRule &r) { return r.onMinute == SCHEDULE_NO_TIME; }
// Length in minutes of one switched-on period of the rule (1 to 1439); 0 for an off-only rule (one single minute).
static inline int scheduleRuleLength(const ScheduleRule &r) {
  if (scheduleOffOnly(r)) return 0;
  return r.offMinute > r.onMinute ? r.offMinute - r.onMinute : r.offMinute + 1440 - r.onMinute;
}
// First minute a rule occupies: its on minute, or for an off-only rule its off minute.
static inline int scheduleRuleStart(const ScheduleRule &r) { return scheduleOffOnly(r) ? r.offMinute : r.onMinute; }

// Mark every minute a rule occupies in a week, both its on minute and its off minute included (so rules that touch conflict).
typedef uint8_t ScheduleWeek[(SCHEDULE_WEEK_MIN + 7) / 8];
static inline void scheduleMark(ScheduleWeek w, const ScheduleRule &r) {
  int len = scheduleRuleLength(r);
  for (int d = 0; d < 7; d++) {
    if (!(r.days & (1 << d))) continue;
    for (int m = 0; m <= len; m++) { int i = (d * 1440 + scheduleRuleStart(r) + m) % SCHEDULE_WEEK_MIN; w[i >> 3] |= (uint8_t)(1 << (i & 7)); }
  }
}
static inline bool scheduleOverlaps(const ScheduleWeek w, const ScheduleRule &r) {
  int len = scheduleRuleLength(r);
  for (int d = 0; d < 7; d++) {
    if (!(r.days & (1 << d))) continue;
    for (int m = 0; m <= len; m++) { int i = (d * 1440 + scheduleRuleStart(r) + m) % SCHEDULE_WEEK_MIN; if (w[i >> 3] & (1 << (i & 7))) return true; }
  }
  return false;
}

// Is the rule valid on its own, and does it fit with the other rules? skip = index of the rule being replaced (-1 for a new rule).
static inline RuleCheck scheduleCheckRule(const ScheduleRule *rules, int count, const ScheduleRule &cand, int skip) {
  if ((cand.days & 0x7f) == 0 || (cand.days & 0x80)) return (cand.days & 0x80) ? RULE_BAD_TIME : RULE_NO_DAY;
  if ((cand.onMinute > 1439 && !scheduleOffOnly(cand)) || cand.offMinute > 1439) return RULE_BAD_TIME;
  if (!scheduleOffOnly(cand) && cand.onMinute == cand.offMinute) return RULE_SAME_TIME;
  if (skip < 0 && count >= SCHEDULE_MAX_RULES) return RULE_FULL;
  ScheduleWeek w; memset(w, 0, sizeof(w));
  for (int i = 0; i < count; i++) if (i != skip) scheduleMark(w, rules[i]);     // disabled rules count too
  return scheduleOverlaps(w, cand) ? RULE_OVERLAP : RULE_OK;
}

// --- flash format: byte 0 = count, then per rule: days, on lo, on hi, off lo, off hi, enabled ---
static inline void scheduleEncode(const ScheduleRule *rules, int count, uint8_t out[SCHEDULE_BLOB_SIZE]) {
  memset(out, 0, SCHEDULE_BLOB_SIZE); out[0] = (uint8_t) count;
  for (int i = 0; i < count; i++) {
    uint8_t *p = out + 1 + i * SCHEDULE_RECORD_SIZE;
    p[0] = rules[i].days; p[1] = rules[i].onMinute & 0xff; p[2] = rules[i].onMinute >> 8;
    p[3] = rules[i].offMinute & 0xff; p[4] = rules[i].offMinute >> 8; p[5] = rules[i].enabled ? 1 : 0;
  }
}
// Returns the number of rules read; a rule that is not valid, or conflicts with an earlier one, ends the list (damaged flash).
static inline int scheduleDecode(const uint8_t *blob, size_t len, ScheduleRule *rules) {
  if (len < 1) return 0;
  int n = blob[0]; if (n > SCHEDULE_MAX_RULES || len < (size_t)(1 + n * SCHEDULE_RECORD_SIZE)) return 0;
  int count = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t *p = blob + 1 + i * SCHEDULE_RECORD_SIZE;
    ScheduleRule r = { p[0], (uint16_t)(p[1] | (p[2] << 8)), (uint16_t)(p[3] | (p[4] << 8)), p[5] != 0 };
    if (scheduleCheckRule(rules, count, r, -1) != RULE_OK) break;
    rules[count++] = r;
  }
  return count;
}

// --- deciding what is due ---
// A "minute key" is (local days since a fixed date) * 1440 + minute of the local day. It runs back in the repeated hour of the
// autumn clock change (that hour is then not executed twice) and jumps forward in the skipped hour of spring (that hour is skipped).
// Days since 1970-01-01 of a calendar date (month 1 to 12). Used to build the minute key from local time fields.
static inline uint32_t scheduleDayNumber(int year, int month, int day) {
  int y = year - (month <= 2); int era = (y >= 0 ? y : y - 399) / 400; int yoe = y - era * 400;
  int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1; int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t)(era * 146097 + doe - 719468);
}
struct ScheduleState { bool started; uint32_t lastKey; };

static inline void scheduleReset(ScheduleState &s) { s.started = false; s.lastKey = 0; }

static inline ScheduleAction scheduleAtMinute(const ScheduleRule *rules, int count, int weekday, int minute) {
  ScheduleAction a = SCHED_NONE;
  for (int i = 0; i < count; i++) {
    const ScheduleRule &r = rules[i];
    if (!r.enabled) continue;
    if ((r.days & (1 << weekday)) && r.onMinute == minute) a = SCHED_ON;
    int offDay = (scheduleOffOnly(r) || r.offMinute > r.onMinute) ? weekday : (weekday + 6) % 7;   // a rule over midnight ends the day after its day
    if ((r.days & (1 << offDay)) && r.offMinute == minute) a = scheduleOffOnly(r) ? SCHED_OFF_IF_ON : SCHED_OFF;
  }
  return a;
}

// Call about once a second with the local time. weekday: 0 = Monday. Returns the action to carry out now (the last one when several
// minutes were missed). The first call only stores the time: nothing is caught up after a restart.
static inline ScheduleAction scheduleDue(const ScheduleRule *rules, int count, ScheduleState &s, uint32_t nowKey, int nowWeekday) {
  if (!s.started) { s.started = true; s.lastKey = nowKey; return SCHED_NONE; }
  if (nowKey <= s.lastKey) return SCHED_NONE;                                          // same minute, or the repeated autumn hour
  uint32_t first = nowKey;
  if (nowKey - s.lastKey <= SCHEDULE_MAX_GAP) first = s.lastKey + 1;                   // a short stall: handle every minute that passed
  ScheduleAction result = SCHED_NONE;
  for (uint32_t k = first; k <= nowKey; k++) {
    int back = (int)((nowKey / 1440 - k / 1440) % 7);
    ScheduleAction a = scheduleAtMinute(rules, count, (nowWeekday - back + 7) % 7, (int)(k % 1440));
    if (a != SCHED_NONE) result = a;
  }
  s.lastKey = nowKey;
  return result;
}
