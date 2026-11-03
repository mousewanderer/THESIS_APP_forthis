#include "sd_store.h"
#include "config.h"
#include <SPI.h>
#include <SD.h>

static SPIClass sdSPI(VSPI);
static SemaphoreHandle_t sdMutex = nullptr;
static bool sdOk = false;

static bool sdLock()   { return sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(SD_WAIT_MS)) == pdTRUE; }
static void sdUnlock() { xSemaphoreGive(sdMutex); }

bool sdBegin() {
  sdMutex = xSemaphoreCreateMutex();
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  sdOk = SD.begin(SD_CS_PIN, sdSPI);
  if (sdOk) {
    Serial.println("SD card OK");
  } else {
    Serial.println("SD card not found -- dose log and crop records are off.");
    Serial.println("  If the wiring is right, try swapping SD_MISO_PIN / SD_MOSI_PIN.");
  }
  return sdOk;
}

bool sdReady() { return sdOk; }

bool sdAppendLine(const char* path, const char* line, const char* header) {
  if (!sdOk) return false;
  if (!sdLock()) { Serial.println("SD busy -- line not written"); return false; }
  if (header && !SD.exists(path)) {
    File h = SD.open(path, FILE_WRITE);
    if (h) { h.println(header); h.close(); }
  }
  bool ok = false;
  File f = SD.open(path, FILE_APPEND);
  if (f) {
    ok = f.println(line) > 0;
    f.close();
  }
  sdUnlock();
  if (!ok) {
    Serial.printf("SD write to %s failed -- marking the SD card offline\n", path);
    sdOk = false;
  }
  return ok;
}

int sdReadAt(const char* path, uint32_t offset, uint8_t* buf, size_t len) {
  if (!sdOk || !sdLock()) return -1;
  int n = -1;
  File f = SD.open(path, FILE_READ);
  if (f) {
    n = f.seek(offset) ? (int)f.read(buf, len) : 0;
    f.close();
  }
  sdUnlock();
  return n;
}

int32_t sdFileSize(const char* path) {
  if (!sdOk || !sdLock()) return -1;
  int32_t size = -1;
  File f = SD.open(path, FILE_READ);
  if (f) {
    size = (int32_t)f.size();
    f.close();
  }
  sdUnlock();
  return size;
}

bool sdMakeDir(const char* path) {
  if (!sdOk || !sdLock()) return false;
  bool ok = SD.exists(path) || SD.mkdir(path);
  sdUnlock();
  return ok;
}

bool sdCreateIfMissing(const char* path, const char* header) {
  if (!sdOk || !sdLock()) return false;
  bool ok = SD.exists(path);
  if (!ok) {
    File f = SD.open(path, FILE_WRITE);
    if (f) { ok = f.println(header) > 0; f.close(); }
  }
  sdUnlock();
  return ok;
}

bool sdRemove(const char* path) {
  if (!sdOk || !sdLock()) return false;
  bool ok = !SD.exists(path) || SD.remove(path);
  sdUnlock();
  return ok;
}

bool sdRename(const char* from, const char* to) {
  if (!sdOk || !sdLock()) return false;
  bool ok = SD.rename(from, to);
  sdUnlock();
  return ok;
}

// The file being written in pieces. It stays open between pieces; other
// tasks can still use the card in between (each piece is one mutex turn).
static File writing;
static bool writeOk = false;

bool sdWriteBegin(const char* path) {
  sdWriteEnd();
  if (!sdOk || !sdLock()) return false;
  writing = SD.open(path, FILE_WRITE);
  writeOk = (bool)writing;
  sdUnlock();
  return writeOk;
}

bool sdWriteMore(const uint8_t* data, size_t len) {
  if (!writing || !writeOk || !sdLock()) return writeOk = false;
  writeOk = writing.write(data, len) == len;
  sdUnlock();
  return writeOk;
}

bool sdWriteEnd() {
  if (!writing) return false;
  bool locked = sdLock();          // card stuck? close anyway: the file must not stay open
  writing.close();                 // after this, `writing` tests false
  if (locked) sdUnlock();
  else writeOk = false;
  return writeOk;
}

uint8_t csvSplit(char* line, char* fields[], uint8_t maxFields) {
  uint8_t n = 0;
  char* p = line;
  while (n < maxFields) {
    fields[n++] = p;
    char* comma = strchr(p, ',');
    if (comma == nullptr) break;
    *comma = 0;
    p = comma + 1;
  }
  return n;
}

// The line splitter behind sdForEachLine and sdReplaceLine. onLine gets each
// line with the file offsets of its first byte and of its end (the '\r' of
// a "\r\n", the '\n', or the end of the file), and returns false to stop.
static bool scanLines(const char* path, const std::function<bool(char* line, uint32_t start, uint32_t end)>& onLine) {
  char chunk[1024];
  char line[SD_LINE_MAX + 1];
  size_t lineLen = 0;
  bool tooLong = false;
  uint32_t pos = 0, lineStart = 0, crAt = UINT32_MAX;

  for (;;) {
    int n = sdReadAt(path, pos, (uint8_t*)chunk, sizeof(chunk));
    if (n < 0) return false;                     // missing file, or card busy / gone
    if (n == 0) break;                           // end of file
    for (int i = 0; i < n; i++) {
      char c = chunk[i];
      uint32_t at = pos + i;
      if (c == '\n') {
        line[lineLen] = 0;
        uint32_t end = (crAt + 1 == at) ? crAt : at;
        if (lineLen > 0 && !tooLong && !onLine(line, lineStart, end)) return true;
        lineLen = 0;
        tooLong = false;
        lineStart = at + 1;
      } else if (c == '\r') {
        crAt = at;                               // Windows line ending: not part of the text
      } else if (lineLen < SD_LINE_MAX) {
        line[lineLen++] = c;
      } else {
        tooLong = true;
      }
    }
    pos += n;
  }
  if (lineLen > 0 && !tooLong) {                 // last line without a newline
    line[lineLen] = 0;
    onLine(line, lineStart, pos);
  }
  return true;
}

bool sdForEachLine(const char* path, const std::function<void(char* line)>& onLine) {
  return scanLines(path, [&](char* line, uint32_t, uint32_t) { onLine(line); return true; });
}

// Copies bytes [from, to) of a file to out, one mutex turn per read and per write.
static bool copyBytes(const char* path, uint32_t from, uint32_t to, File& out) {
  uint8_t buf[512];
  while (from < to) {
    size_t want = to - from < sizeof(buf) ? to - from : sizeof(buf);
    int n = sdReadAt(path, from, buf, want);
    if (n <= 0 || !sdLock()) return false;
    bool ok = out.write(buf, n) == (size_t)n;
    sdUnlock();
    if (!ok) return false;
    from += n;
  }
  return true;
}

// A changed file is written whole as path.new...
static bool openNewCopy(const char* path, File& out) {
  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  if (!sdLock()) return false;
  out = SD.open(tmp, FILE_WRITE);
  sdUnlock();
  return (bool)out;
}

// ...then closed and swapped in (ok = every piece was written). Until the
// second rename the old file is still whole, as path or path.bak.
static bool swapInNewCopy(const char* path, File& out, bool ok) {
  char tmp[48], bak[48];
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  snprintf(bak, sizeof(bak), "%s.bak", path);
  bool locked = sdLock();          // card stuck? close anyway: the file must not stay open
  out.close();
  if (locked) sdUnlock();

  bool swapped = false;
  if (sdLock()) {
    if (ok) {
      SD.remove(bak);                              // left over from an earlier edit
      if (SD.rename(path, bak)) {
        if (SD.rename(tmp, path)) { SD.remove(bak); swapped = true; }
        else SD.rename(bak, path);                 // put the old file back
      }
    }
    if (!swapped) SD.remove(tmp);
    sdUnlock();
  }
  if (!swapped) Serial.printf("SD: could not rewrite %s\n", path);
  return swapped;
}

SdEditResult sdReplaceLine(const char* path, const std::function<bool(const char* line, uint32_t lineNo)>& pick,
                           const char* newLine) {
  if (!sdOk) return SD_EDIT_FAILED;
  uint32_t lineNo = 0, from = 0, to = 0;
  bool found = false;
  if (!scanLines(path, [&](char* line, uint32_t start, uint32_t end) {
        if (pick(line, lineNo)) { found = true; from = start; to = end; return false; }
        lineNo++;
        return true;
      })) return SD_EDIT_FAILED;
  if (!found) return SD_EDIT_NOT_FOUND;
  int32_t size = sdFileSize(path);
  File out;
  if (size < 0 || !openNewCopy(path, out)) return SD_EDIT_FAILED;

  // The new copy: the bytes before the line, the new line, the bytes after it.
  bool ok = copyBytes(path, 0, from, out);
  if (ok && sdLock()) {
    ok = out.write((const uint8_t*)newLine, strlen(newLine)) == strlen(newLine);
    sdUnlock();
  } else {
    ok = false;
  }
  ok = ok && copyBytes(path, to, (uint32_t)size, out);
  return swapInNewCopy(path, out, ok) ? SD_EDIT_OK : SD_EDIT_FAILED;
}

SdEditResult sdDropLines(const char* path, const std::function<bool(const char* line, uint32_t lineNo)>& pick, int& dropped) {
  dropped = 0;
  if (!sdOk) return SD_EDIT_FAILED;
  uint32_t lineNo = 0;
  bool any = false;                                // most files have no line to drop: leave them alone
  if (!scanLines(path, [&](char* line, uint32_t, uint32_t) {
        if (pick(line, lineNo++)) { any = true; return false; }
        return true;
      })) return SD_EDIT_FAILED;
  if (!any) return SD_EDIT_NOT_FOUND;
  int32_t size = sdFileSize(path);
  File out;
  if (size < 0 || !openNewCopy(path, out)) return SD_EDIT_FAILED;

  // The new copy: every byte except the dropped lines and their line ends.
  bool ok = true, skipping = false;
  uint32_t from = 0;
  lineNo = 0;
  ok = scanLines(path, [&](char* line, uint32_t start, uint32_t) {
    if (skipping) { from = start; skipping = false; }   // the dropped line ended just before this one
    if (pick(line, lineNo++)) {
      ok = ok && copyBytes(path, from, start, out);
      skipping = true;
      dropped++;
    }
    return ok;
  }) && ok;
  if (skipping) from = (uint32_t)size;             // the last line was dropped
  ok = ok && copyBytes(path, from, (uint32_t)size, out);
  return swapInNewCopy(path, out, ok) ? SD_EDIT_OK : SD_EDIT_FAILED;
}

void sdRecoverEdit(const char* path) {
  if (!sdOk || !sdLock()) return;
  char tmp[48], bak[48];
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  snprintf(bak, sizeof(bak), "%s.bak", path);
  // path.new is only swapped in after it was written whole, so with the file
  // gone it is the newest whole copy; next to the file it may be cut short.
  const char* restored = nullptr;
  if (SD.exists(tmp)) {
    if (SD.exists(path)) SD.remove(tmp);
    else if (SD.rename(tmp, path)) restored = "the finished edit";
  }
  if (SD.exists(bak)) {
    if (SD.exists(path)) SD.remove(bak);
    else if (SD.rename(bak, path)) restored = "the copy from before the edit";
  }
  sdUnlock();
  if (restored) Serial.printf("SD: %s was restored from %s (power cut during an edit)\n", path, restored);
}
