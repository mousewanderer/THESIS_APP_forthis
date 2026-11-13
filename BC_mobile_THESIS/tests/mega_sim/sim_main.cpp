// Runs the REAL AgriDose_Mega_Link sketch on a laptop for one power-on: from
// switching on until the power is cut or the test's end. tests/test_power_cut.py
// compiles this and runs one process per power-on, so all the sketch's RAM is
// really gone between runs; only the "world" file carries over (EEPROM bytes,
// what is in the hoppers, what came out into the container).
//
//   sim <world file> [--seed n] [--drift g] [--dose A,600,7] [--answer resume,stop,...]
//       [--cut ms] [--cut-in-fault ms] [--no-got] [--end-idle] [--run ms] [--verbose]
//       [--feed-scale x] [--sensor-dead ms] [--fix-at-fault n] [--refill g]
//       [--spike-before-fault] [--press g] [--press-early]
//       [--drain B,7] [--send ms:BODY]...
//
// --answer: the keys pressed in turn, one at each question (flag F or R).
// --cut counts from the dose or the first answer being sent.
// --feed-scale 0 = the augers turn but move nothing (motor power off, jam);
// --sensor-dead = the load cells stop answering this long after the dose starts;
// --fix-at-fault n = the operator fixes those at fault no. n, and adds --refill g
// to the hopper. --spike-before-fault = single readings read 159 g heavy from just
// before an EMPTY fault (the HX711 bit errors seen on 30 Oct). --press g = a hand
// pushes on the hopper that ran out with g grams of force, from the moment the
// fault shows until 5 s later (the answer comes at 11 s); with --press-early it
// already pushes from 2 s after that hopper ran out, while its auger still runs
// (clearing a clog by hand: on 2 Nov a hand on the jug read about 136 g).
// [10 Nov] --drain B,7 sends DRAIN,7,B in place of a dose (the timings that count from
// the dose count from it). --send ms:BODY sends BODY (e.g. STOP,8 or DOSE,9,A,600) that
// long after it; up to 4. The report adds the ACK/NAK replies, the screens shown while
// draining, and which augers ever ran. Prints one JSON line.
#include "Arduino.h"
#include "AgriLink.h"

// ---------------------------------------------------------------- the world --
struct World {
  uint8_t ee[4096];
  double  stock[3];      // grams in each hopper
  double  out[3];        // grams that really came out, into the container
  double  zeroRaw[3];    // the load cell's raw reading with nothing in the hopper
};
static World w;
static const char* worldPath;
static const uint8_t RPWM[3] = {3, 6, 5};
static const uint8_t REN[3] = {24, 22, 29};
static const double FEED_G_PER_S[3] = {8.0, 12.0, 24.0};   // A rough, B and C from the bench notes
static const uint64_t COAST_US = 120000;                   // material keeps falling this long after the cut

static uint64_t nowUs = 0, cutAtUs = 0, eeBusyUntil = 0;
static bool cutArmed = false, cutFired = false;
static int lastEeAddr = -1;
static double driftG = 0;
static int pinVal[80], pwmVal[20];
static double feedScale = 1.0;       // 0 = the auger moves nothing, 1 = normal
static bool sensorDead = false;
static bool spikeBeforeFault = false;
static double pressG = 0;            // --press: grams of force from a hand on the jug
static bool pressEarly = false;      // --press-early
static uint64_t faultShownUs = 0;    // when flag F last appeared
static uint64_t stockOutUs[3];       // when each hopper ran out
static bool motorInFault = false;    // an auger was driven while the dose was paused
static uint64_t lastRunUs[3];
static bool ranOnce[3];

char simLcd[2][17] = {"                ", "                "};
bool simVerbose = false;
SerialMonitor Serial;
LinkPort Serial2;
EEPROMClass EEPROM;

// ---- what the test watched ----
static char   flagNow = '?';
static char   flagTrail[64] = "";
static long   flagChanges = 0;   // F -> D -> F can happen inside one loop() when a dead load cell faults at once
static bool   motorBeforeAnswer = false;
static bool   watchMotors = true;
static int32_t lastHw[3] = {0, 0, 0};
static bool   haveHw = false;
struct DoneSeen { int seq; char recipe; long target, actual; };
static DoneSeen dones[16];
static int doneN = 0;
static long lastWtDg = 0;
static int donesAtFirstAnswer = -1;  // DONE frames seen before the first answer
static char lcdAtFlagR[2][17] = {"", ""};
static bool noGot = false;
static char replies[16][24];      // [10 Nov] every ACK/NAK the Mega sent, in order
static int replyN = 0;
static char rxQueue[1024];
static size_t rxHead = 0, rxTail = 0;

static void queueFrame(const char* body) {
  char f[AGRILINK_MAX_FRAME + 1];
  size_t n = agriBuildFrame(body, f, sizeof(f));
  for (size_t i = 0; i < n; i++) { rxQueue[rxTail] = f[i]; rxTail = (rxTail + 1) % sizeof(rxQueue); }
}

static void loadWorld() {
  FILE* f = fopen(worldPath, "rb");
  if (f) { fread(&w, sizeof(w), 1, f); fclose(f); return; }
  memset(w.ee, 0xFF, sizeof(w.ee));                        // a new board
  w.stock[0] = w.stock[1] = w.stock[2] = 0;
  w.out[0] = w.out[1] = w.out[2] = 0;
  w.zeroRaw[0] = 477353; w.zeroRaw[1] = -440148; w.zeroRaw[2] = 33314;
}
static void saveWorld() {
  FILE* f = fopen(worldPath, "wb");
  fwrite(&w, sizeof(w), 1, f);
  fclose(f);
}

static void report();
static void powerCut() {
  // A byte being written as the power goes is left holding junk.
  if (nowUs < eeBusyUntil && lastEeAddr >= 0) w.ee[lastEeAddr] = (uint8_t)rand();
  cutFired = true;
  saveWorld();
  report();
  exit(0);
}

void simAdvance(uint64_t us) {
  uint64_t end = nowUs + us;
  while (nowUs < end) {
    uint64_t step = end - nowUs < 1000 ? end - nowUs : 1000;
    nowUs += step;
    bool any = false;
    for (int i = 0; i < 3; i++) {
      bool on = pinVal[REN[i]] == HIGH && pwmVal[RPWM[i]] >= 170;
      if (on) { any = true; lastRunUs[i] = nowUs; ranOnce[i] = true; }
      bool falling = on || (ranOnce[i] && nowUs - lastRunUs[i] < COAST_US);
      if (falling && w.stock[i] > 0) {
        double g = feedScale * FEED_G_PER_S[i] * step / 1e6;
        if (g > w.stock[i]) g = w.stock[i];
        w.stock[i] -= g;
        w.out[i] += g;
        if (w.stock[i] <= 0) stockOutUs[i] = nowUs;
      }
    }
    if (any && watchMotors) motorBeforeAnswer = true;
    if (any && flagNow == 'F') motorInFault = true;
    if (cutArmed && nowUs >= cutAtUs) powerCut();
  }
}
uint32_t simMillis() { return (uint32_t)(nowUs / 1000); }
void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t pin, uint8_t v) { pinVal[pin] = v; }
void analogWrite(uint8_t pin, int v) { pwmVal[pin] = v; }

bool eeprom_is_ready() { simAdvance(50); return nowUs >= eeBusyUntil; }
uint8_t EEPROMClass::read(int a) { return w.ee[a]; }
void EEPROMClass::write(int a, uint8_t v) { w.ee[a] = v; lastEeAddr = a; eeBusyUntil = nowUs + 3400; }

double HX711::readRaw(int times) {
  double sum = 0;
  for (int i = 0; i < times; i++) {
    simAdvance(100000);                                    // 10 readings a second
    double noiseG = (rand() % 61 - 30) / 100.0;            // +/- 0.3 g
    if (spikeBeforeFault && times == 1 && w.stock[hop] <= 0 && stockOutUs[hop] && nowUs - stockOutUs[hop] > 14500000) noiseG += 159.0;   // the last 0.5 s of the 15 s EMPTY window
    if (pressG > 0 && stockOutUs[hop]) {
      bool atFault = flagNow == 'F' && nowUs - faultShownUs < 5000000;
      bool whileStuck = pressEarly && flagNow == 'D' && w.stock[hop] <= 0 && nowUs - stockOutUs[hop] > 2000000;
      if (atFault || whileStuck) noiseG += pressG;           // pushing down reads heavier
    }
    sum += w.zeroRaw[hop] + scale * (w.stock[hop] + driftG + noiseG);
  }
  lastUs = nowUs;
  return sum / times;
}
bool HX711::is_ready() { return !sensorDead && nowUs - lastUs >= 100000; }
bool HX711::wait_ready_timeout(unsigned long ms) {
  if (sensorDead) { simAdvance((uint64_t)ms * 1000); return false; }
  return true;
}
float HX711::get_units(int times) { return (float)((readRaw(times) - offset) / scale); }
void HX711::tare(int times) { offset = (long)readRaw(times); }

int LinkPort::available() { return rxHead != rxTail; }
int LinkPort::read() {
  if (rxHead == rxTail) return -1;
  char c = rxQueue[rxHead];
  rxHead = (rxHead + 1) % sizeof(rxQueue);
  return (uint8_t)c;
}

// Frames from the Mega: the test plays the ESP32.
static AgriRx txRx;
size_t LinkPort::write(const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!agriRxFeed(txRx, (char)b[i])) continue;
    char* f[6];
    uint8_t c = agriSplit(txRx.buf, f, 6);
    if (strcmp(f[0], "WT") == 0 && c == 3) {
      lastWtDg = atol(f[1]);
      char fl = f[2][0];
      if (fl != flagNow) {
        flagChanges++;
        size_t l = strlen(flagTrail);
        if (l + 1 < sizeof(flagTrail)) { flagTrail[l] = fl; flagTrail[l + 1] = 0; }
        if (fl == 'R') { memcpy(lcdAtFlagR, simLcd, sizeof(simLcd)); }
        if (fl == 'F') faultShownUs = nowUs;
      }
      flagNow = fl;
    } else if ((strcmp(f[0], "ACK") == 0 || strcmp(f[0], "NAK") == 0) && replyN < 16) {
      char* r = replies[replyN++];
      snprintf(r, sizeof(replies[0]), "%s,%s%s%s", f[0], c > 1 ? f[1] : "", c > 2 ? "," : "", c > 2 ? f[2] : "");
    } else if (strcmp(f[0], "HW") == 0 && c == 4) {
      for (int h = 0; h < 3; h++) lastHw[h] = atol(f[h + 1]);
      haveHw = true;
    } else if (strcmp(f[0], "DONE") == 0 && c == 5) {
      if (doneN < 16) dones[doneN++] = {atoi(f[1]), f[2][0], atol(f[3]), atol(f[4])};
      if (!noGot) { char g[16]; snprintf(g, sizeof(g), "GOT,%s", f[1]); queueFrame(g); }
    }
  }
  return n;
}

// The real sketch, with the prototypes arduino-cli adds to it.
#include "sim_sketch.cpp"

// Every different screen shown while the dose was paused.
static char screens[24][36];
static int screenN = 0;
static void noteScreen() {
  char s[36];
  snprintf(s, sizeof(s), "%s|%s", simLcd[0], simLcd[1]);
  for (int i = 0; i < screenN; i++) if (!strcmp(screens[i], s)) return;
  if (screenN < 24) strcpy(screens[screenN++], s);
}

// [10 Nov] The screens while draining and the one after it: one per top line,
// holding the latest numbers (the grams change on every reading).
static char drainScreens[24][36];
static int drainScreenN = 0;
static void noteDrainScreen() {
  char s[36];
  snprintf(s, sizeof(s), "%s|%s", simLcd[0], simLcd[1]);
  for (int i = 0; i < drainScreenN; i++)
    if (!strncmp(drainScreens[i], s, 17)) { strcpy(drainScreens[i], s); return; }   // same top line and '|'
  if (drainScreenN < 24) strcpy(drainScreens[drainScreenN++], s);
}

static void report() {
  printf("{\"motorInFault\":%s,\"donesAtFirstAnswer\":%d,\"screens\":[", motorInFault ? "true" : "false", donesAtFirstAnswer);
  for (int i = 0; i < screenN; i++) printf("%s\"%s\"", i ? "," : "", screens[i]);
  printf("],\"drainScreens\":[");
  for (int i = 0; i < drainScreenN; i++) printf("%s\"%s\"", i ? "," : "", drainScreens[i]);
  printf("],\"replies\":[");
  for (int i = 0; i < replyN; i++) printf("%s\"%s\"", i ? "," : "", replies[i]);
  printf("],\"ran\":[%d,%d,%d],", ranOnce[0], ranOnce[1], ranOnce[2]);
  printf("\"cut\":%s,\"flags\":\"%s\",\"flag\":\"%c\",\"motorBeforeAnswer\":%s,\"wt\":%ld,"
         "\"stock\":[%.2f,%.2f,%.2f],\"out\":[%.2f,%.2f,%.2f],\"hw\":[%ld,%ld,%ld],\"haveHw\":%s,"
         "\"lcd\":[\"%s\",\"%s\"],\"lcdR\":[\"%s\",\"%s\"],"
         "\"stopEarly\":[%.3f,%.3f,%.3f],\"learned\":[%d,%d,%d],\"zeroed\":[%d,%d,%d],\"haveSaved\":%s,\"dones\":[",
         cutFired ? "true" : "false", flagTrail, flagNow, motorBeforeAnswer ? "true" : "false", lastWtDg,
         w.stock[0], w.stock[1], w.stock[2], w.out[0], w.out[1], w.out[2],
         (long)lastHw[0], (long)lastHw[1], (long)lastHw[2], haveHw ? "true" : "false",
         simLcd[0], simLcd[1], lcdAtFlagR[0], lcdAtFlagR[1],
         hoppers[0].stopEarlyG, hoppers[1].stopEarlyG, hoppers[2].stopEarlyG,
         hoppers[0].learned, hoppers[1].learned, hoppers[2].learned,
         hoppers[0].zeroed, hoppers[1].zeroed, hoppers[2].zeroed, haveSaved ? "true" : "false");
  // Every DONE frame repeats until GOT, so only different ones are listed.
  bool first = true;
  for (int i = 0; i < doneN; i++) {
    if (i && dones[i].seq == dones[i - 1].seq && dones[i].actual == dones[i - 1].actual) continue;
    printf("%s[%d,\"%c\",%ld,%ld]", first ? "" : ",", dones[i].seq, dones[i].recipe, dones[i].target, dones[i].actual);
    first = false;
  }
  printf("]}\n");
  fflush(stdout);
}

int main(int argc, char** argv) {
  worldPath = argv[1];
  char doseBody[32] = "";
  bool drainRun = false;               // [10 Nov] doseBody is a DRAIN
  char sends[4][32];                   // [10 Nov] --send: what...
  long sendMs[4];                      // ...and when, after the dose or drain was sent
  int sendN = 0, sendNext = 0;
  char answers[8][16];
  int answerN = 0, answerNext = 0;
  long cutMs = -1, runMs = -1, cutInFaultMs = -1, sensorDeadMs = -1, fixAtFault = 0;
  double refillG = 0;
  bool endIdle = false;
  int seed = 1;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--seed")) seed = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--drift")) driftG = atof(argv[++i]);
    else if (!strcmp(argv[i], "--answer")) {
      char list[64];
      snprintf(list, sizeof(list), "%s", argv[++i]);
      for (char* t = strtok(list, ","); t && answerN < 8; t = strtok(nullptr, ",")) snprintf(answers[answerN++], 16, "%s", t);
    }
    else if (!strcmp(argv[i], "--cut-in-fault")) cutInFaultMs = atol(argv[++i]);
    else if (!strcmp(argv[i], "--feed-scale")) feedScale = atof(argv[++i]);
    else if (!strcmp(argv[i], "--sensor-dead")) sensorDeadMs = atol(argv[++i]);
    else if (!strcmp(argv[i], "--fix-at-fault")) fixAtFault = atol(argv[++i]);
    else if (!strcmp(argv[i], "--refill")) refillG = atof(argv[++i]);
    else if (!strcmp(argv[i], "--spike-before-fault")) spikeBeforeFault = true;
    else if (!strcmp(argv[i], "--press")) pressG = atof(argv[++i]);
    else if (!strcmp(argv[i], "--press-early")) pressEarly = true;
    else if (!strcmp(argv[i], "--cut")) cutMs = atol(argv[++i]);
    else if (!strcmp(argv[i], "--run")) runMs = atol(argv[++i]);
    else if (!strcmp(argv[i], "--no-got")) noGot = true;
    else if (!strcmp(argv[i], "--end-idle")) endIdle = true;
    else if (!strcmp(argv[i], "--verbose")) simVerbose = true;
    else if (!strcmp(argv[i], "--drain")) {
      char h; int s;
      sscanf(argv[++i], "%c,%d", &h, &s);
      snprintf(doseBody, sizeof(doseBody), "DRAIN,%d,%c", s, h);
      drainRun = true;
    }
    else if (!strcmp(argv[i], "--send") && sendN < 4) {
      const char* a = argv[++i];
      const char* colon = strchr(a, ':');
      sendMs[sendN] = atol(a);
      snprintf(sends[sendN], sizeof(sends[0]), "%s", colon ? colon + 1 : "");
      sendN++;
    }
    else if (!strcmp(argv[i], "--dose")) {
      char r; int g, s;
      sscanf(argv[++i], "%c,%d,%d", &r, &g, &s);
      snprintf(doseBody, sizeof(doseBody), "DOSE,%d,%c,%d", s, r, g);
    }
  }
  srand(seed);
  loadWorld();
  agriRxReset(txRx);

  // A dose started on purpose may run its augers; after a power-on nothing may
  // move until the answer has been sent.
  if (doseBody[0]) watchMotors = false;

  setup();

  uint64_t bootUs = nowUs;
  bool doseSent = false, waitingAnswer = false, anyAnswer = false;
  uint64_t idleFrom = 0, doseSentUs = 0, flagAskedUs = 0;
  long flagChangesSeen = 0;
  long faultCount = 0;
  for (;;) {
    if (doseBody[0] && !doseSent && nowUs - bootUs >= 500000) {
      queueFrame(doseBody);
      doseSent = true;
      doseSentUs = nowUs;
      if (cutMs >= 0) { cutAtUs = nowUs + (uint64_t)cutMs * 1000; cutArmed = true; }
    }
    if (sensorDeadMs >= 0 && doseSent && !sensorDead && nowUs - doseSentUs >= (uint64_t)sensorDeadMs * 1000) {
      sensorDead = true;
      sensorDeadMs = -1;                                   // once only
    }

    // A new question from the Mega: the operator looks at it, fixes things, then answers.
    bool asking = flagNow == 'F' || flagNow == 'R';
    if (flagChanges != flagChangesSeen) {
      if (asking) {
        flagAskedUs = nowUs;
        waitingAnswer = true;
        if (flagNow == 'F') {
          faultCount++;
          if (cutInFaultMs >= 0) { cutAtUs = nowUs + (uint64_t)cutInFaultMs * 1000; cutArmed = true; }
          if (faultCount == fixAtFault) {
            feedScale = 1.0;
            sensorDead = false;
            if (curHopper >= 0) w.stock[curHopper] += refillG;
          }
        }
      }
      flagChangesSeen = flagChanges;
    }
    if (flagNow == 'F' && mstate == M_FAULT) noteScreen();
    // A fault is watched for 11 s, so all its screens are seen. A power-on is answered at once.
    uint64_t lookUs = flagNow == 'F' ? 11000000ULL : 200000ULL;
    if (asking && waitingAnswer && answerNext < answerN && nowUs - flagAskedUs >= lookUs && nowUs - bootUs >= 700000) {
      watchMotors = false;                                 // from here on the auger may turn
      if (!anyAnswer) { donesAtFirstAnswer = doneN; if (cutMs >= 0 && !doseBody[0]) { cutAtUs = nowUs + (uint64_t)cutMs * 1000; cutArmed = true; } }
      anyAnswer = true;
      char body[16];
      snprintf(body, sizeof(body), "%s,%d", !strcmp(answers[answerNext], "resume") ? "RESUME" : "STOP", 200 + answerNext);
      queueFrame(body);
      answerNext++;
      waitingAnswer = false;
    }
    if (doseSent && sendNext < sendN && nowUs - doseSentUs >= (uint64_t)sendMs[sendNext] * 1000) {
      queueFrame(sends[sendNext]);
      sendNext++;
    }
    loop();
    if (drainRun && (mstate == M_DRAIN || mstate == M_RESULT)) noteDrainScreen();
    simAdvance(1000);

    if (runMs >= 0 && nowUs - bootUs >= (uint64_t)runMs * 1000) break;
    if (endIdle) {
      bool settled = flagNow == 'I' && !doneUnacked && mstate == M_IDLE && !stateWriter.busy && nowUs - bootUs > 2000000 &&
                     sendNext >= sendN;   // [10 Nov] and every --send has gone out
      if (settled) { if (!idleFrom) idleFrom = nowUs; if (nowUs - idleFrom > 1000000) break; }
      else idleFrom = 0;
    }
    if (nowUs - bootUs > 1800000000ULL) break;             // 30 simulated minutes: give up
  }
  saveWorld();
  report();
  return 0;
}
