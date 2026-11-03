// =====================================================================
//  crop_validate.h -- checks for what farmers type into the crop forms.
//
//  Plain C++ with no Arduino calls, so tests/test_validate.cpp can run it
//  on a laptop. Each check returns true if the input is acceptable and
//  writes the cleaned value, ready for the CSV file, into out.
//  Blank input is handled by the caller (required or optional field).
// =====================================================================
#pragma once
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// True if s is empty or only spaces.
static inline bool cvIsBlank(const char* s) {
  while (*s) { if (!isspace((unsigned char)*s)) return false; s++; }
  return true;
}

// Copies s without the spaces at both ends. Returns the trimmed length,
// even if out was too small to hold all of it (so callers can reject it).
static inline size_t cvTrim(const char* s, char* out, size_t outSize) {
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
  size_t copy = n < outSize - 1 ? n : outSize - 1;
  memcpy(out, s, copy);
  out[copy] = 0;
  return n;
}

// Whole number from minV to maxV, digits only: "7", "12". Not "-1" or "1.5".
static inline bool cvInt(const char* in, long minV, long maxV, char* out, size_t outSize) {
  char t[16];
  size_t n = cvTrim(in, t, sizeof(t));
  if (n == 0 || n > 9) return false;
  for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)t[i])) return false;
  long v = strtol(t, nullptr, 10);
  if (v < minV || v > maxV) return false;
  snprintf(out, outSize, "%ld", v);
  return true;
}

// Number from minV to maxV with at most `decimals` digits after the point:
// "5", "5.5", "5.25". A comma is taken as the decimal point ("5,5").
// The value is stored with the decimals the farmer typed.
static inline bool cvDecimal(const char* in, double minV, double maxV, int decimals, char* out, size_t outSize) {
  char t[24];
  size_t n = cvTrim(in, t, sizeof(t));
  if (n == 0 || n > 12) return false;
  int before = 0, after = 0;
  bool point = false;
  for (size_t i = 0; i < n; i++) {
    char c = t[i];
    if (isdigit((unsigned char)c)) { if (point) after++; else before++; }
    else if ((c == '.' || c == ',') && !point) { point = true; t[i] = '.'; }
    else return false;
  }
  if (before == 0 || (point && after == 0) || after > decimals) return false;
  double v = strtod(t, nullptr);
  if (v < minV || v > maxV) return false;
  snprintf(out, outSize, "%.*f", after, v);
  return true;
}

// Date as YYYY-MM-DD (what a phone's date picker sends): a real calendar
// day, year 2000 to 2099.
static inline bool cvDate(const char* in, char* out, size_t outSize) {
  char t[16];
  if (cvTrim(in, t, sizeof(t)) != 10) return false;
  for (int i = 0; i < 10; i++) {
    bool dash = (i == 4 || i == 7);
    if (dash ? t[i] != '-' : !isdigit((unsigned char)t[i])) return false;
  }
  int y = atoi(t), m = atoi(t + 5), d = atoi(t + 8);
  static const int DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  if (y < 2000 || y > 2099 || m < 1 || m > 12) return false;
  int maxDay = DAYS[m - 1] + ((m == 2 && leap) ? 1 : 0);
  if (d < 1 || d > maxDay) return false;
  snprintf(out, outSize, "%s", t);
  return true;
}

// Free text, at most maxLen characters, spaces trimmed. Characters that
// would break the CSV file are swapped: ',' -> ';', '"' -> '\'',
// '\' -> '/', tabs and line breaks -> space.
static inline bool cvText(const char* in, size_t maxLen, char* out, size_t outSize) {
  while (*in && isspace((unsigned char)*in)) in++;
  size_t n = strlen(in);
  while (n > 0 && isspace((unsigned char)in[n - 1])) n--;
  if (n > maxLen || n >= outSize) return false;
  for (size_t i = 0; i < n; i++) {
    char c = in[i];
    if (c == ',') c = ';';
    else if (c == '"') c = '\'';
    else if (c == '\\') c = '/';
    else if ((unsigned char)c < 32 || c == 127) c = ' ';
    out[i] = c;
  }
  out[n] = 0;
  return true;
}

// "yes" or "no", any capitals.
static inline bool cvYesNo(const char* in, char* out, size_t outSize) {
  char t[8];
  if (cvTrim(in, t, sizeof(t)) > 3) return false;
  for (char* p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
  if (strcmp(t, "yes") != 0 && strcmp(t, "no") != 0) return false;
  snprintf(out, outSize, "%s", t);
  return true;
}

// Dose numbers such as "12 13" or "12, 13": digits separated by spaces,
// commas or semicolons. Stored as "12 13".
static inline bool cvNumberList(const char* in, size_t maxLen, char* out, size_t outSize) {
  size_t o = 0;
  bool inNumber = false;
  for (const char* p = in; *p; p++) {
    char c = *p;
    if (isdigit((unsigned char)c)) {
      if (!inNumber && o > 0) { if (o + 1 >= outSize) return false; out[o++] = ' '; }
      if (o + 1 >= outSize) return false;
      out[o++] = c;
      inNumber = true;
    } else if (c == ' ' || c == ',' || c == ';') {
      inNumber = false;
    } else {
      return false;
    }
  }
  out[o] = 0;
  return o <= maxLen;
}

// Photo numbers such as "12 13": at most maxCount of them, each from 1 to
// lastId (the newest photo on the card), no number twice. Stored as "12 13".
static inline bool cvPhotoList(const char* in, size_t maxCount, long lastId, char* out, size_t outSize) {
  char t[48];
  if (maxCount > 8 || !cvNumberList(in, sizeof(t) - 1, t, sizeof(t))) return false;
  long seen[8];
  size_t count = 0, o = 0;
  out[0] = 0;
  for (char* p = t; *p;) {
    long v = strtol(p, &p, 10);
    if (v < 1 || v > lastId || count >= maxCount) return false;
    for (size_t i = 0; i < count; i++) if (seen[i] == v) return false;
    seen[count++] = v;
    int n = snprintf(out + o, outSize - o, count > 1 ? " %ld" : "%ld", v);
    if (n < 0 || (size_t)n >= outSize - o) return false;
    o += (size_t)n;
  }
  return true;
}
