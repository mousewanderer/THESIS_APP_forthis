// Laptop unit test for AgriLink.h (no board needed).
//
// Build and run from the project folder (MSYS2 g++):
//   g++ -std=c++11 -Wall -I AgriDose_ESP32_Link tests/test_agrilink.cpp -o test_agrilink
//   ./test_agrilink
//
// The expected frames below were made with Python's binascii.crc_hqx
// (a separate CRC-16/CCITT implementation), so they check this code
// against something it did not write itself.
#include <stdio.h>
#include "AgriLink.h"

static int failures = 0;

static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

// Feeds a whole string to the receiver. Returns how many frames came out,
// and copies the last BODY into lastBody.
static int feedAll(AgriRx& rx, const char* bytes, char* lastBody) {
  int frames = 0;
  for (const char* p = bytes; *p; p++) {
    if (agriRxFeed(rx, *p)) { frames++; strcpy(lastBody, rx.buf); }
  }
  return frames;
}

int main() {
  // 1. CRC check value from the CRC-16/CCITT-FALSE definition
  check(agriCrc16("123456789", 9) == 0x29B1, "CRC of \"123456789\" is 0x29B1");

  // 2. Frames match the Python reference
  const char* bodies[] = {"WT,2486,D", "DOSE,12,A,250", "ACK,12", "NAK,7,BUSY",
                          "DONE,12,A,2500,2486", "GOT,12", "HELLO",
                          "DISP,SELECT RECIPE,A / B / C *=BACK", "WT,-3,I"};
  const char* frames[] = {"$WT,2486,D*E9AA\r\n", "$DOSE,12,A,250*D07F\r\n", "$ACK,12*7D8B\r\n",
                          "$NAK,7,BUSY*C067\r\n", "$DONE,12,A,2500,2486*B8EE\r\n", "$GOT,12*B408\r\n",
                          "$HELLO*49D6\r\n", "$DISP,SELECT RECIPE,A / B / C *=BACK*1541\r\n",
                          "$WT,-3,I*FFEC\r\n"};
  bool allMatch = true;
  for (int i = 0; i < 9; i++) {
    char out[AGRILINK_MAX_FRAME + 1];
    agriBuildFrame(bodies[i], out, sizeof(out));
    if (strcmp(out, frames[i]) != 0) { allMatch = false; printf("      got %s", out); }
  }
  check(allMatch, "9 frames match the Python reference");

  // 3. Every reference frame is received back as its BODY
  bool allBack = true;
  for (int i = 0; i < 9; i++) {
    AgriRx rx; agriRxReset(rx);
    char body[64] = "";
    if (feedAll(rx, frames[i], body) != 1 || strcmp(body, bodies[i]) != 0) allBack = false;
  }
  check(allBack, "receiver returns the BODY of every reference frame");

  // 4. Noise before a frame and between frames is ignored
  {
    AgriRx rx; agriRxReset(rx);
    char body[64] = "";
    int n = feedAll(rx, "xx\x01garbage$WT,2486,D*E9AA\r\nzz$ACK,12*7D8B\r\n", body);
    check(n == 2 && strcmp(body, "ACK,12") == 0, "noise outside frames is ignored");
  }

  // 5. One changed byte is caught by the CRC
  {
    AgriRx rx; agriRxReset(rx);
    char body[64] = "";
    int n = feedAll(rx, "$WT,2487,D*E9AA\r\n", body);
    check(n == 0 && rx.bad == 1, "a changed digit fails the CRC");
  }

  // 6. A frame cut off by a new '$' is dropped, the next one still works
  {
    AgriRx rx; agriRxReset(rx);
    char body[64] = "";
    int n = feedAll(rx, "$WT,24$ACK,12*7D8B\r\n", body);
    check(n == 1 && strcmp(body, "ACK,12") == 0, "a frame cut short by '$' is dropped");
  }

  // 7. Too long, missing CRC, bad hex
  {
    AgriRx rx; agriRxReset(rx);
    char body[64] = "";
    char longFrame[128] = "$";
    for (int i = 0; i < 70; i++) strcat(longFrame, "A");
    strcat(longFrame, "*0000\r\n");
    int n = feedAll(rx, longFrame, body) + feedAll(rx, "$ACK,12\r\n", body) + feedAll(rx, "$ACK,12*7D8G\r\n", body);
    check(n == 0, "too long / no CRC / bad hex are all rejected");
  }

  // 8. The sender refuses bodies that would break a frame
  {
    char out[AGRILINK_MAX_FRAME + 1];
    char tooLong[60]; memset(tooLong, 'A', 49); tooLong[49] = 0;
    bool ok = agriBuildFrame("", out, sizeof(out)) == 0 && agriBuildFrame("A$B", out, sizeof(out)) == 0 &&
              agriBuildFrame("A\nB", out, sizeof(out)) == 0 && agriBuildFrame(tooLong, out, sizeof(out)) == 0;
    check(ok, "sender refuses empty, '$', newline and too-long bodies");
  }

  // 9. The longest DISP (two full 16-char lines) fits
  {
    char out[AGRILINK_MAX_FRAME + 1];
    size_t n = agriBuildFrame("DISP,1234567890123456,1234567890123456", out, sizeof(out));
    check(n > 0, "longest DISP fits in a frame");
  }

  // 10. Field splitting and number reading
  {
    char b[] = "DONE,12,A,2500,2486";
    char* f[6];
    uint8_t n = agriSplit(b, f, 6);
    int32_t t = 0, a = 0;
    bool ok = n == 5 && strcmp(f[0], "DONE") == 0 && f[2][0] == 'A' &&
              agriParseInt(f[3], t) && t == 2500 && agriParseInt(f[4], a) && a == 2486;
    int32_t x;
    uint8_t s;
    ok = ok && !agriParseInt("12a", x) && !agriParseInt("", x) && agriParseInt("-3", x) && x == -3;
    ok = ok && !agriParseSeq("0", s) && !agriParseSeq("256", s) && agriParseSeq("255", s) && s == 255;
    check(ok, "split, parseInt and parseSeq");
  }

  // 11. Recipe split adds up exactly for every recipe and every whole gram AGRILINK_DOSE_MIN_G..MAX_G (300..900 since 1 Nov 2026)
  {
    bool ok = true;
    for (int r = 0; r < 3; r++) {
      for (int g = AGRILINK_DOSE_MIN_G; g <= AGRILINK_DOSE_MAX_G; g++) {
        int32_t h, c, w;
        agriSplitDose(AGRI_RECIPES[r], g * 10, h, c, w);
        if (h + c + w != g * 10 || h <= 0 || c <= 0 || w <= 0) ok = false;
      }
    }
    int32_t h, c, w;
    agriSplitDose(*agriRecipe('A'), 2500, h, c, w);
    ok = ok && h == 625 && c == 1750 && w == 125;
    ok = ok && agriRecipe('D') == nullptr && agriRecipe('a') == nullptr;
    check(ok, "recipe split sums exactly; A at 250 g = 62.5 / 175.0 / 12.5 g");
  }

  // 12. LCD lines are cut to 16 characters and lose ',' and '$'
  {
    char l[17];
    agriCopyLcdLine(l, "A,B$C 1234567890123456789");
    check(strcmp(l, "A B C 1234567890") == 0, "LCD line cleaned and cut to 16");
  }

  // 13. HW deadband: within +-5 g of what was sent is noise, more is sent
  {
    const int32_t sent[3] = {42000, 36000, 9000};
    const int32_t noise[3] = {42050, 35950, 9000};       // exactly 5 g off: not sent
    const int32_t moved[3] = {42000, 36000, 9051};       // ash 5.1 g heavier: sent
    const int32_t down[3]  = {41949, 36000, 9000};       // hull 5.1 g lighter: sent
    check(!agriHwChanged(sent, sent, AGRILINK_HW_DEADBAND_DG), "HW: no change is not sent");
    check(!agriHwChanged(sent, noise, AGRILINK_HW_DEADBAND_DG), "HW: +5 g / -5 g noise is not sent");
    check(agriHwChanged(sent, moved, AGRILINK_HW_DEADBAND_DG), "HW: +5.1 g on one hopper is sent");
    check(agriHwChanged(sent, down, AGRILINK_HW_DEADBAND_DG), "HW: -5.1 g on one hopper is sent");
    // a slow drift of 1 g per reading is sent once it adds up past 5 g from the last HW
    int32_t now[3] = {42000, 36000, 9000};
    int steps = 0;
    while (!agriHwChanged(sent, now, AGRILINK_HW_DEADBAND_DG) && steps < 100) { now[1] -= 10; steps++; }
    check(steps == 6, "HW: a 1 g-per-reading drift is sent on the 6th reading (5 g is still noise)");
    char body[AGRILINK_MAX_BODY + 1];
    snprintf(body, sizeof(body), "HW,%ld,%ld,%ld", -999999L, -999999L, -999999L);
    check(strlen(body) <= AGRILINK_MAX_BODY, "HW: the longest HW body fits a frame");
  }

  // ---- the flags [R added 3 Nov 2026] ----
  check(agriIsFlag('I') && agriIsFlag('D') && agriIsFlag('F') && agriIsFlag('R'), "flags I, D, F and R are accepted in WT");
  check(!agriIsFlag('X') && !agriIsFlag('?') && !agriIsFlag(0), "any other flag is refused");
  {
    char f[AGRILINK_MAX_FRAME + 1];
    check(agriBuildFrame("RESUME,255", f, sizeof(f)) > 0, "RESUME: the longest RESUME frame builds");
  }

  // ---- draining a hopper [10 Nov 2026] ----
  check(agriIsFlag('E') && AGRILINK_FLAG_DRAIN == 'E', "flag E (draining) is accepted in WT");
  check(agriHopperIndex("A") == 0 && agriHopperIndex("B") == 1 && agriHopperIndex("C") == 2, "DRAIN: A, B, C are hoppers 0, 1, 2");
  check(agriHopperIndex("D") < 0 && agriHopperIndex("a") < 0 && agriHopperIndex("AB") < 0 && agriHopperIndex("") < 0 &&
        agriHopperIndex(nullptr) < 0, "DRAIN: any other hopper letter is refused");
  {
    char f[AGRILINK_MAX_FRAME + 1], body[AGRILINK_MAX_BODY + 1];
    check(agriBuildFrame("DRAIN,255,C", f, sizeof(f)) > 0, "DRAIN: the longest DRAIN frame builds");
    strcpy(body, "DRAIN,7,B");
    char* fl[6];
    uint8_t n = agriSplit(body, fl, 6);
    check(n == 3 && strcmp(fl[0], "DRAIN") == 0 && agriHopperIndex(fl[2]) == 1, "DRAIN,7,B splits into the verb, the seq and hopper B");
  }

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
