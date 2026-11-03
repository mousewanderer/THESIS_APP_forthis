#include "dose_log.h"
#include "config.h"
#include "sd_store.h"
#include "wall_clock.h"
#include <Preferences.h>
#include <algorithm>

static uint32_t bootNo = 0;
static uint32_t lastNo = 0;          // newest dose number in the file
static uint8_t  lastSeqAtBoot = 0;

void formatDg(char* out, size_t size, int32_t dg) {
  int32_t a = dg < 0 ? -dg : dg;
  snprintf(out, size, "%s%ld.%ld", dg < 0 ? "-" : "", (long)(a / 10), (long)(a % 10));
}

// "248.6" -> 2486, "-1.4" -> -14, "250" -> 2500. False for anything else.
static bool gramsToDg(const char* s, int32_t& dg) {
  bool negative = (*s == '-');
  if (negative) s++;
  if (!isdigit((unsigned char)*s)) return false;
  int32_t whole = 0;
  while (isdigit((unsigned char)*s)) whole = whole * 10 + (*s++ - '0');
  int32_t tenth = 0;
  if (*s == '.') {
    s++;
    if (!isdigit((unsigned char)*s)) return false;
    tenth = *s++ - '0';
  }
  if (*s) return false;
  dg = whole * 10 + tenth;
  if (negative) dg = -dg;
  return true;
}

// Reads one CSV row. False for the header line or a broken row.
// 9 fields = a row from before 1 Nov 2026 (no date_time), 10 = with it.
static bool parseRow(char* line, DoseRow& r) {
  char* f[11];
  uint8_t n = csvSplit(line, f, 11);
  if (n != 9 && n != 10) return false;
  r.time[0] = 0;
  if (n == 10 && clockTextOk(f[9])) { strncpy(r.time, f[9], sizeof(r.time) - 1); r.time[sizeof(r.time) - 1] = 0; }
  char* end;
  r.no = strtoul(f[0], &end, 10);       if (*end || r.no == 0) return false;
  r.boot = strtoul(f[1], &end, 10);     if (*end) return false;
  r.uptimeS = strtoul(f[2], &end, 10);  if (*end) return false;
  unsigned long seq = strtoul(f[3], &end, 10);
  if (*end || seq > 255) return false;
  r.seq = (uint8_t)seq;
  if (strlen(f[4]) != 1 || f[4][0] < 'A' || f[4][0] > 'C') return false;
  r.recipe = f[4][0];
  return gramsToDg(f[5], r.targetDg) && gramsToDg(f[6], r.actualDg);
}

void doseLogBegin() {
  // Boot counter kept in the ESP32's own flash (NVS), not on the SD card.
  Preferences prefs;
  prefs.begin("agridose", false);
  bootNo = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootNo);
  prefs.end();

  sdRecoverEdit(DOSE_LOG_PATH);      // a header swap cut short by a power cut
  sdCreateIfMissing(DOSE_LOG_PATH, DOSE_LOG_HEADER);

  // A log from before 1 Nov 2026 has the 9-column header: swap in the new
  // one (with date_time) once, so the downloaded CSV has a name over every
  // column. The rows stay as they are. Nothing else ever rewrites this file.
  // Only the first line is read to check (sdReplaceLine alone would scan the
  // whole log on every start).
  const size_t oldLen = sizeof(DOSE_LOG_HEADER_OLD) - 1;
  uint8_t first[sizeof(DOSE_LOG_HEADER_OLD) + 1];
  int got = sdReadAt(DOSE_LOG_PATH, 0, first, oldLen + 1);
  if (got == (int)oldLen + 1 && memcmp(first, DOSE_LOG_HEADER_OLD, oldLen) == 0 && (first[oldLen] == '\n' || first[oldLen] == '\r')) {
    SdEditResult up = sdReplaceLine(DOSE_LOG_PATH,
        [](const char* line, uint32_t lineNo) { return lineNo == 0 && strcmp(line, DOSE_LOG_HEADER_OLD) == 0; },
        DOSE_LOG_HEADER);
    if (up == SD_EDIT_OK) Serial.println("Dose log: header updated (date_time column added)");
    else Serial.println("Dose log: could not update the header (the rows are fine)");
  }

  // Find the newest row so dose numbers and seq numbers carry on after a restart.
  sdForEachLine(DOSE_LOG_PATH, [](char* line) {
    DoseRow r;
    if (parseRow(line, r)) { lastNo = r.no; lastSeqAtBoot = r.seq; }
  });
  Serial.printf("Boot number %lu. Dose log has %lu dose(s).\n", (unsigned long)bootNo, (unsigned long)lastNo);
}

bool doseLogAppend(const DoseResult& d) {
  char target[12], actual[12], error[12], when[20], line[128];
  int32_t errorDg = d.actualDg - d.targetDg;
  float errorPct = d.targetDg ? errorDg * 100.0f / d.targetDg : 0.0f;
  formatDg(target, sizeof(target), d.targetDg);
  formatDg(actual, sizeof(actual), d.actualDg);
  formatDg(error, sizeof(error), errorDg);
  clockNowText(when, sizeof(when));  // "" until a phone has given the ESP32 the time
  snprintf(line, sizeof(line), "%lu,%lu,%lu,%u,%c,%s,%s,%s,%.2f,%s",
           (unsigned long)(lastNo + 1), (unsigned long)bootNo, (unsigned long)(millis() / 1000),
           d.seq, d.recipe, target, actual, error, errorPct, when);
  if (!sdAppendLine(DOSE_LOG_PATH, line, DOSE_LOG_HEADER)) return false;
  lastNo++;                          // a number is used only once the row is really on the card
  return true;
}

uint8_t  doseLogLastSeq() { return lastSeqAtBoot; }
uint32_t doseLogCount()   { return lastNo; }
uint32_t doseLogBoot()    { return bootNo; }

int doseLogNewest(DoseRow* out, int max) {
  if (max <= 0) return 0;
  int seen = 0;
  // Read the file from the top and keep the last `max` rows in a ring:
  // row number k goes into slot k % max, overwriting older ones.
  bool ok = sdForEachLine(DOSE_LOG_PATH, [&](char* line) {
    DoseRow r;
    if (!parseRow(line, r)) return;
    out[seen % max] = r;
    seen++;
  });
  if (!ok) return -1;
  int n = seen < max ? seen : max;
  if (seen > max) std::rotate(out, out + (seen % max), out + max);  // oldest kept row to the front
  std::reverse(out, out + n);                                       // newest first
  return n;
}
