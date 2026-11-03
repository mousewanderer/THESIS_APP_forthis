// =====================================================================
//  AgriDose_Mega_Link -- Arduino Mega 2560: load cells, augers, LCD.
//  Talks to the ESP32 with the AgriLink protocol (AgriLink.h).
//
//  Based on FINAL_agridose_mega.ino (28 Oct 2026). Copied unchanged: the
//  pin map, the calibration values and the dosing engine (adaptive auger
//  speed, learned early stop, settle, trims, empty / sensor / timeout
//  checks). What changed is how the Mega is told what to do:
//    old: RATIO:a,b,c grams per hopper, KEY:OK / KEY:BACK, accept/reject
//    new: DOSE,<seq>,<recipe>,<grams> (300..900 g since 1 Nov 2026, split here by recipe),
//         STOP, TARE, PING; DONE with the actual mass goes back to the ESP32
//
//  PULSED AUGERS [1 Nov]: each hopper has its own PWM range (PWM_MAX[] /
//  PWM_MIN[]). All three augers default to PWM 250 [2 Nov]. Auger B only
//  feeds at PWM 250 and up, so it can't run slowly: near the target it slows
//  down by pulsing (bursts at PWM 250 with pauses that get longer as the
//  target gets closer). A and C hold a steady 250 with no slowdown.
//  main_checker has the same engine for bench tuning.
//
//  FLOW   DOSE -> ACK -> dose the hoppers in DOSE_ORDER (rice hull, wood ash,
//         then compost last) -> DONE, repeated every 500 ms until GOT.
//  STOP   while dosing: augers off at once, DONE with what was dosed so far.
//  FAULTS augers off, DONE with what was dosed so far, the fault stays on the
//         LCD and WT sends flag F until STOP arrives (operator presses *).
//           no weight change while the auger runs  -> "EMPTY: <hopper>"
//           load cell stops answering               -> "SENSOR: <hopper>"
//           hopper takes longer than HOPPER_TIMEOUT_MS -> "TIMEOUT: <hopper>"
//         The old sketch let the operator refill and resume. After a fault
//         the dose still ends: refill and start a new dose. (RESUME, added
//         3 Nov, is only for a dose a power cut interrupted.)
//  HW     [30 Oct] the three hopper weights for the dashboard, sent only when
//         one moved more than 5 g since the last HW (AgriLink.h: noise is not
//         sent), at most every 500 ms, and every 10 s anyway.
//  LCD    idle: the ESP32's menu (DISP frames). During a dose: this sketch's
//         own screens. No frame from the ESP32 for LINK_LOST_MS while idle
//         -> "NO ESP32 LINK / CHECK UART WIRES".
//  LCD FIX [1 Nov]: until then the LCD stayed blank with this sketch. Every
//         screen goes through lcdShow(), which padded each line with
//         snprintf("%-*s"). The Mega's snprintf (avr-libc) does not know the
//         "*" width and printed an empty string, so lcdShow() saw "nothing
//         changed" and never wrote to the LCD (no [LCD] lines either). It now
//         pads by hand in padLine(). Tested on the bench: the keypad menu, a
//         dose, the EMPTY fault and * all show on the glass. Don't use "*"
//         widths or %f in printf formats on the Mega.
//
//  NOT DONE HERE (TODO, team decision)
//    - mixer motor and discharge gate: the old sketch had no pins for them
//      either, only a 3 s "MIXING..." placeholder, which is left out.
//    - FreeRTOS: CLAUDE.md says the dosing firmware runs on FreeRTOS, but
//      the bench-tested 3-hopper code is a plain loop(), so it stays a loop.
//
//  SAVED STATE [3 Nov] (dose_state.h): the Mega gets no warning of a power cut,
//  so it saves to EEPROM while it runs: at every step of a dose, and every 5 s
//  while an auger turns. What it does with the save at the next power-on:
//    a dose was running  NO AUGER MOVES. The hopper that was running is weighed,
//                        and the LCD asks "RESUME A 600g?" / "312g #=YES *=NO"
//                        (flag R). # on the keypad (RESUME) carries on from there,
//                        * (STOP) ends the dose and logs what really came out.
//                        "WEIGHT CHANGED!" in place of the first line: the scale
//                        doesn't fit the save (material added or taken out while
//                        off, or no reading), so the saved grams are shown.
//    DONE had no GOT     DONE is sent again, so the dose log gets its row.
//    always              the scale zeros and the learned stop-early grams come back.
//
//  ZERO AT STARTUP (TARE_ON_BOOT): since 3 Nov only a hopper with no saved zero
//  is zeroed at power-on, so a loaded hopper keeps its true weight. That hopper
//  MUST BE EMPTY then, which means the first start after flashing a new record
//  layout (STATE_MAGIC) or a blank EEPROM. After that the hoppers may stay
//  loaded at power-on. Zero them with 0 on the keypad after a hopper was moved
//  or rebuilt: a saved zero can't know that.
//
//  SERIAL MONITOR (115200 baud, any line ending), idle only, hoppers empty:
//    t = zero all three hoppers     a / b / c = zero one hopper
//    [LCD] lines show every screen; DEBUG_WEIGHTS prints the weights.
//
//  HOW ONE HOPPER IS DOSED (loss-in-weight: its weight goes DOWN as it feeds)
//    1. beginHopper()    weighs the hopper, then starts its auger. Grams
//                        dispensed = that first weight - the weight now.
//    2. updateDosing()   runs while the auger turns: reads the load cell,
//                        slows the auger near the target, checks for faults.
//    3. beginSettle()    cuts the auger stopEarlyG before the target, because
//                        material keeps falling for a moment after the cut.
//    4. finishSettle()   after SETTLE_MS, weighs again and learns how much fell
//                        after the cut. More than TOLERANCE_G short: a top-up
//                        (TRIM, at most MAX_TRIMS). Otherwise the hopper is done.
//    5. completeHopper() moves to the next hopper. After the last one,
//                        endDose() sends DONE to the ESP32.
//
//  WHERE TO FIND THINGS (top to bottom)
//    settings, PIN MAP   feature switches and every pin
//    LCD                 lcdShow() and its copy on Serial Monitor
//    hoppers             calibration, auger speed, finish and fault limits
//    process state       the machine's states (MixState) and time limits
//    AgriLink to ESP32   the frames this board sends, and the retry memory
//    saved state         saving to EEPROM, and what happens to it at power-on
//    dosing              the five steps above, and resuming after a power cut
//    frame handling      what each frame from the ESP32 does
//    setup(), loop()     start-up, then the jobs that run round and round
// =====================================================================
#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>
#include <EEPROM.h>
#include "AgriLink.h"
#include "dose_state.h"        // what is saved across a power-off

#define DEBUG_WEIGHTS 0        // 1 = print the three hopper weights once a second
#define MIRROR_LCD    1        // 1 = copy every LCD screen to Serial Monitor as [LCD] |line 1|line 2|
#define MIRROR_MIN_MS 500UL    // fastest repeat for the same screen with new numbers
#define TARE_ON_BOOT  1        // 1 = at power-on, zero a hopper with no saved zero (must be empty), 0 = use CAL_OFFSET
#define TARE_SAMPLES  20       // readings averaged when zeroing (about 2 s per hopper)
#define TARE_WAIT_MS  1500UL   // pause before zeroing or weighing at power-on, so the cells settle

// ============================== PIN MAP ==============================
// (unchanged from FINAL_agridose_mega.ino)
// LCD 16x2 I2C. SDA/SCL are fixed hardware pins on the Mega 2560.
#define LCD_SDA_PIN 20
#define LCD_SCL_PIN 21
#define LCD_ADDR    0x27       // use the address your I2C scan prints
#define LCD_COLS    16
#define LCD_ROWS    2
#define LCD_BUF     (LCD_COLS + 5)

// ESP32 link on Serial2 (fixed hardware pins: TX2 = 16, RX2 = 17)
#define MEGA_LINK   Serial2
#define LINK_TX_PIN 16
#define LINK_RX_PIN 17

// HX711 load-cell amplifiers
#define HX_A_SCK 36
#define HX_A_DT  37
#define HX_B_SCK 38
#define HX_B_DT  39
#define HX_C_SCK 40
#define HX_C_DT  41

// IBT_2 motor drivers (hopper augers)
#define AUG_A_RPWM 3           // A rice hull
#define AUG_A_LPWM 2
#define AUG_A_REN  24
#define AUG_A_LEN  25
#define AUG_B_RPWM 6           // B compost
#define AUG_B_LPWM 7
#define AUG_B_REN  22
#define AUG_B_LEN  23
#define AUG_C_RPWM 5           // C wood ash
#define AUG_C_LPWM 4
#define AUG_C_REN  29
#define AUG_C_LEN  28
// =====================================================================

// Must stay ABOVE every function that takes a Hopper&.
// Everything remembered about one hopper: its pins, its calibration, and
// how the current dose is going.
struct Hopper {
  const char* label;  // name on the LCD and Serial Monitor: HULL, COMPOST, ASH
  HX711 scale;        // its load-cell amplifier
  uint8_t dtPin, sckPin;        // HX711 data and clock pins
  uint8_t rpwm, lpwm, ren, len; // IBT_2 motor driver pins
  float calFactor;    // raw HX711 counts per gram
  long  offset;       // raw HX711 reading at 0 g
  bool  zeroed;       // offset comes from really zeroing this hopper, not from CAL_OFFSET
  float targetG;      // this hopper's share of the dose, grams
  float startG;       // absolute grams at the start of the current run segment
  float base;         // grams dispensed before the current run segment
  float dispensed;    // total grams dispensed this dose
  float lastProgG;    // empty-detection reference
  uint32_t lastProgAt; // when lastProgG last went up
  float weightG;      // latest absolute weight (stock reading)
  uint32_t lastReadAt; // when the HX711 last gave a reading while dosing
  bool  haveW;        // weightG holds a real reading
  bool  done;         // finished, or nothing to dose this time
  float stopEarlyG;   // learned: how far before the target to cut the auger
  bool  learned;      // false until the first coast has been measured
  float stopDispG;    // dispensed grams at the moment the auger was cut
  uint8_t trims;      // trim passes used on this hopper this dose
  int   pwm;          // PWM last sent to the auger
  int   pwmMax, pwmMin; // this hopper's PWM range (PWM_MAX[] / PWM_MIN[])
  bool  pulse;        // true = slow down by pulsing at pwmMax (PULSE[])
  uint32_t pulseFrom; // start of the current run segment, the bursts count from here
  const char* mode;   // "BULK" / "SLOW" / "SETL" / "TRIM" / "OK"
};

// ------------------------------------------------------- small helpers --
// Grams (float) to whole decigrams, rounded to the nearest one.
int32_t gToDg(float g) {
  return (int32_t)(g >= 0 ? g * 10.0f + 0.5f : g * 10.0f - 0.5f);
}

// Writes decigrams as grams with one decimal: 2486 -> "248.6", -3 -> "-0.3".
// (The Mega's snprintf cannot print floats, so this uses whole numbers.)
void fmtDg(char* out, size_t size, int32_t dg) {
  int32_t a = dg < 0 ? -dg : dg;
  snprintf(out, size, "%s%ld.%ld", dg < 0 ? "-" : "", (long)(a / 10), (long)(a % 10));
}

// ---------------------------------------------------------- LCD ---
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// What the LCD shows right now, so lcdShow() can skip writes that change nothing.
char shownL0[LCD_BUF] = "", shownL1[LCD_BUF] = "";

// LCD mirror on Serial Monitor (unchanged): a new top line prints at once,
// changing numbers on the same screen at most every MIRROR_MIN_MS.
bool mirrorPending = false;
uint32_t mirrorAt = 0;
char mirroredL0[LCD_BUF] = "";

// Prints the current screen to Serial Monitor as [LCD] |line 1|line 2|.
void mirrorPrint() {
  Serial.print("[LCD] |"); Serial.print(shownL0);
  Serial.print("|");       Serial.print(shownL1); Serial.println("|");
  strcpy(mirroredL0, shownL0);
  mirrorAt = millis();
  mirrorPending = false;
}

void mirrorService() {           // called from loop()
#if MIRROR_LCD
  if (mirrorPending && millis() - mirrorAt >= MIRROR_MIN_MS) mirrorPrint();
#endif
}

// Copies text into out, cut or padded with spaces to exactly LCD_COLS
// characters. Done by hand: the Mega's snprintf does not know the "*" width,
// so the "%-*s" used here before printed nothing and the LCD stayed blank
// (found on the bench 1 Nov 2026).
void padLine(char* out, const char* in) {
  uint8_t n = 0;
  while (n < LCD_COLS && in[n]) { out[n] = in[n]; n++; }
  while (n < LCD_COLS) out[n++] = ' ';
  out[LCD_COLS] = 0;
}

// Every screen goes through here. Each line is padded to the full 16 columns
// so no old text is left behind. Nothing is written if the text did not
// change: I2C writes are slow, and this is called on every loop pass while
// dosing.
void lcdShow(const char* l0, const char* l1) {
  char p0[LCD_BUF], p1[LCD_BUF];
  padLine(p0, l0);
  padLine(p1, l1);
  if (strcmp(p0, shownL0) == 0 && strcmp(p1, shownL1) == 0) return;
#if MIRROR_LCD
  if (strcmp(p0, shownL0) != 0 && mirrorPending) mirrorPrint();   // old screen's last numbers first
#endif
  strcpy(shownL0, p0); strcpy(shownL1, p1);
  lcd.setCursor(0, 0); lcd.print(p0);
  lcd.setCursor(0, 1); lcd.print(p1);
#if MIRROR_LCD
  if (strcmp(shownL0, mirroredL0) != 0) mirrorPrint();   // new screen: show it now
  else mirrorPending = true;                             // same screen, new numbers
#endif
}

// The ESP32's latest menu screen, shown whenever the Mega is idle.
char lastMenuL0[LCD_BUF] = "AGRIDOSE MEGA";
char lastMenuL1[LCD_BUF] = "WAITING ESP32";

// ------------------------------------------------------------ hoppers --
Hopper hoppers[3];             // 0 = rice hull, 1 = compost, 2 = wood ash

// What was in the EEPROM at power-on (see the saved state section).
SavedState saved;
bool haveSaved = false;

// Calibration of 30 Oct 2026 (261 g wrench), after the hoppers were moved
// apart so none touches another (before that, weight in one hopper showed
// on the others too). FACTOR = (raw with known weight - OFFSET) / grams.
// 31 Oct 2026: the blue B jug was fixed firmly to its block, then every
// hopper was checked with the wrench at the middle and at a side.
// A read 260.9 g (0.5% with position): kept.
// B read 242.7 g (0.07% with position): 110.9196 -> 103.1425.
// C read 253.6 g (0.45% with position, likely re-seated while B was fixed):
// 201.7758 -> 196.0279.
// Before: A -205.6835, B -123.9307, C 193.5400 (Oct 28, other wiring).
#define CAL_FACTOR_A 193.9649f
#define CAL_FACTOR_B 103.1425f
#define CAL_FACTOR_C 196.0279f
#define CAL_OFFSET_A 477353L       // empty-hopper zeros at the 30 Oct start-up; only used
#define CAL_OFFSET_B -440148L      // when an HX711 does not answer at power-on (TARE_ON_BOOT
#define CAL_OFFSET_C 33314L        // zeroes them again at every start)

// ---- adaptive auger speed (placeholder, tune on the real augers) ----
// Each hopper has its own PWM range. Near the target an auger slows down
// by lowering the PWM, or, with PULSE true, by pulsing: bursts at PWM_MAX
// with pauses. In BULK the burst fills the whole period (always
// on), in the slow zone it shrinks to PULSE_ON_MIN_MS at the target, and
// trims always use the shortest burst. For a motor that only turns near
// full power. 1 Nov 2026: auger B feeds only at PWM 250 and up, so B pulses.
// 2 Nov 2026: with wood ash, 3 s runs of auger C moved about 3 g at PWM 170,
// nothing at 180 and 200, 8 g at 220 and 72 g at 250 (too fast to stop on
// target), so C first held a steady 220.
// 2 Nov 2026, later: the user made PWM 250 the default for all three augers,
// after clearing most of A's hull clog. PWM_MAX = PWM_MIN, so A and C hold a
// steady 250 with no slowdown near the target. B still pulses, now at 250
// (its earlier doses were tested at 255). Not dose-tested yet. If C overshoots
// its small share of ash, set its PULSE to true.
// Keep these the same as main_checker.
//                       A hull  B compost  C ash
const int  PWM_MAX[3] = {  250,     250,     250 };  // far from target
const int  PWM_MIN[3] = {  250,     250,     250 };  // just before target (must still turn the auger); not used by a pulsed hopper
const bool PULSE[3]   = { false,    true,   false };
const uint32_t PULSE_PERIOD_MS = 1000UL;  // one burst plus its pause
const uint32_t PULSE_ON_MIN_MS = 300UL;   // shortest burst; long enough for the auger to start turning
const float SLOW_ZONE_FRAC  = 0.30f;   // slow zone = 30% of that hopper's target...
const float SLOW_ZONE_MIN_G = 25.0f;   // ...but never smaller than this
const float SLOW_ZONE_MAX_G = 300.0f;  // ...and never bigger than this

// ---- exact-weight finish (unchanged) ----
const float    STOP_EARLY_START_G = 3.0f;    // first-dose guess, then learned per hopper
const float    STOP_EARLY_MAX_G   = 40.0f;   // learned value is clamped to 0..this
const float    TOLERANCE_G        = 2.0f;    // accept a hopper if at most this far under target
const uint8_t  MAX_TRIMS          = 3;       // slow top-up passes after the first stop
const uint32_t SETTLE_MS          = 800UL;   // let falling material and the load cell settle

// ---- empty-hopper detection (unchanged) ----
const float    EMPTY_MIN_PROGRESS_G = 1.0f;    // must gain this many grams...
const uint32_t EMPTY_WINDOW_MS      = 5000UL;  // ...within this time, or it's "empty"

// How fast the auger should go for the remaining grams: 1.0 = full speed
// (BULK), falling to 0.0 at the target (SLOW), 0.0 in trims (TRIM).
// Sets h.mode for the LCD.
float speedFrac(Hopper& h, float remainingG) {
  if (h.trims > 0) { h.mode = "TRIM"; return 0; }
  float zone = h.targetG * SLOW_ZONE_FRAC;
  if (zone < SLOW_ZONE_MIN_G) zone = SLOW_ZONE_MIN_G;
  if (zone > SLOW_ZONE_MAX_G) zone = SLOW_ZONE_MAX_G;
  if (remainingG >= zone) { h.mode = "BULK"; return 1; }
  float f = remainingG / zone;                 // 1.0 at zone edge -> 0.0 at target
  if (f < 0) f = 0;
  h.mode = "SLOW";
  return f;
}

// The PWM to send to the auger right now. Normal hopper: the PWM ramps from
// pwmMax down to pwmMin. Pulsed hopper: pwmMax during the burst, 0 in the pause.
int augerPwm(Hopper& h, float remainingG) {
  float f = speedFrac(h, remainingG);
  if (!h.pulse) return h.pwmMin + (int)((h.pwmMax - h.pwmMin) * f);
  uint32_t onMs = PULSE_ON_MIN_MS + (uint32_t)((PULSE_PERIOD_MS - PULSE_ON_MIN_MS) * f);
  return (millis() - h.pulseFrom) % PULSE_PERIOD_MS < onMs ? h.pwmMax : 0;
}

// Motor driver switches. An auger turns only when its driver is enabled AND
// runAuger() gives it a PWM. RPWM turns it; LPWM stays at 0 (one direction only).
void enableDriver(Hopper& h) { digitalWrite(h.ren, HIGH); digitalWrite(h.len, HIGH); }
// PWM to 0, then both enables LOW, so this auger can't turn.
void disableDriver(Hopper& h) {
  analogWrite(h.rpwm, 0);
  analogWrite(h.lpwm, 0);
  digitalWrite(h.ren, LOW);
  digitalWrite(h.len, LOW);
  h.pwm = 0;
}
// Called on every loop pass while dosing, so it only writes a change.
void runAuger(Hopper& h, int pwm) {
  if (pwm == h.pwm) return;
  analogWrite(h.rpwm, pwm); analogWrite(h.lpwm, 0); h.pwm = pwm;
}
// Used by endDose() and STOP, so no auger is left running.
void stopAllAugers() { for (uint8_t i = 0; i < 3; i++) disableDriver(hoppers[i]); }

// Runs once at power-on: gives each hopper its pins and calibration, switches
// every driver off, then brings back each saved zero or zeroes the hopper (see
// ZERO AT STARTUP above). Returns true if any hopper got a new zero to save.
bool setupHoppers() {
  bool zeroChanged = false;
  hoppers[0].label = "HULL";
  hoppers[0].dtPin = HX_A_DT; hoppers[0].sckPin = HX_A_SCK;
  hoppers[0].rpwm = AUG_A_RPWM; hoppers[0].lpwm = AUG_A_LPWM;
  hoppers[0].ren = AUG_A_REN;   hoppers[0].len = AUG_A_LEN;
  hoppers[0].calFactor = CAL_FACTOR_A; hoppers[0].offset = CAL_OFFSET_A;

  hoppers[1].label = "COMPOST";
  hoppers[1].dtPin = HX_B_DT; hoppers[1].sckPin = HX_B_SCK;
  hoppers[1].rpwm = AUG_B_RPWM; hoppers[1].lpwm = AUG_B_LPWM;
  hoppers[1].ren = AUG_B_REN;   hoppers[1].len = AUG_B_LEN;
  hoppers[1].calFactor = CAL_FACTOR_B; hoppers[1].offset = CAL_OFFSET_B;

  hoppers[2].label = "ASH";
  hoppers[2].dtPin = HX_C_DT; hoppers[2].sckPin = HX_C_SCK;
  hoppers[2].rpwm = AUG_C_RPWM; hoppers[2].lpwm = AUG_C_LPWM;
  hoppers[2].ren = AUG_C_REN;   hoppers[2].len = AUG_C_LEN;
  hoppers[2].calFactor = CAL_FACTOR_C; hoppers[2].offset = CAL_OFFSET_C;

  for (uint8_t i = 0; i < 3; i++) {
    hoppers[i].pwmMax = PWM_MAX[i];
    hoppers[i].pwmMin = PWM_MIN[i];
    hoppers[i].pulse = PULSE[i];
    hoppers[i].pulseFrom = 0;
    // Driver off first, so no auger can move during start-up.
    pinMode(hoppers[i].rpwm, OUTPUT);
    pinMode(hoppers[i].lpwm, OUTPUT);
    pinMode(hoppers[i].ren, OUTPUT);
    pinMode(hoppers[i].len, OUTPUT);
    disableDriver(hoppers[i]);

    hoppers[i].scale.begin(hoppers[i].dtPin, hoppers[i].sckPin);
    hoppers[i].scale.set_scale(hoppers[i].calFactor);
    hoppers[i].scale.set_offset(hoppers[i].offset);
    hoppers[i].weightG = 0;
    hoppers[i].dispensed = 0;
    hoppers[i].done = true;
    hoppers[i].haveW = false;
    hoppers[i].zeroed = false;
    // Learned values carry on from the last save; a new board starts with the first guess.
    bool wasLearned = haveSaved && (saved.learnedMask & (1 << i));
    hoppers[i].stopEarlyG = wasLearned ? saved.stopEarlyG[i] : STOP_EARLY_START_G;
    hoppers[i].learned = wasLearned;
    hoppers[i].stopDispG = 0;
    hoppers[i].trims = 0;
    hoppers[i].mode = "OK";
  }

#if TARE_ON_BOOT
  bool anyToZero = false;
  for (uint8_t i = 0; i < 3; i++) {
    if (!(haveSaved && (saved.zeroedMask & (1 << i)))) anyToZero = true;
  }
  if (anyToZero) {
    lcdShow("ZEROING SCALES", "HOPPERS EMPTY!");
    Serial.println("No saved zero for a hopper -- zeroing it now, it must be empty.");
    delay(TARE_WAIT_MS);
  }
#endif

  for (uint8_t i = 0; i < 3; i++) {
    Hopper& h = hoppers[i];
    bool ok = h.scale.wait_ready_timeout(1000);
    Serial.print(h.label);
    Serial.print(ok ? ": HX711 OK" : ": HX711 NOT FOUND");
    if (haveSaved && (saved.zeroedMask & (1 << i))) {
      h.scale.set_offset(saved.offset[i]);     // the zero from before the power-off
      h.offset = saved.offset[i];
      h.zeroed = true;
      Serial.print("  saved zero");
    }
#if TARE_ON_BOOT
    else if (ok) {
      // No saved zero: zero this hopper now, and save it.
      h.scale.tare(TARE_SAMPLES);
      h.offset = h.scale.get_offset();
      h.zeroed = true;
      zeroChanged = true;
      Serial.print("  zeroed");
    } else {
      // Its HX711 does not answer: keep CAL_OFFSET, and try again at the next power-on.
      Serial.print("  using CAL_OFFSET");
    }
#endif
    Serial.print("  factor ");
    Serial.print(h.calFactor, 4);
    Serial.print("  offset ");
    Serial.println(h.offset);
  }
  return zeroChanged;
}

// Zeroes one hopper. Only called while idle. The caller saves the new zero.
void tareHopper(uint8_t i) {
  Hopper& h = hoppers[i];
  if (!h.scale.wait_ready_timeout(1000)) {
    Serial.print(h.label); Serial.println(": HX711 not responding, not zeroed");
    return;
  }
  h.scale.tare(TARE_SAMPLES);
  h.offset = h.scale.get_offset();
  h.zeroed = true;
  h.haveW = false;                 // restart the smoothed stock reading from the new zero
  Serial.print(h.label); Serial.print(": zeroed, offset "); Serial.println(h.offset);
}

// Mass dispensed in the current (or last) dose, all hoppers added, in dg.
int32_t totalDispensedDg() {
  float g = 0;
  for (uint8_t i = 0; i < 3; i++) g += hoppers[i].dispensed;
  return gToDg(g);
}

// ------------------------------------------------------ process state --
// The machine is always in one of these states; updateProcess() runs the current one.
//   M_IDLE      waiting for a command; the LCD shows the ESP32's menu
//   M_DOSING    an auger is running (curHopper)
//   M_SETTLING  the auger has been cut; waiting SETTLE_MS before weighing again
//   M_FAULT     the dose ended on a fault; waiting for STOP (the * key)
//   M_RESULT    "DOSE DONE" or "DOSE STOPPED" on the LCD for RESULT_SHOW_MS
//   M_RESUME    power came back with a dose unfinished: augers off, the LCD
//               asks, and it waits for RESUME (#) or STOP (*)
enum MixState { M_IDLE, M_DOSING, M_SETTLING, M_FAULT, M_RESULT, M_RESUME };
MixState mstate = M_IDLE;
int8_t curHopper = -1;         // hopper index (0 = hull, 1 = compost, 2 = ash) being dosed
int8_t curPos = -1;            // its position in DOSE_ORDER

// Dosing order by hopper index: rice hull, wood ash, compost last.
const uint8_t DOSE_ORDER[3] = {0, 2, 1};

const uint32_t RESULT_SHOW_MS    = 3000UL;    // "DOSE DONE" screen, then back to the menu
const uint32_t HOPPER_TIMEOUT_MS = 240000UL;  // per-hopper safety cutoff [1 Nov 2026: 120 s -> 240 s for doses up to 900 g; compost 630 g at ~10 g/s takes ~90 s]
const uint32_t SENSOR_STALE_MS   = 400UL;     // no HX711 reading this long while dosing -> stop
const uint32_t LINK_LOST_MS      = 6000UL;    // no frame from the ESP32 this long while idle -> warn

uint32_t hopperStartedAt = 0, resultShownAt = 0, settleStartedAt = 0;

// The dose being run now (from its DOSE command).
uint8_t curSeq = 0;
char    curRecipe = 'A';
int32_t curTargetDg = 0;

// I = idle, D = dosing, F = fault, R = waiting to resume. Sent in every WT frame.
char currentFlag() {
  if (mstate == M_DOSING || mstate == M_SETTLING) return AGRILINK_FLAG_DOSING;
  if (mstate == M_FAULT) return AGRILINK_FLAG_FAULT;
  if (mstate == M_RESUME) return AGRILINK_FLAG_RESUME;
  return AGRILINK_FLAG_IDLE;
}

// "Idle" for commands: nothing running, and not waiting for STOP after a fault.
bool machineIdle() { return mstate == M_IDLE || mstate == M_RESULT; }

// ------------------------------------------------- AgriLink to ESP32 --
AgriRx linkRx;

// Seq memory: the last sequenced command and the answer we gave. A retry
// with the same seq gets this answer again and is not run a second time.
uint8_t lastSeq = 0;           // 0 = nothing remembered
char lastReply[24] = "";

// DONE waiting for the ESP32's GOT.
bool doneUnacked = false;
uint8_t doneSeq = 0;
char doneRecipe = 'A';
int32_t doneTargetDg = 0, doneActualDg = 0;
uint32_t doneSentAt = 0;

uint32_t lastWtAt = 0;
uint32_t lastHwAt = 0;
int32_t  sentHwDg[3] = {0, 0, 0};  // the hopper weights in the last HW frame
bool     hwDue = true;             // send HW at the next chance (boot, ESP32 restart)
uint32_t lastFrameAt = 0;      // last good frame from the ESP32
bool linkUp = false;

// Link diagnostics for Serial Monitor (same idea as the old sketch).
uint32_t diagBytes = 0, lastDiagAt = 0;
#define LINK_DIAG_MS 3000UL

// Every frame goes out through here. agriSend() adds the $ start and the CRC
// check (AgriLink.h).
void sendBody(const char* body) { agriSend(MEGA_LINK, body); }

// Sends an ACK/NAK for seq and remembers it.
void reply(uint8_t seq, const char* body) {
  strncpy(lastReply, body, sizeof(lastReply) - 1);
  lastReply[sizeof(lastReply) - 1] = 0;
  lastSeq = seq;
  sendBody(body);
}

// ACK,<seq>: the command was accepted.
void ack(uint8_t seq) {
  char b[12];
  snprintf(b, sizeof(b), "ACK,%u", seq);
  reply(seq, b);
}

// seq 0 means we could not read the seq: answer, but remember nothing.
void nak(uint8_t seq, const char* reason) {
  char b[24];
  snprintf(b, sizeof(b), "NAK,%u,%s", seq, reason);
  if (seq == 0) sendBody(b);
  else reply(seq, b);
  Serial.print("  refused: "); Serial.println(b);
}

// WT: grams dispensed so far and the state flag, every AGRILINK_WT_PERIOD_MS.
// It is also a sign of life: if no frames arrive, the ESP32 shows "Mega offline".
void sendWt() {
  char b[24];
  snprintf(b, sizeof(b), "WT,%ld,%c", (long)totalDispensedDg(), currentFlag());
  sendBody(b);
  lastWtAt = millis();
}

// DONE: the dose's result. doneService() repeats it until the ESP32 answers
// GOT, so one lost frame cannot lose a dose from the log.
void sendDone() {
  char b[40];
  snprintf(b, sizeof(b), "DONE,%u,%c,%ld,%ld", doneSeq, doneRecipe, (long)doneTargetDg, (long)doneActualDg);
  sendBody(b);
  doneSentAt = millis();
}

void wtService()   { if (millis() - lastWtAt >= AGRILINK_WT_PERIOD_MS) sendWt(); }

// Hopper weights: only a change past the deadband goes out (see AgriLink.h HW).
void hwService() {
  uint32_t since = millis() - lastHwAt;
  if (since < AGRILINK_HW_MIN_MS) return;
  // Only the hopper being dosed is read during a dose; the others keep their
  // last reading.
  int32_t now[3];
  for (uint8_t i = 0; i < 3; i++) now[i] = (int32_t)lroundf(hoppers[i].weightG * 10.0f);
  if (!hwDue && since < AGRILINK_HW_REFRESH_MS && !agriHwChanged(sentHwDg, now, AGRILINK_HW_DEADBAND_DG)) return;
  char b[40];
  snprintf(b, sizeof(b), "HW,%ld,%ld,%ld", (long)now[0], (long)now[1], (long)now[2]);
  sendBody(b);
  memcpy(sentHwDg, now, sizeof(now));
  lastHwAt = millis();
  hwDue = false;
}
void doneService() { if (doneUnacked && millis() - doneSentAt >= AGRILINK_DONE_REPEAT_MS) sendDone(); }

// ------------------------------------------------------- saved state --
// See dose_state.h for what is saved, when, and how a power cut during a save
// is survived. The rule that keeps the grams right: an auger never starts until
// a save that can account for it is in the EEPROM (beginHopper).
StateWriter stateWriter;
uint32_t stateCounter = 0;     // the counter of the newest save
uint32_t lastSaveAt = 0;

uint8_t eeRead(uint16_t addr) { return EEPROM.read(addr); }
void eeWrite(uint16_t addr, uint8_t value) { EEPROM.write(addr, value); }

// Starts a save of the machine as it is now, written a byte at a time by
// saveService(). Safe to call while an auger runs.
void queueSave() {
  SavedState s;
  memset(&s, 0, sizeof(s));
  s.counter = ++stateCounter;
  for (uint8_t i = 0; i < 3; i++) {
    const Hopper& h = hoppers[i];
    s.offset[i] = h.offset;
    s.stopEarlyG[i] = h.stopEarlyG;
    s.dispensed[i] = h.dispensed;
    if (h.zeroed)  s.zeroedMask  |= (uint8_t)(1 << i);
    if (h.learned) s.learnedMask |= (uint8_t)(1 << i);
    if (h.done)    s.doneMask    |= (uint8_t)(1 << i);
  }
  if (doneUnacked) {
    s.phase = STATE_PHASE_DONE_WAIT;
    s.seq = doneSeq;
    s.recipe = doneRecipe;
    s.targetDg = doneTargetDg;
    s.actualDg = doneActualDg;
  } else if (mstate == M_DOSING || mstate == M_SETTLING || mstate == M_RESUME) {
    s.phase = STATE_PHASE_RUNNING;
    s.seq = curSeq;
    s.recipe = curRecipe;
    s.targetDg = curTargetDg;
  } else {
    s.phase = STATE_PHASE_NONE;
  }
  s.curPos = curPos;
  if (curPos >= 0) {
    const Hopper& run = hoppers[DOSE_ORDER[curPos]];
    s.trims = run.trims;
    s.base = run.base;
    s.startG = run.startG;
  }
  stateWriteBegin(stateWriter, s);
  lastSaveAt = millis();
}

// Writes the next byte if the EEPROM is ready for it. Never waits.
void saveService() {
  if (eeprom_is_ready()) stateWriteStep(stateWriter, eeRead, eeWrite);
}

// Saves and waits until it is all in the EEPROM (at most about 230 ms). Only
// with the augers off: a running auger would overshoot during the wait.
void saveNow() {
  queueSave();
  while (stateWriter.busy) saveService();
}

// From loop(): while an auger runs, saves its grams every STATE_SAVE_EVERY_MS.
void stateService() {
  if (mstate == M_DOSING && millis() - lastSaveAt >= STATE_SAVE_EVERY_MS) queueSave();
  saveService();
}

// Reads the EEPROM. Called before setupHoppers(), which needs the saved zeros.
void loadSavedState() {
  stateWriter.slot = STATE_SLOTS - 1;          // nothing saved yet: the first save goes in slot 0
  stateWriter.busy = false;
  haveSaved = stateFindNewest(eeRead, saved, stateWriter.slot);
  if (haveSaved) {
    stateCounter = saved.counter;
    Serial.print("Saved state found, save no. "); Serial.println(stateCounter);
  } else {
    Serial.println("No saved state in the EEPROM");
  }
}

// After setupHoppers(): picks up whatever the power cut interrupted. Never
// starts an auger. Takes about 3 s if a dose was running (the weighing).
void restoreSavedDose() {
  if (!haveSaved || saved.phase == STATE_PHASE_NONE) return;
  for (uint8_t i = 0; i < 3; i++) hoppers[i].dispensed = saved.dispensed[i];

  if (saved.phase == STATE_PHASE_DONE_WAIT) {
    doneSeq = saved.seq;
    doneRecipe = saved.recipe;
    doneTargetDg = saved.targetDg;
    doneActualDg = saved.actualDg;
    doneUnacked = true;                        // doneService() sends it until the ESP32 answers GOT
    Serial.print("Dose "); Serial.print(doneSeq);
    Serial.println(" ended before the ESP32 confirmed it -- sending its DONE again");
    return;
  }

  const AgriRecipe* r = agriRecipe(saved.recipe);
  if (saved.phase != STATE_PHASE_RUNNING || r == nullptr || saved.curPos < -1 || saved.curPos > 2) {
    for (uint8_t i = 0; i < 3; i++) hoppers[i].dispensed = 0;
    Serial.println("Saved dose can't be used -- dropped");
    return;
  }

  curSeq = saved.seq;
  curRecipe = saved.recipe;
  curTargetDg = saved.targetDg;
  curPos = saved.curPos;
  int32_t hullDg, compostDg, ashDg;
  agriSplitDose(*r, curTargetDg, hullDg, compostDg, ashDg);
  hoppers[0].targetG = hullDg / 10.0f;
  hoppers[1].targetG = compostDg / 10.0f;
  hoppers[2].targetG = ashDg / 10.0f;
  for (uint8_t i = 0; i < 3; i++) {
    hoppers[i].done = saved.doneMask & (1 << i);
    hoppers[i].base = 0;
  }

  // The hopper that was running: the scale also counts what came out after the
  // last save, as long as it agrees with that save (stateResumeGrams).
  bool trusted = true;
  if (curPos >= 0 && !hoppers[DOSE_ORDER[curPos]].done) {
    uint8_t i = DOSE_ORDER[curPos];
    Hopper& h = hoppers[i];
    curHopper = i;
    h.trims = saved.trims;
    h.base = saved.base;
    h.startG = saved.startG;
    delay(TARE_WAIT_MS);                       // the load cell has only just powered up
    if (h.scale.wait_ready_timeout(1000)) {
      float w = h.scale.get_units(10);
      h.weightG = w;
      h.haveW = true;
      h.dispensed = stateResumeGrams(saved.dispensed[i], h.base + (h.startG - w), trusted);
      Serial.print("  "); Serial.print(h.label);
      Serial.print(": saved "); Serial.print(saved.dispensed[i], 1);
      Serial.print(" g, scale says "); Serial.print(h.base + (h.startG - w), 1);
      Serial.println(trusted ? " g -- using the scale" : " g -- doesn't fit, using the saved grams");
    } else {
      trusted = false;
      Serial.print("  "); Serial.print(h.label); Serial.println(": no reading from HX711, using the saved grams");
    }
  }

  mstate = M_RESUME;
  char l0[LCD_BUF], l1[LCD_BUF];
  if (trusted) snprintf(l0, sizeof(l0), "RESUME %c %ldg?", curRecipe, (long)(curTargetDg / 10));
  else snprintf(l0, sizeof(l0), "WEIGHT CHANGED!");
  snprintf(l1, sizeof(l1), "%ldg #=YES *=NO", (long)(totalDispensedDg() / 10));
  lcdShow(l0, l1);
  Serial.print("Dose "); Serial.print(curSeq);
  Serial.println(" was cut off by a power cut. Augers stay off: # on the keypad continues it, * ends it.");
}

// ----------------------------------------------------------- dosing --
// The dosing screen, e.g. "DOSING HULL" / "148.2/150g SLOW". A reading just
// after the start can be slightly negative, so it shows as 0 until it passes 0.
void showDosing(Hopper& h) {
  char l0[LCD_BUF], l1[LCD_BUF], got[10];
  snprintf(l0, sizeof(l0), "DOSING %s", h.label);
  fmtDg(got, sizeof(got), gToDg(h.dispensed < 0 ? 0 : h.dispensed));
  snprintf(l1, sizeof(l1), "%s/%dg %s", got, (int)(h.targetG + 0.5f), h.mode);
  lcdShow(l0, l1);
}

// Ends the dose: augers off, DONE goes out now and repeats until GOT.
void endDose() {
  stopAllAugers();
  doneSeq = curSeq;
  doneRecipe = curRecipe;
  doneTargetDg = curTargetDg;
  doneActualDg = totalDispensedDg();
  doneUnacked = true;
  sendDone();
  saveNow();                     // a power cut now can't lose the result: the augers are off
  Serial.print("Dose "); Serial.print(doneSeq);
  Serial.print(" ended: "); Serial.print(doneActualDg);
  Serial.print(" dg of "); Serial.print(doneTargetDg); Serial.println(" dg");
}

// "DOSE DONE" / "DOSE STOPPED" with the dispensed mass, for RESULT_SHOW_MS.
void showResult(const char* headline) {
  char l1[LCD_BUF], got[10];
  fmtDg(got, sizeof(got), doneActualDg);
  snprintf(l1, sizeof(l1), "%s g", got);
  mstate = M_RESULT;
  resultShownAt = millis();
  lcdShow(headline, l1);
  sendWt();                      // tell the ESP32 at once (flag I)
}

// A fault ends the dose and waits for STOP. what = "EMPTY", "SENSOR", "TIMEOUT".
void enterFault(const char* what, uint8_t i) {
  endDose();
  mstate = M_FAULT;
  char l0[LCD_BUF];
  snprintf(l0, sizeof(l0), "%s: %s", what, hoppers[i].label);
  lcdShow(l0, "PRESS * TO CLEAR");
  Serial.print("FAULT "); Serial.println(l0);
  sendWt();                      // tell the ESP32 at once (flag F)
}

// Reads a fresh starting weight and starts the auger (unchanged). Returns
// false if the HX711 isn't responding. Takes about 1 s (10 readings).
// Also called for every top-up (trim), so each one restarts the timeout and
// the EMPTY timer.
bool beginHopper(uint8_t i) {
  Hopper& h = hoppers[i];
  curHopper = i;
  if (!h.scale.wait_ready_timeout(1000)) return false;
  h.startG = h.scale.get_units(10);
  h.weightG = h.startG; h.haveW = true;
  h.lastProgG = h.dispensed;
  h.lastProgAt = millis();
  h.lastReadAt = millis();
  hopperStartedAt = millis();
  saveNow();                                   // in the EEPROM before the auger turns: the start weight is the reference for a power cut
  enableDriver(h);
  h.pulseFrom = millis();                      // a pulsed auger starts with a burst
  runAuger(h, augerPwm(h, h.targetG - h.dispensed));
  showDosing(h);
  return true;
}

// Starts the hopper at position pos in DOSE_ORDER.
void beginAtPos(int pos) {
  curPos = pos;
  uint8_t i = DOSE_ORDER[pos];
  mstate = M_DOSING;
  if (!beginHopper(i)) enterFault("SENSOR", i);
}

// Position in DOSE_ORDER of the next hopper that still has something to dose.
int nextPosFrom(int from) {
  for (int p = from; p < 3; p++) if (!hoppers[DOSE_ORDER[p]].done) return p;
  return -1;
}

// Moves on to the next hopper, or finishes the dose.
void advanceToNext() {
  int pos = nextPosFrom(curPos + 1);
  if (pos < 0) { endDose(); showResult("DOSE DONE"); return; }
  beginAtPos(pos);
}

// This hopper is finished: auger off, print its result, go to the next one.
void completeHopper(Hopper& h) {
  disableDriver(h);
  h.done = true;
  h.mode = "OK";
  showDosing(h);
  saveNow();                     // so a power cut now does not run this hopper again
  Serial.print("  "); Serial.print(h.label);
  Serial.print(" done: "); Serial.print(h.dispensed, 1);
  Serial.print(" g of "); Serial.print(h.targetG, 1);
  Serial.print(" g, trims "); Serial.println(h.trims);
  advanceToNext();
}

// Splits the dose by recipe and starts the first hopper.
void startDose(uint8_t seq, const AgriRecipe& r, int grams) {
  int32_t totalDg = (int32_t)grams * 10;
  int32_t hullDg, compostDg, ashDg;
  agriSplitDose(r, totalDg, hullDg, compostDg, ashDg);

  curSeq = seq;
  curRecipe = r.letter;
  curTargetDg = totalDg;
  hoppers[0].targetG = hullDg / 10.0f;
  hoppers[1].targetG = compostDg / 10.0f;
  hoppers[2].targetG = ashDg / 10.0f;
  for (uint8_t i = 0; i < 3; i++) {
    hoppers[i].done = (hoppers[i].targetG <= 0); // nothing to dose here: skip this hopper
    hoppers[i].base = 0;
    hoppers[i].dispensed = 0;
    hoppers[i].trims = 0;
    hoppers[i].mode = "BULK";
  }
  Serial.print("  recipe "); Serial.print(r.letter);
  Serial.print(", targets (g) hull "); Serial.print(hoppers[0].targetG, 1);
  Serial.print("  compost "); Serial.print(hoppers[1].targetG, 1);
  Serial.print("  ash "); Serial.println(hoppers[2].targetG, 1);

  mstate = M_DOSING;
  lcdShow("STARTING DOSE", "PLEASE WAIT...");
  sendWt();                      // flag D goes out before the first 1 s weighing
  int pos = nextPosFrom(0);
  if (pos < 0) { endDose(); showResult("DOSE DONE"); return; }
  beginAtPos(pos);
}

// # on the keypad after a power cut: carries on with the hopper that was
// running, or moves to the next one if it had finished. Only ever called by
// the RESUME command, so an auger never starts by itself at power-on.
void resumeDose() {
  Serial.println("Resuming the dose");
  lcdShow("RESUMING DOSE", "PLEASE WAIT...");
  mstate = M_DOSING;
  sendWt();                      // flag D goes out before the 1 s weighing
  if (curPos < 0 || hoppers[DOSE_ORDER[curPos]].done) { advanceToNext(); return; }
  uint8_t i = DOSE_ORDER[curPos];
  Hopper& h = hoppers[i];
  curHopper = i;
  if (h.targetG - h.dispensed <= TOLERANCE_G) { completeHopper(h); return; }  // it had already got there
  h.base = h.dispensed;          // keep what is already out; beginHopper weighs afresh from here
  if (!beginHopper(i)) enterFault("SENSOR", i);
}

// Back to idle after the result screen or a cleared fault: the menu returns
// to the LCD and the stock readings start again.
void returnToIdle() {
  mstate = M_IDLE;
  lastFrameAt = millis();        // the ESP32 is quiet during a dose on purpose; restart the link timer
  for (uint8_t i = 0; i < 3; i++) hoppers[i].haveW = false;  // re-seed the stock readings
  lcdShow(lastMenuL0, lastMenuL1);
  sendWt();
}

// Auger is cut: stop it and give the material and the load cell time to settle (unchanged).
void beginSettle(Hopper& h) {
  disableDriver(h);
  h.stopDispG = h.dispensed;
  h.mode = "SETL";
  settleStartedAt = millis();
  mstate = M_SETTLING;
  showDosing(h);
}

// After the settle delay: weigh again, learn the coast, then top up or finish (unchanged).
void finishSettle() {
  Hopper& h = hoppers[curHopper];
  if (!h.scale.wait_ready_timeout(1000)) { enterFault("SENSOR", curHopper); return; }

  float w = h.scale.get_units(3);
  h.weightG = w; h.haveW = true;
  // Grams from earlier passes, plus what this pass took out.
  float finalG = h.base + (h.startG - w);
  h.dispensed = finalG;
  float shortG = h.targetG - finalG;           // positive = still under target

  // Learned only from the main run, not from top-ups.
  if (h.trims == 0) {
    // coast = grams that still fell after the cut. Aim to cut this early next time.
    float coast = finalG - h.stopDispG;
    if (coast < 0) coast = 0;
    // First time: use this coast. After that: average it with the old value, so
    // one odd dose can't swing it far.
    float next = h.learned ? 0.5f * h.stopEarlyG + 0.5f * coast : coast;
    h.stopEarlyG = constrain(next, 0.0f, STOP_EARLY_MAX_G);
    h.learned = true;
  }

  if (shortG > TOLERANCE_G && h.trims < MAX_TRIMS) {
    h.trims++;
    h.base = finalG;                           // keep what is already in
    mstate = M_DOSING;
    if (!beginHopper(curHopper)) enterFault("SENSOR", curHopper);
    return;
  }

  // Known limit: after MAX_TRIMS a hopper is accepted however short it is.
  completeHopper(h);                           // within tolerance, over target, or out of trims
}

// Runs on every loop pass while an auger runs: checks the time limit and the
// load cell, works out the grams from each new reading, then sets the auger speed.
// Known limit: readings are not filtered here, so one bad reading (an HX711
// readout error) can end the main run early. The top-ups then fill in what is
// missing, up to MAX_TRIMS.
void updateDosing() {
  Hopper& h = hoppers[curHopper];

  // Time limit. The timer restarts at each top-up (beginHopper).
  if (millis() - hopperStartedAt > HOPPER_TIMEOUT_MS) {
    enterFault("TIMEOUT", curHopper);
    return;
  }

  // The HX711 gives a new reading about every 100 ms. If it goes quiet
  // (unplugged, broken wire) stop at once instead of feeding blind.
  if (!h.scale.is_ready() && millis() - h.lastReadAt > SENSOR_STALE_MS) {
    Serial.print("  "); Serial.print(h.label); Serial.println(": no reading from HX711 -- stopping");
    enterFault("SENSOR", curHopper);
    return;
  }

  if (h.scale.is_ready()) {
    float w = h.scale.get_units(1);
    h.lastReadAt = millis();
    h.weightG = w; h.haveW = true;
    h.dispensed = h.base + (h.startG - w);   // loss-in-weight: weight goes DOWN as it feeds
    float remaining = h.targetG - h.dispensed;

    // Progress check for the EMPTY fault: the timer restarts each time another
    // EMPTY_MIN_PROGRESS_G has come out.
    if (h.dispensed >= h.lastProgG + EMPTY_MIN_PROGRESS_G) {
      h.lastProgG = h.dispensed;
      h.lastProgAt = millis();
    }

    // Main run: cut early, to allow for the coast. Top-up: run up to the target.
    float stopAt = (h.trims > 0) ? 0.0f : h.stopEarlyG;
    if (remaining <= stopAt) { beginSettle(h); return; }

    if (millis() - h.lastProgAt > EMPTY_WINDOW_MS) {
      if (h.trims > 0) { completeHopper(h); return; }   // trims are best effort, no fault
      enterFault("EMPTY", curHopper);
      return;
    }
  }
  // On every loop pass, not only on a new reading (about every 100 ms), so
  // a pulsed auger's bursts are timed to the millisecond.
  runAuger(h, augerPwm(h, h.targetG - h.dispensed));
  showDosing(h);
}

// Round-robin read of all three load cells while idle (unchanged).
void updateIdleWeights() {
  static uint8_t next = 0;
  static uint32_t lastRead = 0;

  // One hopper per pass, at most every 50 ms, so the link keeps being read.
  if (millis() - lastRead >= 50) {
    lastRead = millis();
    Hopper& h = hoppers[next];
    next = (next + 1) % 3;
    if (h.scale.is_ready()) {
      float w = h.scale.get_units(1);
      if (!h.haveW) { h.weightG = w; h.haveW = true; }
      else h.weightG = h.weightG * 0.7f + w * 0.3f; // smoothed: 70% old, 30% new
    }
  }
}

// Does the work of the current state. Called on every loop pass.
void updateProcess() {
  switch (mstate) {
    case M_IDLE:
      updateIdleWeights();
      if (millis() - lastFrameAt > LINK_LOST_MS) lcdShow("NO ESP32 LINK", "CHECK UART WIRES");
      break;
    case M_DOSING:
      updateDosing();
      break;
    case M_SETTLING:
      if (millis() - settleStartedAt >= SETTLE_MS) finishSettle();
      break;
    case M_RESULT:
      if (millis() - resultShownAt >= RESULT_SHOW_MS) returnToIdle();
      break;
    case M_FAULT:
      break;                     // waits for STOP from the ESP32
    case M_RESUME:
      updateIdleWeights();       // the auger stays off; the hopper gauges keep working
      break;                     // waits for RESUME or STOP from the ESP32
  }
}

// Zeroes all three hoppers. Takes about 6 s and the Mega does nothing
// else meanwhile, so the ESP32 will see "Mega offline" for that time.
void tareAll() {
  if (mstate == M_RESULT) mstate = M_IDLE;
  lcdShow("ZEROING SCALES", "HOPPERS EMPTY!");
  for (uint8_t i = 0; i < 3; i++) tareHopper(i);
  for (uint8_t i = 0; i < 3; i++) hoppers[i].dispensed = 0;   // WT goes back to 0
  saveNow();                     // the new zeros last until the next zeroing
  lastFrameAt = millis();
  lcdShow(lastMenuL0, lastMenuL1);
  sendWt();
}

// -------------------------------------------------------- frame handling --
// What STOP does depends on the state: it ends a dose, or clears a fault.
void handleStop() {
  if (mstate == M_DOSING || mstate == M_SETTLING || mstate == M_RESUME) {
    Serial.println(mstate == M_RESUME ? "STOP: the unfinished dose ends here" : "STOP: augers stopped by the operator");
    endDose();
    showResult("DOSE STOPPED");
  } else if (mstate == M_FAULT) {
    Serial.println("STOP: fault cleared");
    returnToIdle();
  } else {
    stopAllAugers();             // nothing is running, but make sure
  }
}

// Checks the request in this order. The first problem gets a NAK and the dose
// does not start.
void handleDose(uint8_t seq, char* f[], uint8_t n) {
  if (n != 4) { nak(seq, AGRILINK_NAK_ARGS); return; }
  const AgriRecipe* r = (strlen(f[2]) == 1) ? agriRecipe(f[2][0]) : nullptr;
  if (r == nullptr) { nak(seq, AGRILINK_NAK_RECIPE); return; }
  int32_t grams;
  if (!agriParseInt(f[3], grams)) { nak(seq, AGRILINK_NAK_ARGS); return; }
  if (grams < AGRILINK_DOSE_MIN_G || grams > AGRILINK_DOSE_MAX_G) { nak(seq, AGRILINK_NAK_RANGE); return; }
  if (!machineIdle() || doneUnacked) { nak(seq, AGRILINK_NAK_BUSY); return; }
  ack(seq);                      // answer first: starting takes about 1 s (first weighing)
  startDose(seq, *r, (int)grams);
}

// DISP,<line 1>,<line 2>: the ESP32's menu. Stored always, shown only when idle.
void handleDisp(const char* l0, const char* l1) {
  strncpy(lastMenuL0, l0, LCD_COLS); lastMenuL0[LCD_COLS] = 0;
  strncpy(lastMenuL1, l1, LCD_COLS); lastMenuL1[LCD_COLS] = 0;
  if (mstate == M_IDLE) lcdShow(lastMenuL0, lastMenuL1);
}

// One good frame from the ESP32 (its CRC has already been checked).
void handleFrame(char* body) {
  lastFrameAt = millis();
  if (!linkUp) {
    linkUp = true;
    Serial.println("ESP32 link OK -- frames are arriving on RX2 (pin 17)");
  }
  if (strncmp(body, "DISP,", 5) != 0) { Serial.print("RX: "); Serial.println(body); }

  // Split at the commas: f[0] is the verb, f[1] onward are its fields.
  char* f[6];
  uint8_t n = agriSplit(body, f, 6);
  const char* verb = f[0];

  // ---- unsequenced: no answer ----
  if (strcmp(verb, "DISP") == 0) {
    handleDisp(n > 1 ? f[1] : "", n > 2 ? f[2] : "");
    return;
  }
  if (strcmp(verb, "HELLO") == 0) {
    lastSeq = 0;                 // the ESP32 restarted and may reuse old seq numbers
    lastReply[0] = 0;
    hwDue = true;                // and it has no hopper weights yet
    Serial.println("ESP32 restarted -- seq memory cleared");
    return;
  }
  if (strcmp(verb, "GOT") == 0) {
    uint8_t s;
    if (n == 2 && agriParseSeq(f[1], s) && doneUnacked && s == doneSeq) {
      doneUnacked = false;
      queueSave();               // the save no longer has to carry this result
      Serial.println("DONE delivered to the ESP32");
    }
    return;
  }

  // ---- sequenced: always answered with ACK or NAK ----
  bool known = strcmp(verb, "PING") == 0 || strcmp(verb, "TARE") == 0 ||
               strcmp(verb, "STOP") == 0 || strcmp(verb, "DOSE") == 0 ||
               strcmp(verb, "RESUME") == 0;
  uint8_t seq = 0;
  bool haveSeq = (n >= 2) && agriParseSeq(f[1], seq);
  if (!known) { nak(haveSeq ? seq : 0, AGRILINK_NAK_UNKNOWN); return; }
  if (!haveSeq) { nak(0, AGRILINK_NAK_ARGS); return; }

  if (seq == lastSeq) {          // a retry: same answer, do not run it again
    sendBody(lastReply);
    Serial.println("  repeat of the last seq -- answered from memory, not run again");
    return;
  }

  if (strcmp(verb, "DOSE") == 0) { handleDose(seq, f, n); return; }
  if (n != 2) { nak(seq, AGRILINK_NAK_ARGS); return; }

  if (strcmp(verb, "PING") == 0) {
    ack(seq);
  } else if (strcmp(verb, "STOP") == 0) {
    ack(seq);
    handleStop();
  } else if (strcmp(verb, "RESUME") == 0) {
    if (mstate != M_RESUME) { nak(seq, AGRILINK_NAK_BUSY); return; }
    ack(seq);                    // answer first: starting takes about 1 s (first weighing)
    resumeDose();
  } else if (strcmp(verb, "TARE") == 0) {
    if (!machineIdle()) { nak(seq, AGRILINK_NAK_BUSY); return; }
    ack(seq);                    // answer first: zeroing takes about 6 s
    tareAll();
  }
}

// Reads every waiting byte. agriRxFeed() returns true when a whole frame with a
// correct CRC has arrived.
void pollLink() {
  while (MEGA_LINK.available()) {
    char c = (char)MEGA_LINK.read();
    diagBytes++;
    if (agriRxFeed(linkRx, c)) handleFrame(linkRx.buf);
  }
}

// Prints a link report while no good frames are arriving. Idle only: during
// a dose the ESP32 sends nothing unless * is pressed, which is normal.
void linkDiag() {
  if (mstate != M_IDLE) { lastDiagAt = millis(); return; }
  if (linkUp && millis() - lastFrameAt > LINK_LOST_MS) {
    linkUp = false;
    Serial.println("ESP32 link LOST -- no frames for 6 s");
    diagBytes = 0; linkRx.good = 0; linkRx.bad = 0;
    lastDiagAt = millis();
  }
  if (linkUp || millis() - lastDiagAt < LINK_DIAG_MS) return;
  lastDiagAt = millis();
  Serial.print("Waiting for ESP32 on RX2 (pin 17): ");
  Serial.print(diagBytes);    Serial.print(" bytes, ");
  Serial.print(linkRx.good);  Serial.print(" good frames, ");
  Serial.print(linkRx.bad);   Serial.print(" bad frames");
  if (diagBytes == 0) Serial.println("  -> nothing arriving: check ESP32 GPIO26 -> Mega pin 17 and the shared GND");
  else if (linkRx.good == 0) Serial.println("  -> data is garbled: check the shared GND and that both sides use 57600 baud");
  else Serial.println();
  diagBytes = 0; linkRx.good = 0; linkRx.bad = 0;
}

// Serial Monitor commands: t = zero all, a/b/c = zero one hopper (idle only).
void handleSerialCmd() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n' || c == ' ') continue;
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if (c != 't' && (c < 'a' || c > 'c')) continue;
    if (mstate != M_IDLE) { Serial.println("Busy -- zeroing is only allowed when idle"); continue; }
    if (c == 't') tareAll();
    else { tareHopper(c - 'a'); saveNow(); }
  }
}

#if DEBUG_WEIGHTS
// One line a second with the three weights (set DEBUG_WEIGHTS to 1).
void debugWeights() {
  static uint32_t at = 0;
  if (millis() - at < 1000) return;
  at = millis();
  Serial.print("hull ");       Serial.print(hoppers[0].weightG, 1);
  Serial.print(" g | compost "); Serial.print(hoppers[1].weightG, 1);
  Serial.print(" g | ash ");     Serial.print(hoppers[2].weightG, 1);
  Serial.print(" g | flag ");    Serial.println(currentFlag());
}
#endif

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== AgriDose Mega (AgriLink) ===");
  MEGA_LINK.begin(AGRILINK_BAUD);
  agriRxReset(linkRx);

  // If Serial Monitor stops after "Starting LCD...", the Mega can't talk to
  // the LCD: check SDA = 20, SCL = 21, and LCD_ADDR against an I2C scan.
  Serial.println("Starting LCD...");
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdShow("AGRIDOSE MEGA", "LCD OK");
  Serial.println("LCD ready");
  delay(800);

  // What was saved before the power went off (dose_state.h).
  loadSavedState();

  // Brings back the saved zeros. A hopper with none is zeroed now, which takes
  // about 2 s per hopper, and it must be empty.
  bool zeroChanged = setupHoppers();

  // An unfinished dose puts the Mega in M_RESUME and its question on the LCD.
  restoreSavedDose();
  if (zeroChanged) saveNow();
  if (mstate == M_IDLE) lcdShow(lastMenuL0, lastMenuL1);

  // Bytes that arrived while the scales were zeroing overflowed the 64-byte
  // receive buffer; throw them away so the first frame parsed is a whole one.
  // (The ESP32 sends HELLO again before its first command, so none is lost.)
  while (MEGA_LINK.available()) MEGA_LINK.read();
  lastFrameAt = millis();
  lastDiagAt = millis();

  // Tell the ESP32 this board has just started.
  sendBody("HELLO");
  sendWt();
  Serial.println("Waiting for frames from the ESP32...");
  Serial.println("Commands: t = zero all, a/b/c = zero one hopper (idle, hoppers empty)");
}

// The main loop. Each job takes a turn and returns quickly, so frames from the
// ESP32 keep being read during a dose. The longer waits are the weighing at the
// start of a hopper (about 1 s) and zeroing (about 6 s).
void loop() {
  handleSerialCmd();  // zero commands typed on Serial Monitor
  pollLink();         // frames from the ESP32
  updateProcess();    // the dosing work for the current state
  wtService();        // WT every 200 ms
  hwService();        // hopper weights, when they change
  doneService();      // repeat DONE until GOT
  stateService();     // save to EEPROM, a byte at a time
  mirrorService();    // LCD copy on Serial Monitor
  linkDiag();         // link report while no frames arrive
#if DEBUG_WEIGHTS
  debugWeights();
#endif
}
