// Runs the REAL AgriDose_Mega_Link sketch on a laptop for one power-on: from
// switching on until the power is cut or the test's end. tests/test_power_cut.py
// compiles this and runs one process per power-on, so all the sketch's RAM is
// really gone between runs; only the "world" file carries over (EEPROM bytes,
// what is in the hoppers, what came out into the container).
//
//   sim <world file> [--seed n] [--drift g] [--dose A,600,7] [--answer resume|stop]
//       [--cut ms] [--no-got] [--end-idle] [--run ms] [--verbose]
//
// --cut counts from the dose or the answer being sent. Prints one JSON line.
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
static bool   motorBeforeAnswer = false;
static bool   watchMotors = true;
static int32_t lastHw[3] = {0, 0, 0};
static bool   haveHw = false;
struct DoneSeen { int seq; char recipe; long target, actual; };
static DoneSeen dones[16];
static int doneN = 0;
static long lastWtDg = 0;
static char lcdAtFlagR[2][17] = {"", ""};
static bool noGot = false;
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
        double g = FEED_G_PER_S[i] * step / 1e6;
        if (g > w.stock[i]) g = w.stock[i];
        w.stock[i] -= g;
        w.out[i] += g;
      }
    }
    if (any && watchMotors) motorBeforeAnswer = true;
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
    sum += w.zeroRaw[hop] + scale * (w.stock[hop] + driftG + noiseG);
  }
  lastUs = nowUs;
  return sum / times;
}
bool HX711::is_ready() { return nowUs - lastUs >= 100000; }
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
        size_t l = strlen(flagTrail);
        if (l + 1 < sizeof(flagTrail)) { flagTrail[l] = fl; flagTrail[l + 1] = 0; }
        if (fl == 'R') { memcpy(lcdAtFlagR, simLcd, sizeof(simLcd)); }
      }
      flagNow = fl;
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

static void report() {
  printf("{\"cut\":%s,\"flags\":\"%s\",\"flag\":\"%c\",\"motorBeforeAnswer\":%s,\"wt\":%ld,"
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
  char doseBody[32] = "", answer[16] = "";
  long cutMs = -1, runMs = -1;
  bool endIdle = false;
  int seed = 1;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--seed")) seed = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--drift")) driftG = atof(argv[++i]);
    else if (!strcmp(argv[i], "--answer")) snprintf(answer, sizeof(answer), "%s", argv[++i]);
    else if (!strcmp(argv[i], "--cut")) cutMs = atol(argv[++i]);
    else if (!strcmp(argv[i], "--run")) runMs = atol(argv[++i]);
    else if (!strcmp(argv[i], "--no-got")) noGot = true;
    else if (!strcmp(argv[i], "--end-idle")) endIdle = true;
    else if (!strcmp(argv[i], "--verbose")) simVerbose = true;
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
  bool doseSent = false, answered = false;
  uint64_t idleFrom = 0;
  for (;;) {
    if (doseBody[0] && !doseSent && nowUs - bootUs >= 500000) {
      queueFrame(doseBody);
      doseSent = true;
      if (cutMs >= 0) { cutAtUs = nowUs + (uint64_t)cutMs * 1000; cutArmed = true; }
    }
    if (answer[0] && !answered && flagNow == 'R' && nowUs - bootUs >= 700000) {
      watchMotors = false;                                 // from here on the auger may turn
      queueFrame(!strcmp(answer, "resume") ? "RESUME,200" : "STOP,200");
      answered = true;
      if (cutMs >= 0) { cutAtUs = nowUs + (uint64_t)cutMs * 1000; cutArmed = true; }
    }
    loop();
    simAdvance(1000);

    if (runMs >= 0 && nowUs - bootUs >= (uint64_t)runMs * 1000) break;
    if (endIdle) {
      bool settled = flagNow == 'I' && !doneUnacked && mstate == M_IDLE && !stateWriter.busy && nowUs - bootUs > 2000000;
      if (settled) { if (!idleFrom) idleFrom = nowUs; if (nowUs - idleFrom > 1000000) break; }
      else idleFrom = 0;
    }
    if (nowUs - bootUs > 1800000000ULL) break;             // 30 simulated minutes: give up
  }
  saveWorld();
  report();
  return 0;
}
