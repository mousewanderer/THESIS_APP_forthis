// =====================================================================
//  AgriDose_ESP32_Link -- ESP32: keypad, microSD logs, link to the Mega,
//  and the look-only web dashboard.
//
//  Based on FINAL_agridose_esp32.ino (28 Oct 2026): same keypad, same
//  pins, same style of menus. What changed:
//    - the Mega link is AgriLink (AgriLink.h): DOSE / TARE / STOP with seq numbers
//    - one dose is 300..900 g of recipe A, B or C (100..300 g until 1 Nov 2026); the Mega splits it by recipe
//    - the UART has its own FreeRTOS task (mega_link.cpp)
//    - every SD access goes through one mutex (sd_store.cpp)
//    - Blynk and the home Wi-Fi are gone; the ESP32 is now its own Wi-Fi
//      access point serving the dashboard (web_server.cpp)
//
//  ADDED 1 Nov 2026 (dashboard side; the keypad and the Mega link did not change)
//    - CLOCK: the ESP32 has no clock, so the dashboard page sends the
//      phone's time (POST /api/clock). Any phone sets it while the ESP32 has
//      none (after every restart); once set, only a logged-in phone more
//      than 2 min off may correct it. wall_clock.* keeps it, clock_rules.h
//      has the rules (tests/test_clock.cpp checks them on a laptop).
//    - DOSE TIMES: every dose row in /dose_log.csv gets a date_time column
//      ("2026-11-01 14:05"), empty while no phone has set the clock. The
//      Batch Log shows it. An old 9-column log still reads; its header is
//      upgraded once at start-up (dose_log.cpp).
//    - DELETE A RECORD: Delete under Edit in a record's details removes that
//      one crop record and its photos (POST /api/delete/<type>,
//      crop_records.cpp cropDeleteRecord). A planting whose plot still has
//      growth records can't be deleted.
//    - CROP GUIDE: the "verify" tag is no longer shown (www/app.js).
//
//  ADDED 3 Nov 2026: AFTER A POWER CUT. The Mega saves a running dose in its
//  EEPROM (AgriDose_Mega_Link/dose_state.h). At the next power-on it keeps the
//  augers off, asks on the LCD and reports flag R. This file only passes on the
//  answer: # sends RESUME, * sends STOP as usual. The dose keeps its old seq,
//  and it gets its one row in the log when it ends.
//
//  OFFLINE MODE (this file): the keypad and the Mega's LCD. The ONLY place
//  a dose can be started. ONLINE MODE (web_server.cpp): look-only.
//
//  KEYS    # = OK / start     * = back / cancel     D = home     0-9 = mass
//          HOME: # = new dose, 0 = zero the scales
//          While dosing: * = STOP.   After a fault: * = clear it.
//          After a power cut, when the LCD asks: # = continue the dose, * = end it.
//
//  SCREENS (sent to the Mega's LCD while it is idle)
//    HOME > SELECT RECIPE > RECIPE x > DOSE MASS > CONFIRM > STARTING
//    HOME > ZERO SCALES? > (the Mega zeroes, about 6 s)
//    During a dose the Mega draws its own screens. When it is done the
//    result shows here, e.g. "DONE 598.6/600g" / "-1.4g -0.2%". Any key clears it.
//
//  SERIAL MONITOR (115200 baud)
//    [LCD] lines show every menu screen, [KEY] every key, [LIVE] the weight
//    during a dose. With SERIAL_KEYS on, typing keys works too: #A#600##
//    = new dose, recipe A, OK, 600 g, start.
//
//  FILES   config.h       every setting and pin
//          AgriLink.h     the protocol (identical copy in AgriDose_Mega_Link/)
//          mega_link.*    the only code that talks to the Mega (its own task)
//          live_state.*   latest weight / flag / last dose, shared between tasks
//          sd_store.*     the SD card and its one mutex
//          dose_log.*     /dose_log.csv
//          wall_clock.*   [1 Nov] the date and time, taken from a phone (rules: clock_rules.h)
//          crop_records.* farmers and crop records, crop_validate.h checks the input
//          web_server.*   the dashboard; web_assets.h holds the page (generated)
//          fake_mega.*    pretend Mega for testing (FAKE_MEGA 1 in config.h)
//
//  ONE DOSE, FROM KEYPAD TO LOG
//    1. onKey() walks through the screens. At CONFIRM, # calls
//       linkRequest(LINK_DOSE, ...) and the screen goes to STARTING.
//    2. The link task sends DOSE. handleLinkResults() sees the ACK -> RUNNING.
//    3. The Mega doses and draws its own LCD screens, so draw() stops sending menus.
//    4. The Mega sends DONE. The link task answers GOT, logNewDoses() adds the
//       SD row, and followMega() keeps the numbers for the result screen.
//    5. When the Mega's flag is back to I, followMega() shows RESULT. Any key -> HOME.
//
//  TWO TASKS: loop() in this file runs the keypad and the screens. The UART
//  to the Mega has its own task (mega_link.cpp), so web traffic can't stop it
//  being read. The two only meet through the link*() queues and live_state.
// =====================================================================
#include <Arduino.h>
#include <Keypad.h>
#include "config.h"
#include "AgriLink.h"      // recipe table and dose limits for the menu screens
#include "live_state.h"
#include "mega_link.h"     // linkRequest(): only this file starts doses
#include "sd_store.h"
#include "dose_log.h"
#include "crop_records.h"
#include "crop_photos.h"
#include "accounts.h"
#include "web_server.h"

#define RANGE_ERR_MS   2000UL      // "OUT OF RANGE" shows this long
#define MSG_SHOW_MS    3000UL      // "NO REPLY" etc. show this long
#define RUN_GRACE_MS   1500UL      // after the DOSE is accepted, wait this long for flag D
#define TARE_WAIT_MS   15000UL     // give up waiting for the Mega to finish zeroing after this

// ------------------------------------------------------------ keypad ---
const byte ROWS = 4, COLS = 4;
// The character each key gives, laid out as on the keypad.
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
// The pin numbers are in config.h.
byte rowPins[ROWS] = KEYPAD_ROW_PINS;
byte colPins[COLS] = KEYPAD_COL_PINS;
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// The recipe names on the RECIPE_OK screen; selRecipe picks one.
const char* const RECIPE_TITLES[3] = {"RECIPE A GENERAL", "RECIPE B", "RECIPE C COMPACT"};

// ------------------------------------------------------------- state ---
// Which screen the keypad is on. draw() shows it; onKey() decides what each key does.
//   HOME        ready: # = new dose, 0 = zero the scales
//   SEL_RECIPE  choose A, B or C
//   RECIPE_OK   the recipe name; # accepts it
//   DOSE_MASS   type the grams
//   RANGE_ERR   the grams were outside the limits; shows for RANGE_ERR_MS
//   CONFIRM     the last check; # sends the DOSE
//   STARTING    the DOSE was sent; waiting for the Mega's answer
//   RUNNING     the Mega is dosing, showing a fault, or asking about an unfinished dose
//   RESULT      the last dose's result; any key clears it
//   TARE_ASK    "ZERO SCALES?"
//   TARE_WAIT   TARE was sent; waiting for the Mega to finish zeroing
//   MSG         a short message (showMsg) for MSG_SHOW_MS
enum State {
  HOME, SEL_RECIPE, RECIPE_OK, DOSE_MASS, RANGE_ERR, CONFIRM, STARTING,
  RUNNING,       // the Mega is dosing, faulted or asking to resume: it owns the LCD
  RESULT,        // the last dose's result
  TARE_ASK, TARE_WAIT, MSG
};
State state = HOME;

uint8_t  selRecipe = 0;            // 0 = A, 1 = B, 2 = C
char     entry[5];                 // up to 4 typed digits
uint8_t  entryN = 0;               // how many digits are in entry
int      doseG = 0;                // the mass accepted at DOSE_MASS, grams
uint32_t stateAt = 0;              // when the current state began
char     msgL0[17] = "", msgL1[17] = "";    // the text for the MSG screen

uint32_t   seenDoneCount = 0;      // live_state.doneCount we have already shown
bool       haveResult = false;     // a DONE arrived during this run
DoseResult result;
uint32_t   tareFrames = 0;         // live_state.frameCount when the TARE was accepted
bool       tareAcked = false;

// Changes the screen and notes the time, for the time limits in timers() and followMega().
void setState(State s) { state = s; stateAt = millis(); }

// A short message on the LCD; timers() goes back home after MSG_SHOW_MS.
void showMsg(const char* l0, const char* l1) {
  strncpy(msgL0, l0, 16); msgL0[16] = 0;
  strncpy(msgL1, l1, 16); msgL1[16] = 0;
  setState(MSG);
}

// Clears the typed digits.
void resetEntry() { entryN = 0; entry[0] = 0; }

// ------------------------------------------------- Serial Monitor LCD ---
// Prints a screen to the Serial Monitor, only when it changes.
void mirrorScreen(const char* l0, const char* l1) {
#if MIRROR_LCD
  static char last0[17] = "", last1[17] = "";
  if (strcmp(l0, last0) == 0 && strcmp(l1, last1) == 0) return;
  Serial.printf("[LCD] |%-16s|%-16s|\n", l0, l1);
  strncpy(last0, l0, 16); last0[16] = 0;
  strncpy(last1, l1, 16); last1[16] = 0;
#endif
}

// ------------------------------------------------------------ screens ---
// Runs on every loop() pass: builds the two lines for the current screen and
// passes them to the link task (linkShowLcd), which sends them to the Mega's LCD.
void draw() {
  if (state == RUNNING || state == TARE_WAIT) { linkReleaseLcd(); return; }   // the Mega draws these

  char l0[40] = "", l1[40] = "", a[12], b[12];   // roomy; cut to 16 columns below
  switch (state) {
    case HOME:
      snprintf(l0, sizeof(l0), "AGRIDOSE READY");
      snprintf(l1, sizeof(l1), "#=DOSE  0=ZERO");
      break;
    case SEL_RECIPE:
      snprintf(l0, sizeof(l0), "SELECT RECIPE");
      snprintf(l1, sizeof(l1), "A / B / C *=BACK");
      break;
    case RECIPE_OK:
      snprintf(l0, sizeof(l0), "%s", RECIPE_TITLES[selRecipe]);
      snprintf(l1, sizeof(l1), "#=OK   *=BACK");
      break;
    case DOSE_MASS:
      snprintf(l0, sizeof(l0), "DOSE MASS (g)");
      snprintf(l1, sizeof(l1), "%d-%d: %s", AGRILINK_DOSE_MIN_G, AGRILINK_DOSE_MAX_G, entryN ? entry : "_");
      break;
    case RANGE_ERR:
      snprintf(l0, sizeof(l0), "OUT OF RANGE");
      snprintf(l1, sizeof(l1), "USE %d-%d g", AGRILINK_DOSE_MIN_G, AGRILINK_DOSE_MAX_G);
      break;
    case CONFIRM: {                    // e.g. "A 600g 25/70/5%"
      const AgriRecipe& r = AGRI_RECIPES[selRecipe];
      snprintf(l0, sizeof(l0), "%c %dg %u/%u/%u%%", r.letter, doseG, r.hullPct, r.compostPct, r.ashPct);
      snprintf(l1, sizeof(l1), "#=START *=CANCEL");
      break;
    }
    case STARTING:
      snprintf(l0, sizeof(l0), "STARTING DOSE");
      snprintf(l1, sizeof(l1), "PLEASE WAIT...");
      break;
    case RESULT: {                     // e.g. "DONE 598.6/600g" / "-1.4g -0.2%"
      int32_t err = result.actualDg - result.targetDg;
      formatDg(a, sizeof(a), result.actualDg);
      formatDg(b, sizeof(b), err);
      float pct = result.targetDg ? err * 100.0f / result.targetDg : 0;
      snprintf(l0, sizeof(l0), "DONE %s/%ldg", a, (long)(result.targetDg / 10));
      snprintf(l1, sizeof(l1), "%s%sg %+.1f%%", err > 0 ? "+" : "", b, pct);
      break;
    }
    case TARE_ASK:
      snprintf(l0, sizeof(l0), "ZERO SCALES?");
      snprintf(l1, sizeof(l1), "#=YES   *=NO");
      break;
    case MSG:
      snprintf(l0, sizeof(l0), "%s", msgL0);
      snprintf(l1, sizeof(l1), "%s", msgL1);
      break;
    default:
      break;
  }
  l0[16] = 0;                          // the LCD has 16 columns
  l1[16] = 0;
  mirrorScreen(l0, l1);
  linkShowLcd(l0, l1);
}

// --------------------------------------------------------------- keys ---
// Adds a typed digit. Digits past the fourth are ignored.
void handleDigit(char k) {
  if (entryN < sizeof(entry) - 1) { entry[entryN++] = k; entry[entryN] = 0; }
}

// One key press, from the keypad or the Serial Monitor. The first switch handles
// the screens where most keys do nothing. After that, D always goes home, and
// the second switch handles the menu screens.
void onKey(char k) {
  Serial.printf("[KEY] %c\n", k);

  switch (state) {                     // screens where only some keys mean something
    case RUNNING:
    case STARTING:
      if (k == '*') {                  // STOP goes to the front of the queue
        Serial.println(linkRequest(LINK_STOP) ? "  -> STOP sent to the Mega" : "  -> STOP could not be queued!");
      } else if (k == '#' && state == RUNNING && liveGet().flag == AGRILINK_FLAG_RESUME) {
        // The answer to the Mega's question after a power cut. It does nothing
        // unless the Mega is asking, so # can't start an auger at any other time.
        Serial.println(linkRequest(LINK_RESUME) ? "  -> RESUME sent to the Mega" : "  -> RESUME could not be queued");
      }
      return;
    case TARE_WAIT:
      return;
    case MSG:
    case RESULT:
      setState(HOME);                  // any key clears the message / result
      return;
    default:
      break;
  }

  if (k == 'D') { setState(HOME); resetEntry(); return; }

  switch (state) {
    case HOME:
      if (k == '#') setState(SEL_RECIPE);
      else if (k == '0') setState(TARE_ASK);
      break;

    case SEL_RECIPE:
      if (k >= 'A' && k <= 'C') { selRecipe = k - 'A'; setState(RECIPE_OK); }
      else if (k == '*') setState(HOME);
      break;

    case RECIPE_OK:
      if (k == '#') { resetEntry(); setState(DOSE_MASS); }
      else if (k == '*') setState(SEL_RECIPE);
      break;

    case DOSE_MASS:
      if (k >= '0' && k <= '9') handleDigit(k);
      else if (k == '#') {
        // Checked here so the operator sees the error at once. The Mega checks again.
        int m = entryN ? atoi(entry) : 0;
        resetEntry();
        if (m < AGRILINK_DOSE_MIN_G || m > AGRILINK_DOSE_MAX_G) setState(RANGE_ERR);
        else { doseG = m; setState(CONFIRM); }
      } else if (k == '*') {
        if (entryN) resetEntry();      // clear what was typed
        else setState(RECIPE_OK);      // nothing typed: go back
      }
      break;

    case RANGE_ERR:                    // typing again dismisses the error
      if (k >= '0' && k <= '9') { setState(DOSE_MASS); handleDigit(k); }
      else if (k == '*') setState(DOSE_MASS);
      break;

    case CONFIRM:
      if (k == '#') {
        // The only line in the firmware that starts a dose.
        if (linkRequest(LINK_DOSE, AGRI_RECIPES[selRecipe].letter, doseG)) setState(STARTING);
        else showMsg("LINK BUSY", "TRY AGAIN");
      } else if (k == '*') setState(HOME);
      break;

    case TARE_ASK:
      if (k == '#') {
        if (linkRequest(LINK_TARE)) { tareAcked = false; setState(TARE_WAIT); }
        else showMsg("LINK BUSY", "TRY AGAIN");
      } else if (k == '*') setState(HOME);
      break;

    default:
      break;
  }
}

// Keys typed in the Serial Monitor: 0-9, A-D (a-d), # and *. One per loop()
// pass, so every screen in between is drawn, like pressing keys one by one.
void pollSerialKeys() {
#if SERIAL_KEYS
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c >= 'a' && c <= 'd') c = c - 'a' + 'A';
    bool isKey = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'D') || c == '#' || c == '*';
    if (isKey) { onKey(c); return; }
  }
#endif
}

// ------------------------------------------------- answers from the Mega ---
// How each command sent to the Mega ended: accepted, refused, or no answer.
void handleLinkResults() {
  LinkResult r;
  while (linkPollResult(r)) {
    if (r.cmd == LINK_DOSE && state == STARTING) {
      if (r.outcome == LINK_ACKED) { haveResult = false; setState(RUNNING); }
      else if (r.outcome == LINK_REFUSED && strcmp(r.nakReason, "BUSY") == 0) showMsg("MACHINE BUSY", "TRY AGAIN LATER");
      else if (r.outcome == LINK_REFUSED) showMsg("DOSE REFUSED", r.nakReason);
      else if (r.outcome == LINK_MEGA_RESTARTED) showMsg("MEGA RESTARTED", "TRY AGAIN");
      else showMsg("NO REPLY", "CHECK MEGA LINK");
    } else if (r.cmd == LINK_TARE && state == TARE_WAIT) {
      if (r.outcome == LINK_ACKED) { tareAcked = true; tareFrames = liveGet().frameCount; }
      else if (r.outcome == LINK_REFUSED) showMsg("MACHINE BUSY", "TRY AGAIN LATER");
      else showMsg("NO REPLY", "CHECK MEGA LINK");
    } else if (r.cmd == LINK_STOP && r.outcome != LINK_ACKED) {
      Serial.println("WARNING: the Mega did not confirm STOP. Press * again or use the E-stop.");
    } else if (r.cmd == LINK_RESUME && r.outcome != LINK_ACKED) {
      Serial.println("The Mega did not accept RESUME. If it is still asking, press # again.");
    }
  }
}

// Writes each new dose to the SD card (one row per DONE seq).
void logNewDoses() {
  DoseResult d;
  while (linkPollDone(d)) {
    if (doseLogAppend(d)) Serial.printf("Dose saved as dose no. %lu\n", (unsigned long)doseLogCount());
    else Serial.println("Dose NOT saved: SD card problem");
  }
}

// Follows what the Mega is doing (flag in WT), so the screens stay right
// even if an ACK was lost or the ESP32 restarted in the middle of a dose.
void followMega() {
  LiveState s = liveGet();
  if (s.doneCount != seenDoneCount) {  // a new DONE: keep it for the result screen
    seenDoneCount = s.doneCount;
    result = s.lastDone;
    haveResult = true;
  }

  bool megaBusy = liveMegaOnline(s) && (s.flag == AGRILINK_FLAG_DOSING || s.flag == AGRILINK_FLAG_FAULT ||
                                        s.flag == AGRILINK_FLAG_RESUME);
  if (megaBusy && state != RUNNING && state != TARE_WAIT) {
    Serial.println(s.flag == AGRILINK_FLAG_RESUME ? "The Mega has an unfinished dose: # continues it, * ends it"
                                                  : "The Mega is dosing or faulted -- it owns the LCD now");
    haveResult = false;
    setState(RUNNING);
  } else if (state == RUNNING && !megaBusy && millis() - stateAt > RUN_GRACE_MS) {
    setState(haveResult ? RESULT : HOME);
  }

  if (state == TARE_WAIT && ((tareAcked && s.frameCount != tareFrames) || millis() - stateAt > TARE_WAIT_MS)) {
    setState(HOME);                    // the Mega reported again, so zeroing is over
  }

  static uint32_t lastLiveAt = 0;      // progress on the Serial Monitor during a dose
  if (state == RUNNING && millis() - lastLiveAt >= 1000) {
    lastLiveAt = millis();
    char g[12];
    formatDg(g, sizeof(g), s.weightDg);
    Serial.printf("[LIVE] %c %s g\n", s.flag, g);
  }
}

// Screens that go away by themselves.
void timers() {
  if (state == RANGE_ERR && millis() - stateAt > RANGE_ERR_MS) setState(DOSE_MASS);
  if (state == MSG && millis() - stateAt > MSG_SHOW_MS) setState(HOME);
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== AgriDose ESP32 (AgriLink + web dashboard) ===");
  keypad.setDebounceTime(KEYPAD_DEBOUNCE_MS);

  // The order matters: the SD card starts before anything that reads its files,
  // and the dose log before linkBegin(), which carries on the numbering from it.
  liveBegin();
  sdBegin();
  doseLogBegin();
  cropBegin();
  photoBegin();
  accountsBegin();
  linkBegin(doseLogLastSeq());
  webBegin();                      // the Wi-Fi access point and the dashboard

#if MIRROR_LCD
  Serial.println("Menu screens print here as [LCD] |line 1|line 2|");
#endif
#if SERIAL_KEYS
  Serial.println("Type keys here too: 0-9  A-D  #  *   (e.g. #A#600# then #)");
#endif
}

// One pass: read the keys, act on news from the Mega, draw the screen, then
// do the dashboard's regular work. The UART itself runs in the link task.
void loop() {
  char k = keypad.getKey();
  if (k) onKey(k);
  pollSerialKeys();                // keys typed on the Serial Monitor

  handleLinkResults();             // how each command ended
  logNewDoses();                   // new doses go to the SD card
  followMega();                    // the screens follow the Mega's state
  timers();                        // screens that time out
  draw();                          // send the current screen
  webLoop();                       // live updates to the phones
}
