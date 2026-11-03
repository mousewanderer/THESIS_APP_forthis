#include "config.h"
#if FAKE_MEGA
#include <Arduino.h>
#include "fake_mega.h"
#include "AgriLink.h"

#define FAKE_FLOW_DG_PER_100MS 10    // about 10 g per second
#define FAKE_FAULT_CLEAR_MS    20000UL  // a demo fault clears itself after this (as if * was pressed)

enum FakeState { F_IDLE, F_DOSING, F_FAULT };
static FakeState st = F_IDLE;

// Bytes waiting to be read by the link task.
static char   outBuf[512];
static size_t outLen = 0, outPos = 0;

static int32_t dispensedDg = 0, targetDg = 0, faultAtDg = 0;
static char    recipe = 'A';
static uint8_t doseSeq = 0;
static uint8_t lastSeq = 0;          // seq memory, like the real Mega
static char    lastReply[24] = "";
static bool    doneUnacked = false;
static int32_t doneActualDg = 0;
static uint32_t lastWtAt = 0, lastDoneAt = 0, lastFlowAt = 0, idleSince = 0, faultSince = 0;
static uint8_t demoCount = 0;

static void emit(const char* body) {
  char frame[AGRILINK_MAX_FRAME + 1];
  size_t n = agriBuildFrame(body, frame, sizeof(frame));
  if (n == 0 || outLen + n > sizeof(outBuf)) return;
  memcpy(outBuf + outLen, frame, n);
  outLen += n;
}

static void reply(uint8_t seq, const char* body) {
  lastSeq = seq;
  strncpy(lastReply, body, sizeof(lastReply) - 1);
  lastReply[sizeof(lastReply) - 1] = 0;
  emit(body);
}

static void ack(uint8_t seq) { char b[12]; snprintf(b, sizeof(b), "ACK,%u", seq); reply(seq, b); }
static void nak(uint8_t seq, const char* why) {
  char b[24];
  snprintf(b, sizeof(b), "NAK,%u,%s", seq, why);
  if (seq) reply(seq, b); else emit(b);
}

static void sendWt() {
  char b[24];
  char flag = st == F_DOSING ? 'D' : st == F_FAULT ? 'F' : 'I';
  snprintf(b, sizeof(b), "WT,%ld,%c", (long)dispensedDg, flag);
  emit(b);
  lastWtAt = millis();
}

static void sendDone() {
  char b[40];
  snprintf(b, sizeof(b), "DONE,%u,%c,%ld,%ld", doseSeq, recipe, (long)targetDg, (long)doneActualDg);
  emit(b);
  lastDoneAt = millis();
}

static void endDose() {
  doneActualDg = dispensedDg;
  doneUnacked = true;
  sendDone();
}

static void startDose(uint8_t seq, char r, int grams, bool withFault) {
  doseSeq = seq;
  recipe = r;
  targetDg = (int32_t)grams * 10;
  dispensedDg = 0;
  faultAtDg = withFault ? targetDg * 6 / 10 : -1;   // demo fault at 60 %
  st = F_DOSING;
  lastFlowAt = millis();
  sendWt();
  Serial.printf("[FAKE MEGA] dose seq %u: recipe %c, %d g%s\n", seq, r, grams, withFault ? " (will fault)" : "");
}

void fakeMegaBegin() {
  idleSince = millis();
  emit("HELLO");
}

void fakeMegaReceive(const char* text) {
  char body[AGRILINK_MAX_BODY + 1];
  strncpy(body, text, sizeof(body) - 1);
  body[sizeof(body) - 1] = 0;
  char* f[6];
  uint8_t n = agriSplit(body, f, 6);
  const char* verb = f[0];
  uint8_t seq;

  if (strcmp(verb, "DISP") == 0) return;                      // there is no LCD here
  if (strcmp(verb, "HELLO") == 0) { lastSeq = 0; return; }
  if (strcmp(verb, "GOT") == 0) {
    if (n == 2 && agriParseSeq(f[1], seq) && seq == doseSeq) doneUnacked = false;
    return;
  }
  if (n < 2 || !agriParseSeq(f[1], seq)) { nak(0, AGRILINK_NAK_ARGS); return; }
  if (seq == lastSeq) { emit(lastReply); return; }             // a retry: same answer, not run again

  bool idle = (st == F_IDLE);
  if (strcmp(verb, "PING") == 0) {
    ack(seq);
  } else if (strcmp(verb, "TARE") == 0) {
    if (!idle) { nak(seq, AGRILINK_NAK_BUSY); return; }
    ack(seq);
    dispensedDg = 0;
  } else if (strcmp(verb, "STOP") == 0) {
    ack(seq);
    if (st == F_DOSING) { endDose(); st = F_IDLE; idleSince = millis(); }
    else if (st == F_FAULT) { st = F_IDLE; idleSince = millis(); }
    sendWt();
  } else if (strcmp(verb, "DOSE") == 0) {
    int32_t grams;
    if (n != 4) { nak(seq, AGRILINK_NAK_ARGS); return; }
    if (strlen(f[2]) != 1 || agriRecipe(f[2][0]) == nullptr) { nak(seq, AGRILINK_NAK_RECIPE); return; }
    if (!agriParseInt(f[3], grams)) { nak(seq, AGRILINK_NAK_ARGS); return; }
    if (grams < AGRILINK_DOSE_MIN_G || grams > AGRILINK_DOSE_MAX_G) { nak(seq, AGRILINK_NAK_RANGE); return; }
    if (!idle || doneUnacked) { nak(seq, AGRILINK_NAK_BUSY); return; }
    ack(seq);
    startDose(seq, f[2][0], (int)grams, false);
  } else {
    nak(seq, AGRILINK_NAK_UNKNOWN);
  }
}

void fakeMegaTick() {
  uint32_t now = millis();

  if (st == F_DOSING && now - lastFlowAt >= 100) {
    lastFlowAt = now;
    dispensedDg += FAKE_FLOW_DG_PER_100MS + (int32_t)random(-3, 4);
    if (faultAtDg >= 0 && dispensedDg >= faultAtDg) {
      st = F_FAULT;                                      // pretend a hopper ran empty
      faultSince = now;
      endDose();
      sendWt();
      Serial.println("[FAKE MEGA] fault: pretend hopper empty");
    } else if (dispensedDg >= targetDg - 20) {
      dispensedDg = targetDg + (int32_t)random(-30, 31); // finish within +/- 3 g
      st = F_IDLE;
      idleSince = now;
      endDose();
      sendWt();
    }
  }

  if (st == F_FAULT && now - faultSince >= FAKE_FAULT_CLEAR_MS) {
    st = F_IDLE;                                         // as if the operator pressed *
    idleSince = now;
    sendWt();
  }

  if (FAKE_AUTO_DOSE_S > 0 && st == F_IDLE && !doneUnacked && now - idleSince >= FAKE_AUTO_DOSE_S * 1000UL) {
    static const char RECIPES[3] = {'A', 'B', 'C'};
    demoCount++;
    uint8_t seq = (doseSeq >= 255) ? 1 : doseSeq + 1;    // never the seq of the last DONE
    startDose(seq, RECIPES[demoCount % 3], 100 + 10 * (int)random(0, 21), demoCount % 4 == 0);
  }

  if (now - lastWtAt >= AGRILINK_WT_PERIOD_MS) sendWt();
  if (doneUnacked && now - lastDoneAt >= AGRILINK_DONE_REPEAT_MS) sendDone();
}

int fakeMegaReadByte() {
  if (outPos < outLen) return (unsigned char)outBuf[outPos++];
  outPos = outLen = 0;
  return -1;
}

#endif  // FAKE_MEGA
