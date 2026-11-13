// =====================================================================
//  AgriLink.h  --  the Mega <-> ESP32 UART protocol, in one file.
//
//  KEEP THIS FILE IDENTICAL in AgriDose_ESP32_Link/ and AgriDose_Mega_Link/.
//  If you change it, copy it to the other folder before flashing either board.
//
//  Written 28 Oct 2026 from the protocol notes in CLAUDE.md. Anything
//  marked [ADDED] or [DEFINED HERE] was not in those notes; the team has
//  reviewed and confirmed those choices.
//
//  WIRING   ESP32 GPIO26 (TX) -> Mega RX2 (D17)
//           Mega TX2 (D16) -> 1k -> ESP32 GPIO27 (RX), 2k from GPIO27 to GND
//           Common ground. 57600 baud, 8N1.
//
//  FRAME    $BODY*HHHH\r\n
//           BODY = comma-separated ASCII, the first field is the verb.
//           HHHH = CRC-16/CCITT-FALSE of BODY, 4 uppercase hex digits.
//           The receiver ignores bytes until a '$' and splits at the LAST
//           '*', so a '*' inside BODY is fine (LCD text like "*=BACK").
//           BODY may not contain '$', CR or LF. Fields may not contain ','.
//
//  MASSES   integer decigrams (dg). 1 g = 10 dg, so 248.6 g is sent as 2486.
//
//  ESP32 -> Mega, sequenced (seq 1..255, the Mega answers ACK or NAK)
//    PING,<seq>                    are you there?
//    TARE,<seq>                    zero all three hopper scales (idle only)
//    STOP,<seq>                    stop the augers now; also ends a paused dose
//    DOSE,<seq>,<A|B|C>,<grams>    one dose of recipe A/B/C, 300..900 g total
//    RESUME,<seq>                  [ADDED 3 Nov] flag F or R only: continue the dose
//                                  a fault or a power cut interrupted. STOP ends it instead.
//    DRAIN,<seq>,<A|B|C>           [ADDED 10 Nov] idle only: run that one hopper's auger
//                                  to empty it (flag E). STOP ends it; so do no flow for
//                                  15 s (empty), a dead load cell and a time limit. Not a
//                                  dose: no DONE, no log row, not saved for a power cut.
//
//  ESP32 -> Mega, unsequenced (no answer)
//    HELLO                         the ESP32 just booted: forget the seq memory
//    GOT,<seq>                     DONE <seq> arrived, stop repeating it
//    DISP,<line 1>,<line 2>        [ADDED] menu text for the 16x2 LCD, shown
//                                  only while the Mega is idle. Needed because
//                                  the keypad is on the ESP32 and the LCD is on
//                                  the Mega.
//
//  Mega -> ESP32
//    ACK,<seq>
//    NAK,<seq>,<BUSY|RANGE|RECIPE|ARGS|UNKNOWN>    (seq 0 = could not read it)
//    WT,<dg>,<I|D|F|R|E>           every 200 ms
//    HW,<hull dg>,<compost dg>,<ash dg>   [ADDED 30 Oct] see HW below
//    DONE,<seq>,<recipe>,<target dg>,<actual dg>   every 500 ms until GOT
//    HELLO                         the Mega just booted
//
//  HW  [ADDED 30 Oct] what is in each hopper now (the stock), for the
//    dashboard's hopper gauges. Sent only when a hopper moved more than
//    AGRILINK_HW_DEADBAND_DG (5 g) from the value last sent, so load-cell
//    noise is not sent at all; at most every AGRILINK_HW_MIN_MS while it
//    keeps changing (dosing); and every AGRILINK_HW_REFRESH_MS anyway, so an
//    ESP32 that restarted or missed a frame catches up.
//
//  WT  [DEFINED HERE]
//    <dg>  mass dispensed so far in the current dose, all hoppers added up.
//          It holds the final value after the dose and goes back to 0 at TARE.
//    I     idle: ready for a command
//    D     dosing: an auger is running or the scale is settling
//    F     fault: hopper empty, sensor error or timeout. The augers are
//          stopped and the dose is paused [3 Nov; until then it ended here].
//          RESUME continues it, STOP ends it and DONE reports what came out.
//    R     [ADDED 3 Nov] the power went off during a dose. The augers stay off
//          until the operator answers on the keypad: RESUME continues the dose,
//          STOP ends it and DONE reports what came out. It keeps its old seq.
//    E     [ADDED 10 Nov] draining: one hopper's auger runs to empty it (DRAIN).
//          <dg> stays the last dose's mass; the hopper's weight going down is in HW.
//
//  SAFETY RULES
//    - One command in flight: the ESP32 waits for ACK/NAK, or gives up,
//      before it sends the next command.
//    - A retry reuses the same seq. The Mega remembers the last seq and its
//      answer: a repeated seq gets the same answer and is never run twice.
//    - The ESP32 acts on each DONE seq once, so there is one log row per dose.
//    - Only the ESP32 keypad starts or resumes a dose, or drains a hopper. The web
//      dashboard is look-only.
// =====================================================================
#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <stdint.h>   // lets the laptop unit test (tests/) compile this file
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#endif

// ---- link settings ----
#define AGRILINK_BAUD       57600UL
#define AGRILINK_MAX_BODY   48                       // longest BODY is DISP with two 16-char lines (38)
#define AGRILINK_MAX_FRAME  (AGRILINK_MAX_BODY + 8)  // + '$' '*' HHHH CR LF

// ---- timing, in milliseconds ----
#define AGRILINK_WT_PERIOD_MS    200UL
#define AGRILINK_DONE_REPEAT_MS  500UL
#define AGRILINK_ACK_TIMEOUT_MS  500UL   // [DEFINED HERE] wait this long for ACK/NAK...
#define AGRILINK_MAX_TRIES       6       // ...and send a command at most this many times (3 s)
#define AGRILINK_HW_MIN_MS       500UL   // [ADDED] HW at most this often...
#define AGRILINK_HW_REFRESH_MS   10000UL // ...and at least this often

// ---- HW deadband: changes this small are noise and are not sent ----
#define AGRILINK_HW_DEADBAND_DG  50      // 5 g

// True if any hopper moved MORE than deadbandDg from the value last sent.
// Compared with what was sent, not with the previous reading, so a slow
// drift is still sent once it adds up past the deadband.
static inline bool agriHwChanged(const int32_t sent[3], const int32_t now[3], int32_t deadbandDg) {
  for (int i = 0; i < 3; i++) {
    int32_t d = now[i] - sent[i];
    if (d > deadbandDg || d < -deadbandDg) return true;
  }
  return false;
}

// ---- dose limits, grams ----
#define AGRILINK_DOSE_MIN_G  300     // [1 Nov 2026] was 100..300 g
#define AGRILINK_DOSE_MAX_G  900

// ---- WT flags ----
#define AGRILINK_FLAG_IDLE    'I'
#define AGRILINK_FLAG_DOSING  'D'
#define AGRILINK_FLAG_FAULT   'F'
#define AGRILINK_FLAG_RESUME  'R'
#define AGRILINK_FLAG_DRAIN   'E'    // [ADDED 10 Nov] a hopper is being drained

// ---- NAK reasons ----
#define AGRILINK_NAK_BUSY     "BUSY"
#define AGRILINK_NAK_RANGE    "RANGE"
#define AGRILINK_NAK_RECIPE   "RECIPE"
#define AGRILINK_NAK_ARGS     "ARGS"
#define AGRILINK_NAK_UNKNOWN  "UNKNOWN"

// ---- recipes ----
// Kept here so both boards use the same numbers. Percent by mass.
struct AgriRecipe {
  char    letter;
  uint8_t hullPct, compostPct, ashPct;   // rice hull, compost, wood ash
};

static const AgriRecipe AGRI_RECIPES[3] = {
  {'A', 25, 70,  5},
  {'B', 30, 65,  5},
  {'C', 30, 60, 10},
};

// [ADDED 10 Nov] The hopper index (0 = A hull, 1 = B compost, 2 = C ash) for the
// letter in DRAIN, or -1 for anything else.
static inline int8_t agriHopperIndex(const char* s) {
  if (s == nullptr || s[0] < 'A' || s[0] > 'C' || s[1] != 0) return -1;
  return (int8_t)(s[0] - 'A');
}

// Returns the recipe for 'A', 'B' or 'C', or nullptr for anything else.
static inline const AgriRecipe* agriRecipe(char letter) {
  for (uint8_t i = 0; i < 3; i++)
    if (AGRI_RECIPES[i].letter == letter) return &AGRI_RECIPES[i];
  return nullptr;
}

// Splits a dose into the three hoppers, in dg. Rice hull and wood ash are
// rounded to the nearest dg; compost gets the rest, so the three always add
// up to exactly totalDg.
static inline void agriSplitDose(const AgriRecipe& r, int32_t totalDg,
                                 int32_t& hullDg, int32_t& compostDg, int32_t& ashDg) {
  hullDg    = (totalDg * r.hullPct + 50) / 100;
  ashDg     = (totalDg * r.ashPct  + 50) / 100;
  compostDg = totalDg - hullDg - ashDg;
}

static inline bool agriIsFlag(char c) {
  return c == AGRILINK_FLAG_IDLE || c == AGRILINK_FLAG_DOSING || c == AGRILINK_FLAG_FAULT || c == AGRILINK_FLAG_RESUME ||
         c == AGRILINK_FLAG_DRAIN;
}

// ---------------------------------------------------------------------
//  CRC-16/CCITT-FALSE: polynomial 0x1021, start value 0xFFFF, no bit
//  reversal, no final XOR. Check value: "123456789" gives 0x29B1.
// ---------------------------------------------------------------------
static inline uint16_t agriCrc16(const char* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)((uint8_t)data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
      else              crc = (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// ---------------------------------------------------------------------
//  Sending: builds "$BODY*HHHH\r\n" into out. Returns its length, or 0
//  if BODY is empty, too long, or has a character that breaks the frame.
// ---------------------------------------------------------------------
static inline size_t agriBuildFrame(const char* body, char* out, size_t outSize) {
  size_t n = strlen(body);
  if (n == 0 || n > AGRILINK_MAX_BODY || outSize < n + 9) return 0;
  for (size_t i = 0; i < n; i++) {
    char c = body[i];
    if (c == '$' || c < 32 || c > 126) return 0;
  }
  static const char HEXDIGITS[] = "0123456789ABCDEF";
  uint16_t crc = agriCrc16(body, n);
  size_t p = 0;
  out[p++] = '$';
  memcpy(out + p, body, n);
  p += n;
  out[p++] = '*';
  out[p++] = HEXDIGITS[(crc >> 12) & 0xF];
  out[p++] = HEXDIGITS[(crc >> 8) & 0xF];
  out[p++] = HEXDIGITS[(crc >> 4) & 0xF];
  out[p++] = HEXDIGITS[crc & 0xF];
  out[p++] = '\r';
  out[p++] = '\n';
  out[p] = 0;
  return p;
}

#ifdef ARDUINO
// Builds the frame and writes it to the port in one piece.
// Returns false if BODY was not valid (nothing is sent).
static inline bool agriSend(Print& port, const char* body) {
  char frame[AGRILINK_MAX_FRAME + 1];
  size_t n = agriBuildFrame(body, frame, sizeof(frame));
  if (n == 0) return false;
  port.write((const uint8_t*)frame, n);
  return true;
}
#endif

// ---------------------------------------------------------------------
//  Receiving: feed every byte to agriRxFeed(). It returns true when a
//  whole frame with a correct CRC has arrived; BODY is then in rx.buf
//  (with the "*HHHH" cut off) until the next byte is fed.
// ---------------------------------------------------------------------
struct AgriRx {
  char     buf[AGRILINK_MAX_BODY + 6];   // BODY + '*' + HHHH + NUL
  uint8_t  len;
  bool     inFrame;
  uint32_t good, bad;                    // counters for the Serial Monitor
};

static inline void agriRxReset(AgriRx& rx) {
  rx.len = 0;
  rx.inFrame = false;
  rx.good = 0;
  rx.bad = 0;
}

static inline int8_t agriHexValue(char h) {
  if (h >= '0' && h <= '9') return h - '0';
  if (h >= 'A' && h <= 'F') return h - 'A' + 10;
  if (h >= 'a' && h <= 'f') return h - 'a' + 10;
  return -1;
}

static inline bool agriRxFeed(AgriRx& rx, char c) {
  if (c == '$') { rx.inFrame = true; rx.len = 0; return false; }   // a frame starts (or restarts)
  if (!rx.inFrame) return false;                                    // noise between frames
  if (c == '\r') return false;

  if (c == '\n') {                                                  // end of frame: check it
    rx.inFrame = false;
    rx.buf[rx.len] = 0;
    char* star = strrchr(rx.buf, '*');
    if (star == nullptr || star == rx.buf || strlen(star + 1) != 4) { rx.bad++; return false; }
    uint16_t want = 0;
    for (uint8_t i = 1; i <= 4; i++) {
      int8_t v = agriHexValue(star[i]);
      if (v < 0) { rx.bad++; return false; }
      want = (uint16_t)((want << 4) | (uint8_t)v);
    }
    *star = 0;                                                      // rx.buf now holds only BODY
    if (agriCrc16(rx.buf, (size_t)(star - rx.buf)) != want) { rx.bad++; return false; }
    rx.good++;
    return true;
  }

  if (c < 32 || c > 126 || rx.len >= sizeof(rx.buf) - 1) {          // garbage or too long: drop it
    rx.inFrame = false;
    rx.bad++;
    return false;
  }
  rx.buf[rx.len++] = c;
  return false;
}

// ---------------------------------------------------------------------
//  Reading the fields of a BODY
// ---------------------------------------------------------------------

// Splits BODY in place at the commas. fields[0] is the verb.
// Returns how many fields there are (at most maxFields).
static inline uint8_t agriSplit(char* body, char* fields[], uint8_t maxFields) {
  uint8_t n = 0;
  char* p = body;
  while (n < maxFields) {
    fields[n++] = p;
    char* comma = strchr(p, ',');
    if (comma == nullptr) break;
    *comma = 0;
    p = comma + 1;
  }
  return n;
}

// Reads a whole decimal number, e.g. "2486" or "-3". Returns false for
// anything else, so "12a" or "" is rejected instead of being read as 12 or 0.
static inline bool agriParseInt(const char* s, int32_t& out) {
  if (s == nullptr || *s == 0) return false;
  char* end;
  long v = strtol(s, &end, 10);
  if (*end != 0) return false;
  out = (int32_t)v;
  return true;
}

// Reads a seq number, 1..255.
static inline bool agriParseSeq(const char* s, uint8_t& seq) {
  int32_t v;
  if (!agriParseInt(s, v) || v < 1 || v > 255) return false;
  seq = (uint8_t)v;
  return true;
}

// Copies one LCD line (at most 16 characters) and replaces the characters
// that would break a DISP frame (',' and '$', or non-printable) with spaces.
// dst must hold 17 bytes.
static inline void agriCopyLcdLine(char* dst, const char* src) {
  uint8_t n = 0;
  while (n < 16 && src[n]) {
    char c = src[n];
    dst[n] = (c == ',' || c == '$' || c < 32 || c > 126) ? ' ' : c;
    n++;
  }
  dst[n] = 0;
}
