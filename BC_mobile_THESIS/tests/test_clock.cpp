// Laptop unit test for clock_rules.h (no board needed): when the ESP32 takes
// a phone's time, and the "2026-11-01 14:05" text in dose_log.csv.
// The reference times were worked out with Python's calendar.timegm.
//
// Build and run from the project folder (MSYS2 g++):
//   g++ -std=c++11 -Wall -I AgriDose_ESP32_Link tests/test_clock.cpp -o test_clock
//   ./test_clock
#include <stdio.h>
#include "clock_rules.h"

static int failures = 0;

static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

static bool fmtIs(long long epoch, long tz, const char* want) {
  char out[20];
  clockFormat(epoch, tz, out, sizeof(out));
  if (strcmp(out, want) != 0) printf("      got %s, want %s\n", out, want);
  return strcmp(out, want) == 0;
}

int main() {
  const long long T = 1793513100LL;   // 2026-11-01 06:05 UTC = 14:05 in the Philippines

  // ---- the rules ----
  check(clockDecide(false, 0, T, 480, false) == CLOCK_SET, "no time yet: any phone sets it");
  check(clockDecide(false, 0, T, 480, true) == CLOCK_SET, "no time yet: a logged-in phone sets it too");
  check(clockDecide(true, T, T + 3600, 480, false) == CLOCK_KEPT, "set: a phone that isn't logged in can't move it");
  check(clockDecide(true, T, T + 3600, 480, true) == CLOCK_CORRECTED, "set: a logged-in phone 1 h off corrects it");
  check(clockDecide(true, T, T - 3600, 480, true) == CLOCK_CORRECTED, "set: a logged-in phone 1 h behind corrects it");
  check(clockDecide(true, T, T + CLOCK_SLACK_S, 480, true) == CLOCK_KEPT, "set: exactly CLOCK_SLACK_S off is kept");
  check(clockDecide(true, T, T + CLOCK_SLACK_S + 1, 480, true) == CLOCK_CORRECTED, "set: one second more is corrected");
  check(clockDecide(false, 0, 315532800LL, 480, false) == CLOCK_BAD, "a 1980 time (dead phone clock) is refused");
  check(clockDecide(false, 0, 0, 480, false) == CLOCK_BAD, "1970 is refused");
  check(clockDecide(false, 0, CLOCK_MIN_EPOCH, 480, false) == CLOCK_SET, "2026-01-01 00:00 UTC is the first time taken");
  check(clockDecide(false, 0, CLOCK_MAX_EPOCH + 1, 480, false) == CLOCK_BAD, "after 2099 is refused");
  check(clockDecide(false, 0, T, 841, false) == CLOCK_BAD && clockDecide(false, 0, T, -721, false) == CLOCK_BAD,
        "a time zone outside UTC-12..+14 is refused");
  check(clockDecide(false, 0, T, 840, false) == CLOCK_SET && clockDecide(false, 0, T, -720, false) == CLOCK_SET,
        "UTC+14 and UTC-12 are taken");
  check(clockDecide(true, T, 0, 480, true) == CLOCK_BAD, "a bad time is refused even when logged in");

  // ---- the text ----
  check(fmtIs(T, 480, "2026-11-01 14:05"), "14:05 in the Philippines (UTC+8)");
  check(fmtIs(T, 0, "2026-11-01 06:05"), "06:05 UTC");
  check(fmtIs(T, -300, "2026-11-01 01:05"), "a time zone west of UTC");
  check(fmtIs(1767225600LL, 480, "2026-01-01 08:00"), "new year in UTC");
  check(fmtIs(1767225600LL, -300, "2025-12-31 19:00"), "the day before, west of UTC");
  check(fmtIs(1835452740LL, 480, "2028-02-29 23:59"), "a leap day");
  check(fmtIs(1835452740LL + 60, 480, "2028-03-01 00:00"), "the day after a leap day");
  check(fmtIs(4102444740LL, 480, "2100-01-01 07:59"), "2099-12-31 23:59 UTC, next morning in UTC+8");
  check(fmtIs(T + 59, 480, "2026-11-01 14:05"), "seconds are cut, not rounded");

  // ---- reading the text back from the CSV ----
  check(clockTextOk("2026-11-01 14:05"), "time text accepted");
  check(clockTextOk(""), "an empty time (not known) is accepted");
  check(!clockTextOk("2026-11-01 14:5") && !clockTextOk("2026-11-01T14:05") && !clockTextOk("2026/11/01 14:05") &&
        !clockTextOk("2026-11-01 14:05:00") && !clockTextOk("abcd-ef-gh ij:kl"), "broken time text rejected");

  printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
