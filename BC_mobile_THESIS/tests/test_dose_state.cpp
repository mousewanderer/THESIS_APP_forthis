// Laptop unit test for dose_state.h (no board needed): the saved record, the
// ring, and above all a power cut at every single byte of a save.
//
// Build and run from the project folder (MSYS2 g++):
//   g++ -std=c++11 -Wall -I AgriDose_Mega_Link tests/test_dose_state.cpp -o test_dose_state
//   ./test_dose_state
#include <stdio.h>
#include <string.h>
#include "dose_state.h"

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) failures++;
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// A pretend EEPROM. A new chip reads 0xFF everywhere.
static uint8_t ee[STATE_EEPROM_SIZE];
static long eeWrites = 0;
static uint8_t eeRead(uint16_t a) { return ee[a]; }
static void eeWrite(uint16_t a, uint8_t v) { ee[a] = v; eeWrites++; }
static void eeBlank() { memset(ee, 0xFF, sizeof(ee)); eeWrites = 0; }

static SavedState makeRecord(uint32_t counter) {
  SavedState s;
  memset(&s, 0, sizeof(s));
  s.counter = counter;
  s.zeroedMask = 7;
  s.offset[0] = 477353; s.offset[1] = -440148; s.offset[2] = 33314;
  s.learnedMask = 2;
  s.stopEarlyG[1] = 2.0f + counter;
  s.phase = STATE_PHASE_RUNNING;
  s.seq = 5;
  s.recipe = 'A';
  s.targetDg = 6000;
  s.curPos = 1;
  s.doneMask = 1;
  s.dispensed[0] = 150.2f;
  s.dispensed[2] = (float)counter;
  s.trims = 1;
  s.base = 12.5f;
  s.startG = 880.25f;
  return s;
}

// Saves a record all the way, the way the sketch does.
static void saveFully(StateWriter& w, uint32_t counter) {
  stateWriteBegin(w, makeRecord(counter));
  while (w.busy) stateWriteStep(w, eeRead, eeWrite);
}

int main() {
  SavedState got;
  uint8_t slot = 99;

  // ---- nothing saved yet ----
  eeBlank();
  check(!stateFindNewest(eeRead, got, slot), "a blank EEPROM (all 0xFF) has no record");
  check(slot == 99, "slot is left alone when nothing is found");
  memset(ee, 0x00, sizeof(ee));
  check(!stateFindNewest(eeRead, got, slot), "an all-zero EEPROM has no record");
  srand(1234);
  for (size_t i = 0; i < sizeof(ee); i++) ee[i] = (uint8_t)rand();
  check(!stateFindNewest(eeRead, got, slot), "random bytes (an old prototype's data) have no record");

  // ---- save, read back ----
  eeBlank();
  StateWriter w;
  memset(&w, 0, sizeof(w));
  w.slot = STATE_SLOTS - 1;
  saveFully(w, 1);
  check(w.slot == 0, "the first save goes in slot 0");
  check(stateFindNewest(eeRead, got, slot) && slot == 0, "the record is found again");
  SavedState want = makeRecord(1);
  check(got.counter == 1 && got.offset[1] == want.offset[1] && got.recipe == 'A' && got.targetDg == 6000 &&
        got.curPos == 1 && got.doneMask == 1 && got.trims == 1 && got.seq == 5 && got.phase == STATE_PHASE_RUNNING &&
        got.base == 12.5f && got.startG == 880.25f && got.dispensed[0] == 150.2f && got.stopEarlyG[1] == want.stopEarlyG[1],
        "every field comes back the same");

  // ---- the ring ----
  for (uint32_t c = 2; c <= 100; c++) saveFully(w, c);
  check(stateFindNewest(eeRead, got, slot) && got.counter == 100, "after 100 saves the newest is found");
  check(slot == (100 - 1) % STATE_SLOTS, "the ring has gone round to the right slot");

  // Carrying on after a restart: a new writer starts from the slot that was found.
  StateWriter after;
  memset(&after, 0, sizeof(after));
  after.slot = slot;
  saveFully(after, got.counter + 1);
  check(stateFindNewest(eeRead, got, slot) && got.counter == 101 && slot == (100 % STATE_SLOTS), "saving carries on after a restart");

  // ---- wear: every slot gets its turn ----
  bool everySlotUsed = true;
  for (uint8_t i = 0; i < STATE_SLOTS; i++) {
    SavedState s;
    memcpy(&s, ee + stateSlotAddr(i), sizeof(s));
    if (!stateValid(s)) everySlotUsed = false;
  }
  check(everySlotUsed, "the ring spreads the writes over all the slots");

  // ---- the point of it all: cut the power at every byte of a save ----
  // Whole-byte cut: the write of byte k never happened, nothing after it either.
  {
    bool allOk = true;
    uint8_t snapshot[sizeof(ee)];
    eeBlank();
    StateWriter base;
    memset(&base, 0, sizeof(base));
    base.slot = STATE_SLOTS - 1;
    for (uint32_t c = 1; c <= 20; c++) saveFully(base, c);   // a ring full of older records
    memcpy(snapshot, ee, sizeof(ee));

    for (int cutAfterWrites = 0; ; cutAfterWrites++) {
      memcpy(ee, snapshot, sizeof(ee));
      StateWriter t = base;
      stateWriteBegin(t, makeRecord(21));
      int n = 0;
      while (t.busy && n < cutAfterWrites) {
        long before = eeWrites;
        stateWriteStep(t, eeRead, eeWrite);
        if (eeWrites != before) n++;
      }
      bool finished = !t.busy;
      // the power is cut here
      SavedState r;
      uint8_t s = 0;
      bool found = stateFindNewest(eeRead, r, s);
      bool complete = n >= cutAfterWrites && finished;
      if (!found) allOk = false;
      else if (complete && r.counter != 21) allOk = false;
      else if (!complete && r.counter != 20 && r.counter != 21) allOk = false;
      // A record reported as 21 must really be record 21, every byte.
      if (found && r.counter == 21) {
        SavedState full = makeRecord(21);
        if (memcmp((uint8_t*)&r + 1, (uint8_t*)&full + 1, sizeof(SavedState) - 3) != 0) allOk = false;
      }
      if (finished) break;
    }
    check(allOk, "a cut before any byte of a save leaves a good record (the old one or the new one)");
  }

  // Torn-byte cut: byte k is left holding junk, as a write cut half-way can.
  {
    int wrongCount = 0, tested = 0;
    for (uint16_t k = 0; k < sizeof(SavedState); k++) {
      static const uint8_t JUNK[] = {0x00, 0xFF, 0x55, 0xAA};
      for (uint8_t j = 0; j < 4; j++) {
        eeBlank();
        StateWriter t;
        memset(&t, 0, sizeof(t));
        t.slot = STATE_SLOTS - 1;
        saveFully(t, 1);
        saveFully(t, 2);
        // the save of record 3: the bytes before k are in, byte k is torn, the rest are not.
        stateWriteBegin(t, makeRecord(3));
        const uint8_t* p = (const uint8_t*)&t.rec;
        uint16_t a = stateSlotAddr(t.slot);
        for (uint16_t b = 0; b < k; b++) ee[a + b] = p[b];
        bool sameAsRecord = (JUNK[j] == p[k]);
        ee[a + k] = JUNK[j];
        SavedState r;
        uint8_t s = 0;
        tested++;
        bool found = stateFindNewest(eeRead, r, s);
        if (!found) wrongCount++;
        else if (sameAsRecord && k == sizeof(SavedState) - 1) { if (r.counter != 3) wrongCount++; }
        else if (r.counter != 2) wrongCount++;
      }
    }
    printf("      (%d torn-byte cuts tested)\n", tested);
    check(wrongCount == 0, "a byte torn anywhere in a save falls back to the record before it");
  }

  // A new save that starts while the last one is still writing.
  {
    eeBlank();
    StateWriter t;
    memset(&t, 0, sizeof(t));
    t.slot = STATE_SLOTS - 1;
    saveFully(t, 1);
    stateWriteBegin(t, makeRecord(2));
    uint8_t slotOfTwo = t.slot;
    for (int i = 0; i < 10; i++) stateWriteStep(t, eeRead, eeWrite);    // part-way through
    stateWriteBegin(t, makeRecord(3));
    check(t.slot == slotOfTwo, "a save that overtakes a half-done one reuses its slot");
    check(stateFindNewest(eeRead, got, slot) && got.counter == 1, "while it writes, the last whole record is what would be found");
    while (t.busy) stateWriteStep(t, eeRead, eeWrite);
    check(stateFindNewest(eeRead, got, slot) && got.counter == 3, "then the overtaking save is found");
  }

  // One bit wrong anywhere: the CRC or the magic must catch it.
  {
    eeBlank();
    StateWriter t;
    memset(&t, 0, sizeof(t));
    t.slot = STATE_SLOTS - 1;
    saveFully(t, 1);
    saveFully(t, 2);
    int missed = 0;
    uint16_t a = stateSlotAddr(t.slot);
    for (uint16_t b = 0; b < sizeof(SavedState); b++) {
      for (uint8_t bit = 0; bit < 8; bit++) {
        ee[a + b] ^= (uint8_t)(1 << bit);
        if (!stateFindNewest(eeRead, got, slot) || got.counter != 1) missed++;
        ee[a + b] ^= (uint8_t)(1 << bit);
      }
    }
    check(missed == 0, "one flipped bit anywhere in a record: it is not believed");
  }

  // A record from an older layout (other magic) is ignored, however good its CRC.
  {
    eeBlank();
    SavedState old = makeRecord(5);
    old.magic = STATE_MAGIC + 1;
    old.crc = stateCrc(old);
    memcpy(ee, &old, sizeof(old));
    check(!stateFindNewest(eeRead, got, slot), "a record with another layout's magic is ignored");
  }

  // Writes that change nothing are skipped: same record again = 0 EEPROM writes.
  {
    eeBlank();
    StateWriter t;
    memset(&t, 0, sizeof(t));
    t.slot = 0;
    stateWriteBegin(t, makeRecord(7));
    uint8_t s = t.slot;
    while (t.busy) stateWriteStep(t, eeRead, eeWrite);
    long used = eeWrites;
    t.slot = (uint8_t)(s + STATE_SLOTS - 1) % STATE_SLOTS;              // the next begin lands on the same slot
    stateWriteBegin(t, makeRecord(7));
    while (t.busy) stateWriteStep(t, eeRead, eeWrite);
    check(used > 0 && eeWrites == used, "bytes that already match are not written again (saves wear)");
  }

  // ---- how the grams are worked out after a power cut ----
  {
    bool trusted = false;
    float g = stateResumeGrams(100.0f, 130.0f, trusted);
    check(trusted && g == 130.0f, "the scale shows more than the last save: believed (it counts the last seconds)");
    g = stateResumeGrams(100.0f, 100.0f, trusted);
    check(trusted && g == 100.0f, "the scale agrees with the save: believed");
    g = stateResumeGrams(100.0f, 93.0f, trusted);
    check(trusted && g == 93.0f, "a few grams under the save (the zero moved a little): believed");
    g = stateResumeGrams(100.0f, 89.0f, trusted);
    check(!trusted && g == 100.0f, "11 g under the save (material added while off): the saved grams, with a warning");
    g = stateResumeGrams(100.0f, 251.0f, trusted);
    check(!trusted && g == 100.0f, "151 g over the save (material taken out while off): the saved grams, with a warning");
    g = stateResumeGrams(100.0f, 250.0f, trusted);
    check(trusted && g == 250.0f, "exactly 5 s of the fastest auger: still believed");
    g = stateResumeGrams(0.0f, -500.0f, trusted);
    check(!trusted && g == 0.0f, "a refilled hopper that has not fed yet stays at 0 g");
    float nan = 0.0f / 0.0f;
    g = stateResumeGrams(100.0f, nan, trusted);
    check(!trusted && g == 100.0f, "a NaN from the scale is not believed");
  }

  // ---- the numbers the header comment claims ----
  printf("      (one record is %u bytes; the ring uses %u of %u EEPROM bytes)\n",
         (unsigned)sizeof(SavedState), (unsigned)(sizeof(SavedState) * STATE_SLOTS), (unsigned)STATE_EEPROM_SIZE);
  check(sizeof(SavedState) <= 80, "a record stays small enough to write in under 300 ms");

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
