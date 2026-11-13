#include "mega_link.h"
#include "config.h"
#include "AgriLink.h"
#if FAKE_MEGA
#include "fake_mega.h"
#endif

#define LCD_REFRESH_MS 2000UL      // resend the menu text this often (a Mega that boots late still gets it)

struct CmdRequest { LinkCmd cmd; char recipe; int16_t grams; };
struct LcdText    { bool on; char l0[17]; char l1[17]; };

// Queues between the loop task (keypad) and the link task.
static QueueHandle_t cmdQueue;     // keypad -> link: commands to send
static QueueHandle_t resultQueue;  // link -> keypad: how each command ended
static QueueHandle_t doneQueue;    // link -> loop: new doses for the SD log
static QueueHandle_t lcdQueue;     // keypad -> link: menu text (1 slot, newest wins)

// ---- everything below is used by the link task only ----
static AgriRx   rx;
static uint8_t  nextSeq = 1;
static uint8_t  lastDoneSeq = 0;   // DONE seq already handled (0 = none)
static bool     helloBeforeFirstCmd = true;
static uint32_t lastRxAt = 0;
static bool     everRx = false, wasOnline = false;

static bool       inFlight = false;   // one command in flight at a time
static CmdRequest flightReq;
static uint8_t    flightSeq = 0;
static char       flightBody[24];
static uint8_t    flightTries = 0;
static uint32_t   flightSentAt = 0;

static LcdText  lcd = {false, "", ""};
static uint32_t lcdSentAt = 0;

static const char* const CMD_NAMES[] = {"PING", "TARE", "STOP", "DOSE", "RESUME", "DRAIN"};  // same order as LinkCmd

// The one place a frame leaves the ESP32 for the Mega.
static void sendBody(const char* body) {
#if FAKE_MEGA
  fakeMegaReceive(body);
#else
  agriSend(MEGA_SERIAL, body);
#endif
#if DEBUG_LINK
  if (strncmp(body, "DISP,", 5) != 0) Serial.printf("[TX] %s\n", body);
#endif
}

static uint8_t takeSeq() {
  uint8_t s = nextSeq;
  nextSeq = (nextSeq == 255) ? 1 : nextSeq + 1;
  return s;
}

// ---------------------------------------------------------- commands --
static void finishCommand(LinkOutcome outcome, const char* reason) {
  LinkResult r;
  r.cmd = flightReq.cmd;
  r.seq = flightSeq;
  r.outcome = outcome;
  strncpy(r.nakReason, reason, sizeof(r.nakReason) - 1);
  r.nakReason[sizeof(r.nakReason) - 1] = 0;
  xQueueSend(resultQueue, &r, 0);
  inFlight = false;

  // The Mega refuses a new DOSE while an old DONE is still unanswered, so
  // once a DOSE is accepted no old DONE can come back. Forgetting the last
  // DONE seq here means the new dose's DONE is never mistaken for an old
  // one, even after the seq numbers wrap round past 255. The same goes for a
  // resumed dose: it runs on to a DONE of its own, with its old seq.
  if (outcome == LINK_ACKED && (r.cmd == LINK_DOSE || r.cmd == LINK_RESUME)) lastDoneSeq = 0;

  static const char* const OUTCOME_NAMES[] = {"ACK", "NAK", "no reply", "Mega restarted"};
  Serial.printf("Mega link: %s seq %u -> %s %s\n", CMD_NAMES[r.cmd], r.seq, OUTCOME_NAMES[outcome], reason);
}

static void startNextCommand() {
  if (inFlight) return;
  CmdRequest req;
  if (xQueueReceive(cmdQueue, &req, 0) != pdTRUE) return;

  // HELLO tells the Mega to forget its seq memory. It also went out at boot,
  // but the Mega throws away bytes that arrive while it zeroes its scales,
  // so it goes out once more right before our first command.
  if (helloBeforeFirstCmd) { sendBody("HELLO"); helloBeforeFirstCmd = false; }

  flightReq = req;
  flightSeq = takeSeq();
  if (req.cmd == LINK_DOSE) snprintf(flightBody, sizeof(flightBody), "DOSE,%u,%c,%d", flightSeq, req.recipe, req.grams);
  else if (req.cmd == LINK_DRAIN) snprintf(flightBody, sizeof(flightBody), "DRAIN,%u,%c", flightSeq, req.recipe);
  else snprintf(flightBody, sizeof(flightBody), "%s,%u", CMD_NAMES[req.cmd], flightSeq);
  inFlight = true;
  flightTries = 1;
  flightSentAt = millis();
  sendBody(flightBody);
}

// No ACK/NAK in time: send the very same frame (same seq) again.
static void retryIfDue() {
  if (!inFlight || millis() - flightSentAt < AGRILINK_ACK_TIMEOUT_MS) return;
  if (flightTries >= AGRILINK_MAX_TRIES) { finishCommand(LINK_NO_REPLY, ""); return; }
  flightTries++;
  flightSentAt = millis();
  sendBody(flightBody);
}

// ------------------------------------------------------- LCD menu text --
static void sendDisp() {
  char b[AGRILINK_MAX_BODY + 1];
  snprintf(b, sizeof(b), "DISP,%s,%s", lcd.l0, lcd.l1);
  sendBody(b);
  lcdSentAt = millis();
}

static void lcdService() {
  LcdText t;
  if (xQueueReceive(lcdQueue, &t, 0) == pdTRUE) {
    lcd = t;
    if (lcd.on) sendDisp();          // new text: send it now
    return;
  }
  if (lcd.on && millis() - lcdSentAt >= LCD_REFRESH_MS) sendDisp();
}

// ------------------------------------------------------ frames received --
static void handleDone(char* f[], uint8_t n) {
  uint8_t seq;
  int32_t target, actual;
  if (n != 5 || !agriParseSeq(f[1], seq) || strlen(f[2]) != 1 || agriRecipe(f[2][0]) == nullptr ||
      !agriParseInt(f[3], target) || !agriParseInt(f[4], actual)) {
    Serial.println("Mega link: bad DONE frame, ignored");
    return;
  }
  char got[12];
  snprintf(got, sizeof(got), "GOT,%u", seq);
  sendBody(got);                     // every time: our last GOT may have been lost
  if (seq == lastDoneSeq) return;    // a repeat of a dose we already handled
  lastDoneSeq = seq;

  DoseResult d;
  d.valid = true;
  d.seq = seq;
  d.recipe = f[2][0];
  d.targetDg = target;
  d.actualDg = actual;
  liveSetDone(d);
  if (xQueueSend(doneQueue, &d, 0) != pdTRUE) Serial.println("Mega link: dose log queue full, row lost");
  Serial.printf("Mega link: DONE seq %u recipe %c target %ld dg actual %ld dg\n", seq, d.recipe, (long)target, (long)actual);
}

static void handleFrame(char* body) {
  lastRxAt = millis();
  everRx = true;
  liveTouch();

  char* f[6];
  uint8_t n = agriSplit(body, f, 6);
  const char* verb = f[0];
  int32_t dg;
  uint8_t seq;

  if (strcmp(verb, "WT") == 0) {
    if (n == 3 && agriParseInt(f[1], dg) && strlen(f[2]) == 1 && agriIsFlag(f[2][0])) liveSetWeight(dg, f[2][0]);
  } else if (strcmp(verb, "HW") == 0) {
    int32_t hw[3];
    if (n == 4 && agriParseInt(f[1], hw[0]) && agriParseInt(f[2], hw[1]) && agriParseInt(f[3], hw[2])) liveSetHoppers(hw);
  } else if (strcmp(verb, "ACK") == 0) {
    if (n == 2 && agriParseSeq(f[1], seq) && inFlight && seq == flightSeq) finishCommand(LINK_ACKED, "");
  } else if (strcmp(verb, "NAK") == 0) {
    if (n == 3 && agriParseSeq(f[1], seq) && inFlight && seq == flightSeq) finishCommand(LINK_REFUSED, f[2]);
    else Serial.printf("Mega link: NAK not for the command in flight (%s)\n", n > 2 ? f[2] : "?");
  } else if (strcmp(verb, "DONE") == 0) {
    handleDone(f, n);
  } else if (strcmp(verb, "HELLO") == 0) {
    Serial.println("Mega link: the Mega restarted (HELLO)");
    if (inFlight) finishCommand(LINK_MEGA_RESTARTED, "");   // never repeat a command on a freshly booted Mega
  } else {
    Serial.printf("Mega link: unknown frame '%s'\n", verb);
  }
}

static void watchOnline() {
  bool online = everRx && millis() - lastRxAt < LINK_OFFLINE_MS;
  if (online == wasOnline) return;
  wasOnline = online;
  Serial.println(online ? "Mega link: online" : "Mega link: OFFLINE (no valid frame for 2 s)");
}

// ------------------------------------------------------------ the task --
static void linkTask(void*) {
  for (;;) {
#if FAKE_MEGA
    fakeMegaTick();
    int c;
    while ((c = fakeMegaReadByte()) >= 0)
      if (agriRxFeed(rx, (char)c)) handleFrame(rx.buf);
#else
    while (MEGA_SERIAL.available())
      if (agriRxFeed(rx, (char)MEGA_SERIAL.read())) handleFrame(rx.buf);
#endif
    startNextCommand();
    retryIfDue();
    lcdService();
    watchOnline();
    vTaskDelay(pdMS_TO_TICKS(5));    // sleep 5 ms; the 1 KB UART buffer holds the bytes meanwhile
  }
}

void linkBegin(uint8_t lastLoggedSeq) {
  cmdQueue    = xQueueCreate(3, sizeof(CmdRequest));
  resultQueue = xQueueCreate(4, sizeof(LinkResult));
  doneQueue   = xQueueCreate(4, sizeof(DoseResult));
  lcdQueue    = xQueueCreate(1, sizeof(LcdText));
  agriRxReset(rx);

  lastDoneSeq = lastLoggedSeq;       // a DONE repeated from before a restart is not logged twice
  nextSeq = (lastLoggedSeq >= 255) ? 1 : lastLoggedSeq + 1;

#if FAKE_MEGA
  fakeMegaBegin();
  Serial.println("FAKE_MEGA is ON: no UART; a pretend Mega runs inside the ESP32.");
#else
  MEGA_SERIAL.setRxBufferSize(MEGA_RX_BUF);   // must come before begin()
  MEGA_SERIAL.begin(AGRILINK_BAUD, SERIAL_8N1, MEGA_RX_PIN, MEGA_TX_PIN);
#endif
  sendBody("HELLO");
  xTaskCreatePinnedToCore(linkTask, "megaLink", LINK_TASK_STACK, nullptr, LINK_TASK_PRIORITY, nullptr, LINK_TASK_CORE);
}

// ---------------------------------------------- called from the loop task --
bool linkRequest(LinkCmd cmd, char recipe, int grams) {
  CmdRequest r = {cmd, recipe, (int16_t)grams};
  if (cmd == LINK_STOP) return xQueueSendToFront(cmdQueue, &r, 0) == pdTRUE;   // STOP jumps the line
  return xQueueSend(cmdQueue, &r, 0) == pdTRUE;
}

bool linkPollResult(LinkResult& out) { return xQueueReceive(resultQueue, &out, 0) == pdTRUE; }
bool linkPollDone(DoseResult& out)   { return xQueueReceive(doneQueue, &out, 0) == pdTRUE; }

// Only the loop task calls these two, so `posted` needs no lock.
static LcdText posted = {false, "", ""};

void linkShowLcd(const char* line0, const char* line1) {
  LcdText t;
  t.on = true;
  agriCopyLcdLine(t.l0, line0);
  agriCopyLcdLine(t.l1, line1);
  if (posted.on && strcmp(posted.l0, t.l0) == 0 && strcmp(posted.l1, t.l1) == 0) return;   // no change
  posted = t;
  xQueueOverwrite(lcdQueue, &t);
}

void linkReleaseLcd() {
  if (!posted.on) return;
  posted.on = false;
  xQueueOverwrite(lcdQueue, &posted);
}
