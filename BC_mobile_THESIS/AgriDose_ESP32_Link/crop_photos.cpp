#include "crop_photos.h"
#include "config.h"
#include "sd_store.h"
#include "crop_records.h"

#define PHOTO_DIR       CROP_DIR "/photos"
#define PHOTO_INDEX     CROP_DIR "/photos.csv"
#define PHOTO_HEADER    "photo_id,user_id,file,bytes,compressed_file,compressed_bytes"
#define WRITE_BUF       8192        // bytes gathered in RAM before each SD write (fewer, faster writes)
#define UPLOAD_STALE_MS 30000UL     // an upload with no new piece for this long is dropped

// Only the web server's task uses these (one upload at a time), so no lock.
static long lastId = 0;

enum Part : int8_t { PART_NONE = -1, PART_COMPRESSED = 0, PART_FULL = 1 };
static const char* const EXTS[]  = {"jpg", "png", "webp"};
static const char* const TYPES[] = {"image/jpeg", "image/png", "image/webp"};

struct Upload {
  const void* who = nullptr;        // the request that owns the upload (nullptr = none)
  long id = 0, userId = 0;
  uint32_t lastMs = 0;
  bool failed = false;
  char error[96] = "";
  int8_t part = PART_NONE;          // the file being written now
  int8_t ext = 0;                   // full file: index into EXTS
  bool created[2] = {false, false};
  bool done[2] = {false, false};
  uint32_t bytes[2] = {0, 0};
  uint8_t* buf = nullptr;
  size_t bufLen = 0;
};
static Upload up;

long photoLastId() { return lastId; }

static void photoPath(long id, int8_t part, int8_t ext, char* out, size_t size) {
  if (part == PART_COMPRESSED) snprintf(out, size, PHOTO_DIR "/%04ld_compressed.jpg", id);
  else snprintf(out, size, PHOTO_DIR "/%04ld.%s", id, EXTS[ext]);
}

void photoBegin() {
  if (!sdMakeDir(PHOTO_DIR)) { Serial.println("Photos: could not make " PHOTO_DIR); return; }
  sdCreateIfMissing(PHOTO_INDEX, PHOTO_HEADER);
  sdForEachLine(PHOTO_INDEX, [](char* line) {
    if (!isdigit((unsigned char)line[0])) return;   // header
    long id = strtol(line, nullptr, 10);
    if (id > lastId) lastId = id;
  });
  Serial.printf("Photos: %ld on the SD card\n", lastId);
}

bool photoFile(long id, bool full, char* path, size_t pathSize, const char*& type, int32_t& bytes) {
  if (id < 1 || id > lastId) return false;
  for (int8_t e = 0; e < (full ? 3 : 1); e++) {      // the save file may be .jpg, .png or .webp
    photoPath(id, full ? PART_FULL : PART_COMPRESSED, e, path, pathSize);
    bytes = sdFileSize(path);
    if (bytes >= 0) { type = TYPES[e]; return true; }
  }
  return false;
}

bool photoDeleteForUser(long userId, int& count) {
  count = 0;
  bool readOk = sdForEachLine(PHOTO_INDEX, [&](char* line) {
    char* f[3];
    if (!isdigit((unsigned char)line[0]) || csvSplit(line, f, 3) < 2 || strtol(f[1], nullptr, 10) != userId) return;
    long id = strtol(f[0], nullptr, 10);
    char path[48];
    const char* type;
    int32_t bytes;
    if (photoFile(id, true, path, sizeof(path), type, bytes)) sdRemove(path);
    if (photoFile(id, false, path, sizeof(path), type, bytes)) sdRemove(path);
    count++;
  });
  if (!readOk) return false;
  int dropped;
  return sdDropLines(PHOTO_INDEX, [userId](const char* line, uint32_t) {
    char* f[3];
    char copy[32];
    snprintf(copy, sizeof(copy), "%s", line);
    return isdigit((unsigned char)copy[0]) && csvSplit(copy, f, 3) >= 2 && strtol(f[1], nullptr, 10) == userId;
  }, dropped) != SD_EDIT_FAILED;
}

// True if the number id is in the list "12 13 20".
static bool inIdList(const char* ids, long id) {
  const char* p = ids;
  while (*p) {
    char* end;
    long n = strtol(p, &end, 10);
    if (end == p) return false;
    if (n == id) return true;
    p = end;
    while (*p == ' ') p++;
  }
  return false;
}

// 1 Nov 2026: the photos of one deleted record (cropDeleteRecord). Files
// first, then their rows in the index, the same way as a deleted farmer.
bool photoDeleteIds(const char* ids, long userId, int& count) {
  count = 0;
  if (!ids || !*ids) return true;
  bool readOk = sdForEachLine(PHOTO_INDEX, [&](char* line) {
    char* f[3];
    if (!isdigit((unsigned char)line[0]) || csvSplit(line, f, 3) < 2 || strtol(f[1], nullptr, 10) != userId) return;
    long id = strtol(f[0], nullptr, 10);
    if (!inIdList(ids, id)) return;
    char path[48];
    const char* type;
    int32_t bytes;
    if (photoFile(id, true, path, sizeof(path), type, bytes)) sdRemove(path);
    if (photoFile(id, false, path, sizeof(path), type, bytes)) sdRemove(path);
    count++;
  });
  if (!readOk) return false;
  if (count == 0) return true;
  int dropped;
  String list(ids);                  // the lambda keeps its own copy
  return sdDropLines(PHOTO_INDEX, [userId, list](const char* line, uint32_t) {
    char* f[3];
    char copy[32];
    snprintf(copy, sizeof(copy), "%s", line);
    return isdigit((unsigned char)copy[0]) && csvSplit(copy, f, 3) >= 2 && strtol(f[1], nullptr, 10) == userId &&
           inIdList(list.c_str(), strtol(f[0], nullptr, 10));
  }, dropped) != SD_EDIT_FAILED;
}

long photoOwner(long id) {
  long owner = 0;
  sdForEachLine(PHOTO_INDEX, [&](char* line) {
    char* f[3];
    if (owner || !isdigit((unsigned char)line[0]) || csvSplit(line, f, 3) < 2) return;
    if (strtol(f[0], nullptr, 10) == id) owner = strtol(f[1], nullptr, 10);
  });
  return owner;
}

// ---------------------------------------------------------- upload --

// Does the file start like the picture type its name says?
static bool looksLike(int8_t ext, const uint8_t* d, size_t len) {
  if (ext == 0) return len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
  if (ext == 1) return len >= 4 && d[0] == 0x89 && memcmp(d + 1, "PNG", 3) == 0;
  return len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0;
}

static void fail(const char* message) {
  if (!up.failed) snprintf(up.error, sizeof(up.error), "%s", message);
  up.failed = true;
  sdWriteEnd();
}

// keep = false: the files written so far are deleted.
static void endUpload(bool keep) {
  sdWriteEnd();
  if (!keep) {
    char path[48];
    for (int8_t p = PART_COMPRESSED; p <= PART_FULL; p++) {
      if (!up.created[p]) continue;
      photoPath(up.id, p, up.ext, path, sizeof(path));
      sdRemove(path);
    }
  }
  free(up.buf);
  up = Upload();
}

static void startUpload(const void* who, long userId) {
  up = Upload();
  up.who = who;
  up.userId = userId;
  up.id = lastId + 1;
  bool readOk;
  if (!sdReady()) fail("SD card problem: the photo cannot be saved");
  else if (!cropUserExists(userId, readOk)) fail(readOk ? "Pick a registered farmer first" : "Could not read the users file");
  else if ((up.buf = (uint8_t*)malloc(WRITE_BUF)) == nullptr) fail("The ESP32 is out of memory, try again");
}

static bool flush() {
  if (up.bufLen > 0 && !sdWriteMore(up.buf, up.bufLen)) {
    fail("Could not write the photo to the SD card (is it full?)");
    return false;
  }
  up.bufLen = 0;
  return true;
}

void photoUploadPiece(const void* who, long userId, const char* filename, size_t index,
                      const uint8_t* data, size_t len, bool final) {
  uint32_t now = millis();
  if (who != up.who) {
    bool firstFile = index == 0 && strcmp(filename, "compressed.jpg") == 0;
    if (!firstFile) return;                                    // a piece of an upload we turned away
    if (up.who && now - up.lastMs < UPLOAD_STALE_MS) return;   // another phone is uploading
    if (up.who) endUpload(false);                              // that phone went quiet: drop its upload
    startUpload(who, userId);
  }
  up.lastMs = now;
  if (up.failed) return;

  if (index == 0) {                                            // a new file starts
    int8_t part = PART_NONE, ext = 0;
    if (strcmp(filename, "compressed.jpg") == 0) {
      part = PART_COMPRESSED;
    } else if (strncmp(filename, "full.", 5) == 0) {
      for (int8_t e = 0; e < 3; e++) {
        if (strcmp(filename + 5, EXTS[e]) == 0) { part = PART_FULL; ext = e; }
      }
    }
    if (part == PART_NONE || up.done[part] || up.part != PART_NONE) { fail("Unexpected file in the upload"); return; }
    if (!looksLike(ext, data, len)) { fail("That file is not a JPEG, PNG or WebP photo"); return; }
    char path[48];
    if (part == PART_FULL) up.ext = ext;
    photoPath(up.id, part, ext, path, sizeof(path));
    up.created[part] = true;
    if (!sdWriteBegin(path)) { fail("Could not write the photo to the SD card"); return; }
    up.part = part;
  }
  if (up.part == PART_NONE) { fail("Broken upload, try again"); return; }

  size_t total = index + len;
  if (total > (up.part == PART_FULL ? PHOTO_FULL_MAX : PHOTO_COMPRESSED_MAX)) {
    fail(up.part == PART_FULL ? "The photo is too big for the SD card upload" : "The compressed photo is too big");
    return;
  }
  while (len > 0) {
    size_t n = min(len, (size_t)WRITE_BUF - up.bufLen);
    memcpy(up.buf + up.bufLen, data, n);
    up.bufLen += n;
    data += n;
    len -= n;
    if (up.bufLen == WRITE_BUF && !flush()) return;
  }
  if (final) {
    if (!flush()) return;
    if (!sdWriteEnd()) { fail("Could not write the photo to the SD card (is it full?)"); return; }
    up.bytes[up.part] = total;
    up.done[up.part] = true;
    up.part = PART_NONE;
  }
}

bool photoUploadFinish(const void* who, long& id, String& error) {
  if (who != up.who) {
    error = up.who ? "Another phone is uploading a photo. Try again in a moment." : "No photo arrived. Try again.";
    return false;
  }
  if (!up.failed && !(up.done[PART_COMPRESSED] && up.done[PART_FULL])) fail("The upload was cut off. Try again.");
  if (!up.failed) {
    char line[112];
    snprintf(line, sizeof(line), "%ld,%ld,%04ld.%s,%lu,%04ld_compressed.jpg,%lu", up.id, up.userId,
             up.id, EXTS[up.ext], (unsigned long)up.bytes[PART_FULL], up.id, (unsigned long)up.bytes[PART_COMPRESSED]);
    if (!sdAppendLine(PHOTO_INDEX, line, PHOTO_HEADER)) fail("Could not write to the SD card");
  }
  if (up.failed) {
    error = up.error;
    endUpload(false);
    return false;
  }
  id = lastId = up.id;
  Serial.printf("Photos: no. %ld saved for user %ld (%lu bytes, compressed %lu)\n", up.id, up.userId,
                (unsigned long)up.bytes[PART_FULL], (unsigned long)up.bytes[PART_COMPRESSED]);
  endUpload(true);
  return true;
}

void photoUploadGone(const void* who) {
  if (who != nullptr && who == up.who) {
    Serial.println("Photos: the phone went away mid-upload, partial files deleted");
    endUpload(false);
  }
}
