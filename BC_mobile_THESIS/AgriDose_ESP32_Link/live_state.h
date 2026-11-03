// =====================================================================
//  live_state -- the latest news from the Mega, shared between tasks.
//
//  The link task writes it; the keypad code and the web dashboard read
//  it. A FreeRTOS mutex makes sure nobody reads half of an update.
//  Readers get a copy (liveGet), so they never hold the mutex for long.
//
//  This file does not include AgriLink.h or mega_link.h on purpose: the
//  web code includes this file and must not be able to reach the UART.
// =====================================================================
#pragma once
#include <Arduino.h>

// The last finished dose, from the Mega's DONE frame.
struct DoseResult {
  bool    valid;       // false until the first DONE after boot
  uint8_t seq;
  char    recipe;      // 'A', 'B' or 'C'
  int32_t targetDg;    // decigrams
  int32_t actualDg;
};

struct LiveState {
  bool     everHeard;     // at least one valid frame since boot
  uint32_t lastFrameMs;   // millis() of the last valid frame from the Mega
  uint32_t frameCount;    // valid frames since boot
  int32_t  weightDg;      // from WT: dispensed in the current / last dose
  char     flag;          // from WT: 'I', 'D', 'F', 'R', or '?' before the first WT
  uint32_t doneCount;     // goes up by one for every new DONE
  DoseResult lastDone;
  bool     haveHoppers;   // at least one HW frame since boot
  int32_t  hopperDg[3];   // from HW: what is in hopper A, B, C (the Mega sends only changes over 5 g)
};

void liveBegin();
void liveTouch();                         // any valid frame arrived
void liveSetWeight(int32_t dg, char flag);
void liveSetHoppers(const int32_t dg[3]);
void liveSetDone(const DoseResult& d);
LiveState liveGet();                      // a copy, taken under the mutex
bool liveMegaOnline(const LiveState& s);  // a valid frame in the last LINK_OFFLINE_MS
