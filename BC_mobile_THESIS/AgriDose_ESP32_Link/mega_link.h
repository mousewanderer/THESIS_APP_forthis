// =====================================================================
//  mega_link -- the ONLY code that talks to the Mega.
//
//  It runs as its own FreeRTOS task (priority above the web server), so
//  web traffic can never stop the UART from being read. The task:
//    - reads every frame and keeps live_state up to date
//    - answers DONE with GOT and passes each new dose on for the SD log
//    - sends the keypad's commands one at a time, retrying with the same seq
//    - sends the menu text (DISP) to the Mega's LCD
//
//  LOOK-ONLY RULE: the web dashboard files never include this header, so
//  no web request can call linkRequest() (DOSE, TARE, STOP, RESUME, DRAIN).
//  Only the keypad code in AgriDose_ESP32_Link.ino uses it.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "live_state.h"

enum LinkCmd : uint8_t { LINK_PING, LINK_TARE, LINK_STOP, LINK_DOSE, LINK_RESUME,
                         LINK_DRAIN };   // [10 Nov] empty one hopper (DRAIN)

enum LinkOutcome : uint8_t {
  LINK_ACKED,           // the Mega said ACK
  LINK_REFUSED,         // the Mega said NAK (reason in nakReason)
  LINK_NO_REPLY,        // no answer after AGRILINK_MAX_TRIES sends
  LINK_MEGA_RESTARTED   // the Mega rebooted while we waited; not sent again
};

struct LinkResult {
  LinkCmd     cmd;
  uint8_t     seq;
  LinkOutcome outcome;
  char        nakReason[10];
};

// Opens the UART and starts the task. lastLoggedSeq is the seq of the newest
// row in the dose log (0 if none), so numbering carries on after a restart.
void linkBegin(uint8_t lastLoggedSeq);

// Keypad -> Mega. Returns false if the command queue is full.
// recipe and grams are used by LINK_DOSE only. LINK_DRAIN takes the hopper
// letter ('A', 'B' or 'C') in recipe.
bool linkRequest(LinkCmd cmd, char recipe = 0, int grams = 0);

// For loop(): how did each command end? (true = one result was taken)
bool linkPollResult(LinkResult& out);

// For loop(): a new finished dose to write to the SD log (one per DONE seq).
bool linkPollDone(DoseResult& out);

// What the Mega's LCD should show while it is idle (sent again every 2 s).
void linkShowLcd(const char* line0, const char* line1);
// Stop sending DISP: the Mega is drawing its own screens.
void linkReleaseLcd();
