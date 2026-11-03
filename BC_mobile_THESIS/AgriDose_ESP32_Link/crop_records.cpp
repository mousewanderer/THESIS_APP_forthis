#include "crop_records.h"
#include "config.h"
#include "sd_store.h"
#include "crop_validate.h"
#include "crop_photos.h"
#include "dose_log.h"
#include "json_util.h"

#define USERS_PATH   CROP_DIR "/users.csv"
#define USERS_HEADER "user_id,name"
#define USER_ID_MAX  9999
#define USER_NAME_MAX     30
#define MAX_LIST     100           // "View details" shows at most the newest 100

// ---- what each record type contains ----
enum FieldKind : uint8_t { K_INT, K_DECIMAL, K_DATE, K_TEXT, K_YESNO, K_NUMLIST, K_PHOTOS };
static const char* const KIND_NAMES[] = {"int", "decimal", "date", "text", "yesno", "numlist", "photos"};

struct FieldSpec {
  const char* name;       // CSV column and form field name
  const char* label;      // shown on the page and in error messages
  FieldKind   kind;
  bool        required;
  long        minV, maxV; // K_INT and K_DECIMAL
  uint8_t     size;       // K_DECIMAL: decimals allowed. K_TEXT / K_NUMLIST: max characters. K_PHOTOS: max photos
  const char* step;       // K_INT / K_DECIMAL: what the - and + buttons add (nullptr = 1)
  const char* options;    // K_TEXT: choices for the drop-down, split by '|' (the farmer can still type another)
};

// The choice lists. Edit them here; the page shows whatever the ESP32 sends.
// The seed types are the Crop Guide's crops (www/agri.js), so the Recommend
// tab finds each planting's pH range.
#define SEED_TYPES    "Pechay|Rice (lowland)|Corn|Tomato|Eggplant|Okra|Bitter gourd|String bean|" \
                      "Squash|Cabbage|Water spinach|Sweet potato"
#define GROWTH_STAGES "Seedling|Vegetative|Flowering|Fruiting|Harvest"
// The first word must stay: the Recommend tab knows urea, ammonium sulfate
// and complete fertilizer by it (www/agri.js, fertKey). Others are listed
// but not counted in the pH estimate.
#define FERTILIZERS   "Urea (46-0-0)|Ammonium sulfate (21-0-0)|Complete (14-14-14)"

static const FieldSpec SOIL_FIELDS[] = {
  {"plot",              "Plot number",       K_INT,     true,  1, 99,  0, nullptr, nullptr},
  {"date_tested",       "Soil test date",    K_DATE,    true,  0, 0,   0, nullptr, nullptr},
  {"soil_ph",           "Soil pH",           K_DECIMAL, true,  0, 14,  2, "0.1",   nullptr},
  {"soil_moisture_pct", "Soil moisture (%)", K_DECIMAL, false, 0, 100, 1, nullptr, nullptr},
  {"photos",            "Photos",            K_PHOTOS,  false, 0, 0,   PHOTOS_PER_RECORD, nullptr, nullptr},
};
static const FieldSpec PLANTING_FIELDS[] = {
  {"plot",          "Plot number",             K_INT,     true,  1, 99, 0,  nullptr, nullptr},
  {"crop",          "Seed type",               K_TEXT,    true,  0, 0,  30, nullptr, SEED_TYPES},
  {"variety",       "Variety",                 K_TEXT,    false, 0, 0,  30, nullptr, nullptr},
  {"date_planted",  "Date planted",            K_DATE,    true,  0, 0,  0,  nullptr, nullptr},
  {"used_agridose", "Used AgriDose amendment", K_YESNO,   true,  0, 0,  0,  nullptr, nullptr},
  {"dose_numbers",  "AgriDose dose numbers",   K_NUMLIST, false, 0, 0,  40, nullptr, nullptr},
  {"photos",        "Photos",                  K_PHOTOS,  false, 0, 0,  PHOTOS_PER_RECORD, nullptr, nullptr},
};
// The page fills in date_planted from the newest planting of the same plot,
// and the day count from the two dates; the farmer can change both.
static const FieldSpec GROWTH_FIELDS[] = {
  {"plot",                "Plot number",          K_INT,     true,  1, 99,  0,  nullptr, nullptr},
  {"stage",               "Growth stage",         K_TEXT,    true,  0, 0,   20, nullptr, GROWTH_STAGES},
  {"date_observed",       "Growth date observed", K_DATE,    true,  0, 0,   0,  nullptr, nullptr},
  {"date_planted",        "Date planted",         K_DATE,    false, 0, 0,   0,  nullptr, nullptr},
  {"days_after_planting", "Growth day count",     K_INT,     false, 0, 365, 0,  nullptr, nullptr},
  {"height_cm",           "Plant height (cm)",    K_DECIMAL, false, 0, 500, 1,  "0.5",   nullptr},
  {"leaf_count",          "Leaf count",           K_INT,     false, 0, 999, 0,  nullptr, nullptr},
  {"notes",               "Notes",                K_TEXT,    false, 0, 0,   80, nullptr, nullptr},
  {"photos",              "Photos",               K_PHOTOS,  false, 0, 0,   PHOTOS_PER_RECORD, nullptr, nullptr},
};
static const FieldSpec FERT_FIELDS[] = {
  {"plot",         "Plot number",  K_INT,     true,  1, 99,  0,  nullptr, nullptr},
  {"date_applied", "Date applied", K_DATE,    true,  0, 0,   0,  nullptr, nullptr},
  {"fertilizer",   "Fertilizer",   K_TEXT,    true,  0, 0,   30, nullptr, FERTILIZERS},
  {"amount_kg",    "Amount (kg)",  K_DECIMAL, false, 0, 100, 2,  "0.5",   nullptr},
  {"notes",        "Notes",        K_TEXT,    false, 0, 0,   80, nullptr, nullptr},
};

// NO DUPLICATES: a farmer can't have two records of one type with the same
// values in these fields (a second save of the same thing); they edit the
// one they have instead.
static const char* const SOIL_KEY[]     = {"plot", "date_tested", nullptr};
static const char* const PLANTING_KEY[] = {"plot", "date_planted", nullptr};
static const char* const GROWTH_KEY[]   = {"plot", "date_observed", nullptr};
static const char* const FERT_KEY[]     = {"plot", "date_applied", "fertilizer", nullptr};

struct RecordSpec {
  const char* type;      // used in the URL: /api/records/<type>
  const char* title;
  const char* one;       // one record, for messages: "soil test"
  const char* path;
  const FieldSpec* fields;
  uint8_t count;
  const char* const* key;
};

#define COUNT_OF(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))
static const RecordSpec SPECS[REC_TYPE_COUNT] = {
  {"soil",     "Soil tests",     "soil test",     CROP_DIR "/soil.csv",      SOIL_FIELDS,     COUNT_OF(SOIL_FIELDS), SOIL_KEY},
  {"planting", "Seed type and planting", "planting", CROP_DIR "/plantings.csv", PLANTING_FIELDS, COUNT_OF(PLANTING_FIELDS), PLANTING_KEY},
  {"growth",   "Growth records", "growth record", CROP_DIR "/growth.csv",    GROWTH_FIELDS,   COUNT_OF(GROWTH_FIELDS), GROWTH_KEY},
  {"fertilizer", "Fertilizer applications", "fertilizer application", CROP_DIR "/fertilizer.csv", FERT_FIELDS, COUNT_OF(FERT_FIELDS), FERT_KEY},
};

const char* cropTypeName(RecordType t) { return SPECS[t].type; }
const char* cropTypePath(RecordType t) { return SPECS[t].path; }
const char* cropUsersPath() { return USERS_PATH; }

// "user_id,plot,date_tested,..."
static void buildHeader(const RecordSpec& s, char* out, size_t size) {
  size_t n = snprintf(out, size, "user_id");
  for (uint8_t i = 0; i < s.count && n < size; i++) n += snprintf(out + n, size - n, ",%s", s.fields[i].name);
}

// A file made by an older version of this sketch has other columns. It is
// renamed to soil_old.csv (soil_old2.csv, ...) and a new file is started,
// so old and new rows never end up under the wrong header.
static void startFile(const char* path, const char* header) {
  char first[SD_LINE_MAX + 1];
  int n = sdReadAt(path, 0, (uint8_t*)first, sizeof(first) - 1);
  if (n > 0) {
    first[n] = 0;
    first[strcspn(first, "\r\n")] = 0;
    if (strcmp(first, header) != 0) {
      char old[48];
      int stem = (int)(strrchr(path, '.') - path);   // "/crop/soil"
      for (int k = 1; k < 100; k++) {
        if (k == 1) snprintf(old, sizeof(old), "%.*s_old.csv", stem, path);
        else snprintf(old, sizeof(old), "%.*s_old%d.csv", stem, path, k);
        if (sdFileSize(old) < 0) break;              // this name is free
      }
      bool moved = sdRename(path, old);
      Serial.printf("Crop records: %s has old columns, %s %s\n", path, moved ? "renamed to" : "COULD NOT rename to", old);
    }
  }
  sdCreateIfMissing(path, header);
}

void cropBegin() {
  if (!sdMakeDir(CROP_DIR)) { Serial.println("Crop records: could not make " CROP_DIR); return; }
  sdRecoverEdit(USERS_PATH);
  sdCreateIfMissing(USERS_PATH, USERS_HEADER);
  for (uint8_t t = 0; t < REC_TYPE_COUNT; t++) {
    char header[160];
    buildHeader(SPECS[t], header, sizeof(header));
    sdRecoverEdit(SPECS[t].path);
    startFile(SPECS[t].path, header);
  }
}

// FNV-1a of a whole CSV line: the "rev" a phone sends back with an edit,
// so the line is only replaced if it is still the one the phone was shown.
static uint32_t lineRev(const char* line) {
  uint32_t h = 2166136261u;
  for (; *line; line++) {
    h ^= (uint8_t)*line;
    h *= 16777619u;
  }
  return h;
}

// ---- users ----

bool cropUserExists(long id, bool& readOk) {
  bool found = false;
  readOk = sdForEachLine(USERS_PATH, [&](char* line) {
    if (isdigit((unsigned char)line[0]) && strtol(line, nullptr, 10) == id) found = true;
  });
  return found;
}

bool cropWriteUsersJson(Print& out, String& error, long onlyId) {
  bool first = true;
  out.print('[');
  bool ok = sdForEachLine(USERS_PATH, [&](char* line) {
    char* f[3];
    if (!isdigit((unsigned char)line[0]) || csvSplit(line, f, 3) < 2) return;   // header or broken row
    long id = strtol(f[0], nullptr, 10);
    if (onlyId && id != onlyId) return;
    out.printf("%s{\"id\":%ld,\"name\":", first ? "" : ",", id);
    jsonString(out, f[1]);
    out.print('}');
    first = false;
  });
  out.print(']');
  if (!ok) error = "Could not read the SD card";
  return ok;
}

// Checks the user ID and name of the farmer form.
static bool checkUserForm(const FieldGetter& get, char* id, size_t idSize, char* name, size_t nameSize, String& error) {
  if (!cvInt(get("user_id").c_str(), 1, USER_ID_MAX, id, idSize)) {
    error = "User ID must be a whole number from 1 to " + String(USER_ID_MAX);
    return false;
  }
  String rawName = get("name");
  if (cvIsBlank(rawName.c_str()) || !cvText(rawName.c_str(), USER_NAME_MAX, name, nameSize)) {
    error = "Name is required (at most " + String(USER_NAME_MAX) + " characters)";
    return false;
  }
  return true;
}

// NO DUPLICATE NAMES. Two names are the same if they only differ in
// capitals and spaces: "Rhea  Bolo " = "rhea bolo".
static void nameKey(const char* name, char* out, size_t size) {
  size_t o = 0;
  bool space = false;
  for (const char* p = name; *p && o + 2 < size; p++) {
    if (isspace((unsigned char)*p)) { space = o > 0; continue; }
    if (space) out[o++] = ' ';
    space = false;
    out[o++] = (char)tolower((unsigned char)*p);
  }
  out[o] = 0;
}

// False (with the reason) if another farmer than exceptId has this name.
static bool nameFree(const char* name, long exceptId, String& error) {
  char want[USER_NAME_MAX + 2], have[USER_NAME_MAX + 2];
  nameKey(name, want, sizeof(want));
  long other = 0;
  bool readOk = sdForEachLine(USERS_PATH, [&](char* line) {
    char* f[3];
    if (other || !isdigit((unsigned char)line[0]) || csvSplit(line, f, 3) < 2) return;
    long id = strtol(f[0], nullptr, 10);
    nameKey(f[1], have, sizeof(have));
    if (id != exceptId && strcmp(want, have) == 0) other = id;
  });
  if (!readOk) { error = "Could not read the users file"; return false; }
  if (other) {
    error = String("Farmer ID ") + other + " is already called " + name + ". Add an initial or a family name to tell them apart.";
    return false;
  }
  return true;
}

// Note: the web server runs every request one after another in one task,
// so two phones cannot both pass the "already exists" checks at once.
bool cropAddUser(const FieldGetter& get, String& error) {
  if (!sdReady()) { error = "SD card problem"; return false; }
  char id[12], name[USER_NAME_MAX + 1];
  if (!checkUserForm(get, id, sizeof(id), name, sizeof(name), error)) return false;
  bool readOk;
  if (cropUserExists(atol(id), readOk)) { error = "User ID " + String(id) + " already exists"; return false; }
  if (!readOk) { error = "Could not read the users file"; return false; }
  if (!nameFree(name, 0, error)) return false;

  char line[48];
  snprintf(line, sizeof(line), "%s,%s", id, name);
  if (!sdAppendLine(USERS_PATH, line, USERS_HEADER)) { error = "Could not write to the SD card"; return false; }
  Serial.printf("Crop records: new user %s (%s)\n", id, name);
  return true;
}

// The user ID stays (the records point to it); only the name changes.
bool cropEditUser(const FieldGetter& get, String& error) {
  if (!sdReady()) { error = "SD card problem"; return false; }
  char id[12], name[USER_NAME_MAX + 1];
  if (!checkUserForm(get, id, sizeof(id), name, sizeof(name), error)) return false;
  long want = atol(id);
  if (!nameFree(name, want, error)) return false;

  char line[48];
  snprintf(line, sizeof(line), "%s,%s", id, name);
  SdEditResult r = sdReplaceLine(USERS_PATH, [want](const char* old, uint32_t) {
    return isdigit((unsigned char)old[0]) && strtol(old, nullptr, 10) == want;
  }, line);
  if (r == SD_EDIT_NOT_FOUND) { error = "User ID " + String(id) + " is not registered"; return false; }
  if (r != SD_EDIT_OK) { error = "Could not write to the SD card"; return false; }
  Serial.printf("Crop records: user %s renamed to %s\n", id, name);
  return true;
}

// Deletes a farmer and everything that is theirs: their crop records, their
// photos, and last their row in users.csv, so a failure part-way leaves the
// farmer there to delete again. The dose log is the machine's and stays.
bool cropDeleteUser(long id, String& error) {
  if (!sdReady()) { error = "SD card problem"; return false; }
  bool readOk;
  if (!cropUserExists(id, readOk)) {
    error = readOk ? "User ID " + String(id) + " is not registered" : String("Could not read the users file");
    return false;
  }
  char key[16];
  snprintf(key, sizeof(key), "%ld,", id);
  size_t keyLen = strlen(key);
  int records = 0, n;
  for (uint8_t t = 0; t < REC_TYPE_COUNT; t++) {
    SdEditResult r = sdDropLines(SPECS[t].path, [&](const char* line, uint32_t) { return strncmp(line, key, keyLen) == 0; }, n);
    if (r == SD_EDIT_FAILED) { error = String("Could not change ") + SPECS[t].path; return false; }
    records += n;
  }
  int photos;
  if (!photoDeleteForUser(id, photos)) { error = "Could not delete the photos"; return false; }
  SdEditResult r = sdDropLines(USERS_PATH, [id](const char* line, uint32_t) {
    return isdigit((unsigned char)line[0]) && strtol(line, nullptr, 10) == id;
  }, n);
  if (r != SD_EDIT_OK) { error = "Could not write to the SD card"; return false; }
  Serial.printf("Crop records: user %ld deleted, with %d record(s) and %d photo(s)\n", id, records, photos);
  return true;
}

// ---- records ----

// Checks one form value against its FieldSpec and writes the cleaned value.
static bool checkField(const FieldSpec& f, const char* raw, char* out, size_t outSize, String& error) {
  if (cvIsBlank(raw)) {
    out[0] = 0;
    if (!f.required) return true;
    error = String(f.label) + " is required";
    return false;
  }
  bool ok = false;
  String range = " from " + String(f.minV) + " to " + String(f.maxV);
  switch (f.kind) {
    case K_INT:
      ok = cvInt(raw, f.minV, f.maxV, out, outSize);
      if (!ok) error = String(f.label) + " must be a whole number" + range;
      break;
    case K_DECIMAL:
      ok = cvDecimal(raw, f.minV, f.maxV, f.size, out, outSize);
      if (!ok) error = String(f.label) + " must be a number" + range + " (at most " + String(f.size) + " decimals)";
      break;
    case K_DATE:
      ok = cvDate(raw, out, outSize);
      if (!ok) error = String(f.label) + " must be a date (YYYY-MM-DD)";
      break;
    case K_TEXT:
      ok = cvText(raw, f.size, out, outSize);
      if (!ok) error = String(f.label) + " must be at most " + String(f.size) + " characters";
      break;
    case K_YESNO:
      ok = cvYesNo(raw, out, outSize);
      if (!ok) error = String(f.label) + " must be yes or no";
      break;
    case K_NUMLIST:
      ok = cvNumberList(raw, f.size, out, outSize);
      if (!ok) error = String(f.label) + " must be numbers separated by spaces";
      break;
    case K_PHOTOS:
      ok = cvPhotoList(raw, f.size, photoLastId(), out, outSize);
      if (!ok) error = "A photo is not on the SD card (at most " + String(f.size) + " photos). Add the photos again.";
      break;
  }
  return ok;
}

// The user_id of a record form, if it is a registered farmer.
static bool registeredUser(const FieldGetter& get, char* id, size_t idSize, String& error) {
  if (!cvInt(get("user_id").c_str(), 1, USER_ID_MAX, id, idSize)) { error = "Pick a farmer (user ID) first"; return false; }
  bool readOk;
  if (!cropUserExists(atol(id), readOk)) {
    error = readOk ? "User ID " + String(id) + " is not registered" : String("Could not read the users file");
    return false;
  }
  return true;
}

// Checks every field of a record form and builds its CSV line, user ID first.
static bool buildRecordLine(const RecordSpec& s, const char* id, const FieldGetter& get, char* line, size_t size, String& error) {
  size_t n = snprintf(line, size, "%s", id);
  for (uint8_t i = 0; i < s.count; i++) {
    char value[96];
    if (!checkField(s.fields[i], get(s.fields[i].name).c_str(), value, sizeof(value), error)) return false;
    n += snprintf(line + n, size - n, ",%s", value);
    if (n >= size) { error = "Record is too long"; return false; }
  }
  return true;
}

// ---- the data rules, checked on every save (add or edit) ----

// A record line split into its fields: f[0] is the user ID, f[i + 1] the
// value of s.fields[i].
struct Fields {
  char buf[SD_LINE_MAX + 1];
  char* f[16];
  uint8_t n;
  explicit Fields(const char* line) { snprintf(buf, sizeof(buf), "%s", line); n = csvSplit(buf, f, 16); }
};

static int fieldIndex(const RecordSpec& s, const char* name) {
  for (uint8_t i = 0; i < s.count; i++) if (strcmp(s.fields[i].name, name) == 0) return i;
  return -1;
}

// The value of field `name` in a record ("" if the line is too short).
static const char* valueOf(const RecordSpec& s, const Fields& r, const char* name) {
  int i = fieldIndex(s, name);
  return (i >= 0 && i + 1 < r.n) ? r.f[i + 1] : "";
}

// How many of this farmer's records of type t have `value` in `field`
// (capitals ignored). skipLine: a line not to count (0 = none; line 0 is the header).
static int countMatching(RecordType t, const char* id, const char* field, const char* value, uint32_t skipLine, bool& readOk) {
  const RecordSpec& s = SPECS[t];
  char key[16];
  snprintf(key, sizeof(key), "%s,", id);
  size_t keyLen = strlen(key);
  int count = 0;
  uint32_t lineNo = 0;
  readOk = sdForEachLine(s.path, [&](char* line) {
    uint32_t at = lineNo++;
    if (at == skipLine || strncmp(line, key, keyLen) != 0) return;
    Fields other(line);
    if (strcasecmp(valueOf(s, other, field), value) == 0) count++;
  });
  return count;
}

// True if this farmer has another record of the type with the same key
// fields (NO DUPLICATES).
static bool isDuplicate(const RecordSpec& s, const Fields& rec, const char* id, uint32_t skipLine, bool& readOk) {
  char key[16];
  snprintf(key, sizeof(key), "%s,", id);
  size_t keyLen = strlen(key);
  bool dup = false;
  uint32_t lineNo = 0;
  readOk = sdForEachLine(s.path, [&](char* line) {
    uint32_t at = lineNo++;
    if (dup || at == skipLine || strncmp(line, key, keyLen) != 0) return;
    Fields other(line);
    for (const char* const* k = s.key; *k; k++)
      if (strcasecmp(valueOf(s, rec, *k), valueOf(s, other, *k)) != 0) return;
    dup = true;
  });
  return dup;
}

// The data rules (30 Oct), for the record in `line`. skipLine: the line
// being edited (0 for a new record), so it is not its own duplicate.
//   - no duplicate records (the KEY fields above)
//   - no date after today; the ESP32 has no clock, so the phone sends its
//     own date as "today" (the page always does)
//   - a growth record: observed on or after the date planted, on a plot
//     that has a planting (records must link up)
//   - a planting's dose numbers: doses that are in the Batch Log
static bool checkRules(RecordType t, const char* line, const char* id, uint32_t skipLine, const FieldGetter& get, String& error) {
  const RecordSpec& s = SPECS[t];
  Fields rec(line);
  bool readOk = true;

  char today[12];
  if (cvDate(get("today").c_str(), today, sizeof(today))) {
    for (uint8_t i = 0; i < s.count && i + 1 < rec.n; i++) {
      const char* v = rec.f[i + 1];
      if (s.fields[i].kind == K_DATE && *v && strcmp(v, today) > 0) {
        error = String(s.fields[i].label) + " can't be after today (" + today + ")";
        return false;
      }
    }
  }

  if (t == REC_GROWTH) {
    const char* planted = valueOf(s, rec, "date_planted");
    if (*planted && strcmp(valueOf(s, rec, "date_observed"), planted) < 0) {
      error = String("The date observed can't be before the date planted (") + planted + ")";
      return false;
    }
    const char* plot = valueOf(s, rec, "plot");
    if (countMatching(REC_PLANTING, id, "plot", plot, 0, readOk) == 0) {
      error = readOk ? String("Plot ") + plot + " has no planting. Add the planting first." : String("Could not read the SD card");
      return false;
    }
  }

  if (t == REC_PLANTING) {
    uint32_t newest = doseLogCount();               // dose numbers run 1, 2, 3, ... and never repeat
    const char* p = valueOf(s, rec, "dose_numbers");
    while (*p) {
      char* end;
      unsigned long n = strtoul(p, &end, 10);
      if (end == p) break;
      if (n < 1 || n > newest) {
        error = String("Dose ") + n + " is not in the Batch Log" +
                (newest ? String(" (it has doses 1 to ") + newest + ")" : String(" (no doses yet)"));
        return false;
      }
      p = end;
      while (*p == ' ') p++;
    }
  }

  if (isDuplicate(s, rec, id, skipLine, readOk)) {
    String what;
    for (const char* const* k = s.key; *k; k++) {
      if (what.length()) what += k[1] ? ", " : " and ";
      what += String(s.fields[fieldIndex(s, *k)].label) + " " + valueOf(s, rec, *k);
    }
    error = String("This farmer already has a ") + s.one + " with " + what + ". Open it and tap Edit to change it.";
    return false;
  }
  if (!readOk) { error = "Could not read the SD card"; return false; }
  return true;
}

bool cropAddRecord(RecordType t, const FieldGetter& get, String& error) {
  const RecordSpec& s = SPECS[t];
  if (!sdReady()) { error = "SD card problem"; return false; }

  char id[12];
  if (!registeredUser(get, id, sizeof(id), error)) return false;
  char line[SD_LINE_MAX + 1];
  if (!buildRecordLine(s, id, get, line, sizeof(line), error)) return false;
  if (!checkRules(t, line, id, 0, get, error)) return false;

  char header[160];
  buildHeader(s, header, sizeof(header));
  if (!sdAppendLine(s.path, line, header)) { error = "Could not write to the SD card"; return false; }
  Serial.printf("Crop records: %s saved for user %s\n", s.type, id);
  return true;
}

// Replaces one record with the corrected values. row and rev come with the
// record from cropWriteRecordsJson; the line must still be that farmer's
// and unchanged, or nothing is written.
bool cropEditRecord(RecordType t, const FieldGetter& get, String& error) {
  const RecordSpec& s = SPECS[t];
  if (!sdReady()) { error = "SD card problem"; return false; }

  char id[12], rowText[12];
  if (!registeredUser(get, id, sizeof(id), error)) return false;
  if (!cvInt(get("row").c_str(), 1, 999999, rowText, sizeof(rowText))) { error = "Which record? The row number is missing"; return false; }
  uint32_t row = strtoul(rowText, nullptr, 10);
  uint32_t rev = strtoul(get("rev").c_str(), nullptr, 16);
  char line[SD_LINE_MAX + 1];
  if (!buildRecordLine(s, id, get, line, sizeof(line), error)) return false;
  if (!checkRules(t, line, id, row, get, error)) return false;

  char key[16];
  snprintf(key, sizeof(key), "%s,", id);
  size_t keyLen = strlen(key);
  SdEditResult r = sdReplaceLine(s.path, [&](const char* old, uint32_t lineNo) {
    return lineNo == row && strncmp(old, key, keyLen) == 0 && lineRev(old) == rev;
  }, line);
  if (r == SD_EDIT_NOT_FOUND) { error = "This record was changed since the list was loaded. Close it and open it again."; return false; }
  if (r != SD_EDIT_OK) { error = "Could not write to the SD card"; return false; }
  Serial.printf("Crop records: %s line %lu edited for user %s\n", s.type, (unsigned long)row, id);
  return true;
}

// Deletes one record (1 Nov 2026). row and rev come with the record from
// cropWriteRecordsJson, as for an edit: the line must still be that
// farmer's and unchanged, or nothing is deleted. A planting can't go while
// its plot has growth records and no other planting (they would have no
// planting, the same rule as adding one). The record's photos go after
// its line; if that part fails the record is still gone (left-over photo
// files do no harm).
bool cropDeleteRecord(RecordType t, const FieldGetter& get, String& error) {
  const RecordSpec& s = SPECS[t];
  if (!sdReady()) { error = "SD card problem"; return false; }

  char id[12], rowText[12];
  if (!registeredUser(get, id, sizeof(id), error)) return false;
  if (!cvInt(get("row").c_str(), 1, 999999, rowText, sizeof(rowText))) { error = "Which record? The row number is missing"; return false; }
  uint32_t row = strtoul(rowText, nullptr, 10);
  uint32_t rev = strtoul(get("rev").c_str(), nullptr, 16);
  char key[16];
  snprintf(key, sizeof(key), "%s,", id);
  size_t keyLen = strlen(key);
  const char* changed = "This record was changed since the list was loaded. Close it and open it again.";

  // Read the line first: its plot and photos are needed.
  char found[SD_LINE_MAX + 1] = "";
  uint32_t lineNo = 0;
  if (!sdForEachLine(s.path, [&](char* line) {
        if (lineNo++ == row && strncmp(line, key, keyLen) == 0 && lineRev(line) == rev) snprintf(found, sizeof(found), "%s", line);
      })) { error = "Could not read the SD card"; return false; }
  if (!found[0]) { error = changed; return false; }
  Fields rec(found);

  if (t == REC_PLANTING) {
    const char* plot = valueOf(s, rec, "plot");
    bool readOk = true, readOk2 = true;
    int growth = countMatching(REC_GROWTH, id, "plot", plot, 0, readOk);
    int others = countMatching(REC_PLANTING, id, "plot", plot, row, readOk2);
    if (!readOk || !readOk2) { error = "Could not read the SD card"; return false; }
    if (growth > 0 && others == 0) {
      error = String("Plot ") + plot + " has " + growth + " growth record(s) from this planting. Delete them first.";
      return false;
    }
  }

  int n;
  SdEditResult r = sdDropLines(s.path, [&](const char* line, uint32_t ln) {
    return ln == row && strncmp(line, key, keyLen) == 0 && lineRev(line) == rev;
  }, n);
  if (r == SD_EDIT_NOT_FOUND) { error = changed; return false; }
  if (r != SD_EDIT_OK) { error = "Could not write to the SD card"; return false; }

  int photos = 0;
  if (!photoDeleteIds(valueOf(s, rec, "photos"), strtol(id, nullptr, 10), photos))
    Serial.println("Crop records: the record is deleted, but its photos could not be removed");
  Serial.printf("Crop records: %s line %lu deleted for user %s, with %d photo(s)\n", s.type, (unsigned long)row, id, photos);
  return true;
}

bool cropWriteRecordsJson(RecordType t, long userId, int limit, Print& out, String& error) {
  const RecordSpec& s = SPECS[t];
  if (limit <= 0 || limit > MAX_LIST) limit = MAX_LIST;

  // Keep the newest `limit` lines of this user in a ring (line k -> slot k % limit),
  // with their line numbers in the file (the header is line 0) for edits.
  typedef char Line[SD_LINE_MAX + 1];
  Line* ring = (Line*)malloc(sizeof(Line) * limit);
  uint32_t* rows = (uint32_t*)malloc(sizeof(uint32_t) * limit);
  if (ring == nullptr || rows == nullptr) { free(ring); free(rows); error = "The ESP32 is out of memory, try again"; return false; }

  char key[16];
  snprintf(key, sizeof(key), "%ld,", userId);
  size_t keyLen = strlen(key);
  int seen = 0;
  uint32_t lineNo = 0;
  bool ok = sdForEachLine(s.path, [&](char* line) {
    if (strncmp(line, key, keyLen) == 0) {
      strcpy(ring[seen % limit], line);
      rows[seen % limit] = lineNo;
      seen++;
    }
    lineNo++;
  });
  if (!ok) { free(ring); free(rows); error = "Could not read the SD card"; return false; }

  out.printf("{\"type\":\"%s\",\"user_id\":%ld,\"total\":%d,\"items\":[", s.type, userId, seen);
  int n = seen < limit ? seen : limit;
  for (int k = 0; k < n; k++) {
    int slot = (seen - 1 - k) % limit;                              // newest first
    out.printf("%s{\"row\":%lu,\"rev\":\"%08lx\"", k ? "," : "", (unsigned long)rows[slot], (unsigned long)lineRev(ring[slot]));
    char* f[16];
    uint8_t nf = csvSplit(ring[slot], f, 16);
    for (uint8_t i = 0; i < s.count; i++) {
      out.print(',');
      jsonString(out, s.fields[i].name);
      out.print(':');
      jsonString(out, (uint8_t)(i + 1) < nf ? f[i + 1] : "");
    }
    out.print('}');
  }
  out.print("]}");
  free(ring);
  free(rows);
  return true;
}

// A farmer's own download: the header and that farmer's lines only (the
// admin gets the whole file straight from the card, web_server.cpp).
bool cropWriteRecordsCsv(RecordType t, long userId, Print& out, String& error) {
  char key[16];
  snprintf(key, sizeof(key), "%ld,", userId);
  size_t keyLen = strlen(key);
  uint32_t lineNo = 0;
  bool ok = sdForEachLine(SPECS[t].path, [&](char* line) {
    if (lineNo++ == 0 || strncmp(line, key, keyLen) == 0) { out.print(line); out.print("\r\n"); }
  });
  if (!ok) error = "Could not read the SD card";
  return ok;
}

void cropWriteTypesJson(Print& out) {
  out.print('[');
  for (uint8_t t = 0; t < REC_TYPE_COUNT; t++) {
    const RecordSpec& s = SPECS[t];
    out.printf("%s{\"type\":\"%s\",\"title\":\"%s\",\"fields\":[", t ? "," : "", s.type, s.title);
    for (uint8_t i = 0; i < s.count; i++) {
      const FieldSpec& f = s.fields[i];
      out.printf("%s{\"name\":\"%s\",\"label\":\"%s\",\"kind\":\"%s\",\"required\":%s,\"min\":%ld,\"max\":%ld,\"size\":%u",
                 i ? "," : "", f.name, f.label, KIND_NAMES[f.kind], f.required ? "true" : "false", f.minV, f.maxV, f.size);
      if (f.step) out.printf(",\"step\":\"%s\"", f.step);
      if (f.options) {                       // "A|B" -> "options":["A","B"]
        out.print(",\"options\":[\"");
        for (const char* c = f.options; *c; c++) {
          if (*c == '|') out.print("\",\"");
          else out.print(*c);
        }
        out.print("\"]");
      }
      out.print('}');
    }
    out.print("]}");
  }
  out.print(']');
}
