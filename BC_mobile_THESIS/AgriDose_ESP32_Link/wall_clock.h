// =====================================================================
//  wall_clock -- the date and time, taken from a phone (1 Nov 2026).
//
//  The ESP32 has no clock that keeps running while it is off, and its
//  Wi-Fi is its own access point, so there is no internet time either.
//  The dashboard page sends the phone's time (POST /api/clock) and the
//  rules in clock_rules.h decide whether to take it. From then on the
//  ESP32 counts on from that time by itself.
//
//  After every restart the time is gone again until a phone opens the
//  dashboard. A dose made before that is logged with no time
//  (dose_log.csv: an empty date_time).
//
//  Set from the web server task, read from loop() (the dose log): every
//  access takes a short spinlock.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "clock_rules.h"

// Offers a phone's time. loggedIn = the request came from a logged-in phone.
ClockVerdict clockOffer(int64_t phoneEpoch, long tzMin, bool loggedIn);

bool    clockIsSet();
int64_t clockNow();                          // UTC seconds, 0 if not set
long    clockTzMin();                        // minutes east of UTC of the phone that set it

// The time now as "2026-11-01 14:05" (the phone's time zone), or "" if the
// clock is not set. out needs 17 bytes.
void clockNowText(char* out, size_t size);
