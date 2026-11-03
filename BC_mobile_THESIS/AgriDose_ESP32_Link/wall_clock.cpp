// =====================================================================
//  wall_clock.cpp -- keeps the date and time a phone gave the ESP32
//  (1 Nov 2026). See wall_clock.h for what it is for, clock_rules.h for
//  when a phone's time is taken.
//
//  HOW IT COUNTS: when a phone's time is taken, two numbers are kept: the
//  phone's UTC seconds (baseEpoch) and the ESP32's own microsecond counter
//  at that moment (baseUs, esp_timer_get_time(), which never wraps). The
//  time "now" is baseEpoch + the seconds counted since. The ESP32's crystal
//  drifts only a few seconds a day, and a logged-in phone corrects it if it
//  is ever more than CLOCK_SLACK_S off.
//
//  THREADS: the web server task sets the time (POST /api/clock) while
//  loop() reads it (dose log) and the web server reads it (status). The
//  64-bit numbers can't be read in one step on the ESP32, so every access
//  takes a short spinlock (portMUX), held for a few instructions only.
// =====================================================================
#include "wall_clock.h"
#include <esp_timer.h>

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool    isSet = false;        // false after every restart, until a phone sends its time
static int64_t baseEpoch = 0;        // UTC seconds at baseUs
static int64_t baseUs = 0;           // esp_timer_get_time() when it was set (microseconds; does not wrap)
static long    tzMin = 0;            // the phone's time zone, minutes east of UTC (+480 = Philippines)

// The time now in UTC seconds. Call only while holding the lock.
static int64_t nowLocked() { return baseEpoch + (esp_timer_get_time() - baseUs) / 1000000; }

// A phone offers its time: clockDecide() says whether to take it. The
// Serial Monitor shows every time it is set or corrected.
ClockVerdict clockOffer(int64_t phoneEpoch, long tz, bool loggedIn) {
  portENTER_CRITICAL(&lock);
  int64_t was = isSet ? nowLocked() : 0;
  ClockVerdict v = clockDecide(isSet, was, phoneEpoch, tz, loggedIn);
  if (v == CLOCK_SET || v == CLOCK_CORRECTED) {
    baseEpoch = phoneEpoch;
    baseUs = esp_timer_get_time();
    tzMin = tz;
    isSet = true;
  }
  portEXIT_CRITICAL(&lock);

  // Printing is done outside the lock (Serial is slow).
  if (v == CLOCK_SET || v == CLOCK_CORRECTED) {
    char t[20];
    clockFormat(phoneEpoch, tz, t, sizeof(t));
    long a = tz < 0 ? -tz : tz;
    if (v == CLOCK_SET)
      Serial.printf("Clock set from a phone: %s (UTC%c%ld:%02ld)\n", t, tz < 0 ? '-' : '+', a / 60, a % 60);
    else
      Serial.printf("Clock corrected by a logged-in phone: %s (it was %lld s off)\n", t, (long long)(phoneEpoch - was));
  }
  return v;
}

// True once a phone has given the time since the ESP32 started.
bool clockIsSet() {
  portENTER_CRITICAL(&lock);
  bool s = isSet;
  portEXIT_CRITICAL(&lock);
  return s;
}

// UTC seconds now, or 0 while no phone has given the time.
int64_t clockNow() {
  portENTER_CRITICAL(&lock);
  int64_t t = isSet ? nowLocked() : 0;
  portEXIT_CRITICAL(&lock);
  return t;
}

// The time zone of the phone that set the clock.
long clockTzMin() {
  portENTER_CRITICAL(&lock);
  long t = tzMin;
  portEXIT_CRITICAL(&lock);
  return t;
}

// "2026-11-01 14:05" for the dose log, or "" while the clock is not set
// (the dose is then logged with an empty date_time).
void clockNowText(char* out, size_t size) {
  portENTER_CRITICAL(&lock);
  bool s = isSet;
  int64_t t = s ? nowLocked() : 0;
  long tz = tzMin;
  portEXIT_CRITICAL(&lock);
  if (!s) { if (size) out[0] = 0; return; }
  clockFormat(t, tz, out, size);
}
