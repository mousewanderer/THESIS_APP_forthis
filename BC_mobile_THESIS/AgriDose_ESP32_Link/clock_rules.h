// =====================================================================
//  clock_rules -- when the ESP32 takes a phone's time, and how a time is
//  written. Plain C++ with no Arduino parts, so tests/test_clock.cpp can
//  check it on a laptop. wall_clock.cpp uses it on the ESP32.
//
//  THE RULES (1 Nov 2026)
//    - A time before 2026 or after 2099, or a time zone outside UTC-12..+14,
//      is refused (a phone with a dead clock reports 1970 or 1980).
//    - The ESP32 has no time yet (it just started): ANY phone sets it. The
//      dashboard is open without a login, so the first phone that opens it
//      after a restart gives the machine its time.
//    - The ESP32 already has a time: only a LOGGED-IN phone may change it,
//      and only when it is more than CLOCK_SLACK_S off. So a stray phone
//      with a wrong clock can't move the time once it is set.
//
//  TIME TEXT  "2026-11-01 14:05" (local time of the phone that set the
//  clock), the form used in /dose_log.csv and on the dashboard.
// =====================================================================
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CLOCK_MIN_EPOCH 1767225600LL   // 2026-01-01 00:00 UTC
#define CLOCK_MAX_EPOCH 4102444800LL   // 2100-01-01 00:00 UTC
#define CLOCK_TZ_MIN    (-720L)        // UTC-12:00, in minutes
#define CLOCK_TZ_MAX    840L           // UTC+14:00
#define CLOCK_SLACK_S   120            // a logged-in phone corrects the clock only past this many seconds
#define CLOCK_TEXT_LEN  16             // "YYYY-MM-DD HH:MM"

enum ClockVerdict { CLOCK_BAD, CLOCK_SET, CLOCK_CORRECTED, CLOCK_KEPT };

// isSet / espEpoch: the ESP32's clock now. phoneEpoch: UTC seconds from the
// phone. tzMin: the phone's time zone, minutes east of UTC (+480 = UTC+8).
inline ClockVerdict clockDecide(bool isSet, int64_t espEpoch, int64_t phoneEpoch, long tzMin, bool loggedIn) {
  if (phoneEpoch < CLOCK_MIN_EPOCH || phoneEpoch > CLOCK_MAX_EPOCH) return CLOCK_BAD;
  if (tzMin < CLOCK_TZ_MIN || tzMin > CLOCK_TZ_MAX) return CLOCK_BAD;
  if (!isSet) return CLOCK_SET;
  int64_t off = phoneEpoch - espEpoch;
  if (off < 0) off = -off;
  if (loggedIn && off > CLOCK_SLACK_S) return CLOCK_CORRECTED;
  return CLOCK_KEPT;
}

// Days since 1970-01-01 -> year, month, day (H. Hinnant's civil_from_days).
// Written out by hand so the laptop and the ESP32 give the same answer.
inline void clockCivil(int64_t days, int& y, unsigned& m, unsigned& d) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = (unsigned)(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400 + (m <= 2));
}

// UTC seconds + time zone -> "2026-11-01 14:05". out needs 17 bytes.
inline void clockFormat(int64_t epoch, long tzMin, char* out, size_t size) {
  int64_t t = epoch + (int64_t)tzMin * 60;
  int64_t days = t / 86400, secs = t % 86400;
  if (secs < 0) { secs += 86400; days--; }
  int y; unsigned m, d;
  clockCivil(days, y, m, d);
  snprintf(out, size, "%04d-%02u-%02u %02u:%02u", y, m, d, (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60));
}

// True for "" (no time) or text shaped like "2026-11-01 14:05".
inline bool clockTextOk(const char* s) {
  if (s[0] == 0) return true;
  if (strlen(s) != CLOCK_TEXT_LEN) return false;
  for (int i = 0; i < CLOCK_TEXT_LEN; i++) {
    char want = (i == 4 || i == 7) ? '-' : i == 10 ? ' ' : i == 13 ? ':' : '0';
    if (want == '0' ? (s[i] < '0' || s[i] > '9') : s[i] != want) return false;
  }
  return true;
}
