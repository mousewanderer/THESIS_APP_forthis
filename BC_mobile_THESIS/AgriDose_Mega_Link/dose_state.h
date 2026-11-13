// =====================================================================
//  dose_state.h -- what the Mega remembers across a power-off (3 Nov 2026).
//  Mega only; the ESP32 keeps its own records on the SD card.
//
//  WHY IT SAVES AHEAD OF TIME
//    The Mega gets no warning when the power goes: no pin senses it, so
//    nothing can be saved "at the moment of switching off". It saves while
//    it runs instead, and after a power cut the newest save is what it knows.
//
//  WHAT IS SAVED (one record, struct SavedState)
//    Always        each scale's zero and the learned stop-early grams, so a
//                  loaded hopper still reads its true weight after a power-off.
//    During a dose recipe, mass, seq, which hopper, the grams from each hopper,
//                  and what the running hopper weighed when it started. Also
//                  when a fault has paused the dose.
//    After a dose  the result, until the ESP32 answers GOT, so a power cut
//                  can't lose the row in the dose log.
//
//  WHEN (the calls are in AgriDose_Mega_Link.ino)
//    Auger off     before every hopper start and top-up, when a hopper
//                  finishes, when a fault pauses a dose, when a dose ends,
//                  after zeroing.
//    Auger running every STATE_SAVE_EVERY_MS, one byte per loop pass, so dosing
//                  never waits for the EEPROM (a byte takes about 3.4 ms).
//
//  A POWER CUT DURING A SAVE
//    Records go round a ring of STATE_SLOTS slots, and each ends in a CRC. A
//    half-written record fails its CRC, so the record before it is used. The
//    newest good record is the one with the highest counter.
//
//  WEAR
//    An EEPROM byte lasts about 100,000 writes, so the ring takes about
//    1.6 million saves. A 600 g dose makes about 30, which is over 50,000 doses.
//
//  No Arduino calls here: EEPROM access comes in as function pointers, so
//  tests/test_dose_state.cpp can cut the power at every byte on a laptop.
// =====================================================================
#pragma once
#include "AgriLink.h"   // agriCrc16()

#define STATE_MAGIC   0xA1   // change it whenever SavedState changes: older records are then ignored
#define STATE_SLOTS   16     // the ring; 16 records fit easily in the Mega's 4096-byte EEPROM
#define STATE_EEPROM_SIZE 4096

#define STATE_SAVE_EVERY_MS 5000UL  // how often the grams are saved while an auger runs

// How far the scale may disagree with the last save before it is not believed
// (stateResumeGrams). Bench value not measured yet.
#define STATE_DRIFT_G  10.0f   // less than the save: allows for the zero moving a little across a power cut
#define STATE_UNSEEN_G 150.0f  // more than the save: the fastest auger (about 24 g/s) for 5 s, plus what falls after the cut

// What the machine was doing when this record was saved.
enum StatePhase : uint8_t {
  STATE_PHASE_NONE = 0,   // no dose: only the zeros and learned values matter
  STATE_PHASE_RUNNING,    // a dose was running: ask the operator at power-on
  STATE_PHASE_DONE_WAIT,  // a dose had ended, but the ESP32 had not answered GOT
  STATE_PHASE_FAULT       // a fault had paused the dose: the grams are exact, don't ask the scale
};

struct __attribute__((packed)) SavedState {
  uint8_t  magic;         // STATE_MAGIC
  uint32_t counter;       // goes up by one with every save; the highest is the newest
  // kept across every power-off
  uint8_t  zeroedMask;    // bit i: hopper i has a real zero in offset[i]
  int32_t  offset[3];     // raw HX711 reading at 0 g
  uint8_t  learnedMask;   // bit i: stopEarlyG[i] was learned from a real dose
  float    stopEarlyG[3];
  // the dose
  uint8_t  phase;         // StatePhase
  uint8_t  seq;
  char     recipe;        // 'A', 'B' or 'C'
  int32_t  targetDg;      // the whole dose
  int32_t  actualDg;      // the result (STATE_PHASE_DONE_WAIT only)
  int8_t   curPos;        // position in DOSE_ORDER of the hopper being dosed
  uint8_t  doneMask;      // bit i: hopper i is finished
  float    dispensed[3];  // grams from each hopper at this save
  // the running hopper: its grams can be worked out again from these two
  uint8_t  trims;         // top-ups used
  float    base;          // grams dispensed before this run of the auger
  float    startG;        // what the hopper weighed when this run started
  uint16_t crc;           // must stay last
};

static_assert(sizeof(SavedState) * STATE_SLOTS <= STATE_EEPROM_SIZE, "the state ring does not fit in the EEPROM");

typedef uint8_t (*StateReadByte)(uint16_t addr);
typedef void    (*StateWriteByte)(uint16_t addr, uint8_t value);

static inline uint16_t stateSlotAddr(uint8_t slot) { return (uint16_t)(slot * sizeof(SavedState)); }

static inline uint16_t stateCrc(const SavedState& s) {
  return agriCrc16((const char*)&s, sizeof(SavedState) - sizeof(s.crc));
}

static inline bool stateValid(const SavedState& s) {
  return s.magic == STATE_MAGIC && s.crc == stateCrc(s);
}

// Reads every slot and keeps the newest good record. False if there is none
// (a new board, or a record layout from an older build). slot is left alone then.
static inline bool stateFindNewest(StateReadByte read, SavedState& newest, uint8_t& slot) {
  bool found = false;
  for (uint8_t i = 0; i < STATE_SLOTS; i++) {
    SavedState s;
    uint8_t* p = (uint8_t*)&s;
    for (uint16_t b = 0; b < sizeof(SavedState); b++) p[b] = read(stateSlotAddr(i) + b);
    if (stateValid(s) && (!found || s.counter > newest.counter)) {
      newest = s;
      slot = i;
      found = true;
    }
  }
  return found;
}

// ---- writing a record a little at a time ----
struct StateWriter {
  SavedState rec;     // the record being written
  uint8_t    slot;    // the slot it goes in
  uint16_t   pos;     // its next byte
  bool       busy;    // true until every byte is in the EEPROM
};

// Takes a record to write. A write still going is given up, and the new record
// goes in the same slot, because that slot is half-written anyway.
static inline void stateWriteBegin(StateWriter& w, const SavedState& rec) {
  if (!w.busy) w.slot = (uint8_t)((w.slot + 1) % STATE_SLOTS);
  w.rec = rec;
  w.rec.magic = STATE_MAGIC;
  w.rec.crc = stateCrc(w.rec);
  w.pos = 0;
  w.busy = true;
}

// Does at most one real write per call, and skips bytes that already match.
// The caller only calls this when the EEPROM is ready for the next byte.
static inline void stateWriteStep(StateWriter& w, StateReadByte read, StateWriteByte write) {
  if (!w.busy) return;
  const uint8_t* p = (const uint8_t*)&w.rec;
  while (w.pos < sizeof(SavedState)) {
    uint16_t addr = stateSlotAddr(w.slot) + w.pos;
    uint8_t  value = p[w.pos++];
    if (read(addr) != value) { write(addr, value); return; }
  }
  w.busy = false;
}

// ---- after a power cut ----
// How many grams came out of the hopper that was running.
//   savedG    the grams in the last save, up to 5 s old
//   weighedG  worked out from the scale: base + (startG - the weight now). This
//             also counts what came out after the last save, so it is the better number.
// Material added to or taken from the hopper while the power was off would fool
// the scale. So if it disagrees too much with the save, the saved grams are used,
// and trusted comes back false so the LCD can warn the operator.
// Written so a NaN from a bad reading also counts as not trusted.
static inline float stateResumeGrams(float savedG, float weighedG, bool& trusted) {
  trusted = weighedG >= savedG - STATE_DRIFT_G && weighedG <= savedG + STATE_UNSEEN_G;
  return trusted ? weighedG : savedG;
}
