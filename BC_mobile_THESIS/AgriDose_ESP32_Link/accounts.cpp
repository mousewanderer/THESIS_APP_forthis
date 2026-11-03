#include "accounts.h"
#include "config.h"
#include "sd_store.h"
#include "crop_records.h"
#include "crop_validate.h"
#include "json_util.h"
#include <mbedtls/pkcs5.h>
#include <esp_random.h>

#define AUTH_DIR        "/auth"
#define ADMIN_PATH      AUTH_DIR "/admin.csv"
#define ADMIN_HEADER    "username,rounds,salt,hash,must_change"
#define FARMERS_PATH    AUTH_DIR "/farmers.csv"
#define FARMERS_HEADER  "user_id,username,rounds,salt,hash"
#define USERNAME_MIN    3
#define USERNAME_MAX    20
#define SALT_BYTES      16
#define HASH_BYTES      32
#define FAILS_KEPT      8            // usernames with recent wrong passwords, remembered in RAM

static Session sessions[SESSIONS_MAX];

struct Fail {
  char     username[USERNAME_MAX + 1];
  uint8_t  count;
  uint32_t lastMs;
};
static Fail fails[FAILS_KEPT];

// ---------------------------------------------------------- hashing --
static void toHex(const uint8_t* b, size_t n, char* out) {
  static const char* const DIGITS = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = DIGITS[b[i] >> 4]; out[2 * i + 1] = DIGITS[b[i] & 15]; }
  out[2 * n] = 0;
}

static bool fromHex(const char* s, uint8_t* b, size_t n) {
  if (strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < 2 * n; i++) if (!isxdigit((unsigned char)s[i])) return false;
  for (size_t i = 0; i < n; i++) {
    char pair[3] = {s[2 * i], s[2 * i + 1], 0};
    b[i] = (uint8_t)strtoul(pair, nullptr, 16);
  }
  return true;
}

static void hashPassword(const char* password, const uint8_t* salt, uint32_t rounds, uint8_t* out) {
  mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char*)password, strlen(password),
                                salt, SALT_BYTES, rounds, HASH_BYTES, out);
}

// "rounds,salt,hash" for a new password, with a new random salt every time.
static void makeHashFields(const char* password, char* out, size_t size) {
  uint8_t salt[SALT_BYTES], hash[HASH_BYTES];
  char saltHex[2 * SALT_BYTES + 1], hashHex[2 * HASH_BYTES + 1];
  esp_fill_random(salt, sizeof(salt));
  hashPassword(password, salt, PBKDF2_ROUNDS, hash);
  toHex(salt, SALT_BYTES, saltHex);
  toHex(hash, HASH_BYTES, hashHex);
  snprintf(out, size, "%lu,%s,%s", (unsigned long)PBKDF2_ROUNDS, saltHex, hashHex);
}

// Does the password give this stored hash? Compared in constant time.
static bool passwordMatches(const char* password, const char* roundsText, const char* saltHex, const char* hashHex) {
  uint8_t salt[SALT_BYTES], want[HASH_BYTES], got[HASH_BYTES];
  unsigned long rounds = strtoul(roundsText, nullptr, 10);
  if (rounds < 1000 || rounds > 1000000 || !fromHex(saltHex, salt, SALT_BYTES) || !fromHex(hashHex, want, HASH_BYTES)) return false;
  hashPassword(password, salt, rounds, got);
  uint8_t diff = 0;
  for (size_t i = 0; i < HASH_BYTES; i++) diff |= got[i] ^ want[i];
  return diff == 0;
}

// ------------------------------------------------- names and checks --
// Usernames: 3 to 20 of a-z 0-9 . _ - ; kept in small letters, so "Rhea" logs in as "rhea".
static bool cleanUsername(const char* in, char* out, size_t size) {
  char t[USERNAME_MAX + 2];
  size_t n = cvTrim(in, t, sizeof(t));
  if (n < USERNAME_MIN || n > USERNAME_MAX || n >= size) return false;
  for (size_t i = 0; i < n; i++) {
    char c = (char)tolower((unsigned char)t[i]);
    if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    out[i] = c;
  }
  out[n] = 0;
  return true;
}

static bool passwordOk(const char* password, String& error) {
  size_t n = strlen(password);
  if (n >= PASSWORD_MIN && n <= PASSWORD_MAX) return true;
  error = "The password must have " + String(PASSWORD_MIN) + " to " + String(PASSWORD_MAX) + " characters";
  return false;
}

// One login as stored on the card.
struct Account {
  bool found;
  Role role;
  long userId;
  char username[USERNAME_MAX + 1];
  char rounds[12];
  char salt[2 * SALT_BYTES + 1];
  char hash[2 * HASH_BYTES + 1];
  bool mustChange;
};

static void copyField(char* to, size_t size, const char* from) { snprintf(to, size, "%s", from); }

// Looks a (clean) username up: the admin first, then the farmers.
static bool findAccount(const char* username, Account& a, bool& readOk) {
  memset(&a, 0, sizeof(a));
  uint32_t lineNo = 0;
  readOk = sdForEachLine(ADMIN_PATH, [&](char* line) {
    char* f[6];
    if (lineNo++ == 0 || a.found || csvSplit(line, f, 6) < 5 || strcmp(f[0], username) != 0) return;
    a.found = true; a.role = ROLE_ADMIN; a.userId = 0;
    copyField(a.username, sizeof(a.username), f[0]);
    copyField(a.rounds, sizeof(a.rounds), f[1]);
    copyField(a.salt, sizeof(a.salt), f[2]);
    copyField(a.hash, sizeof(a.hash), f[3]);
    a.mustChange = f[4][0] == '1';
  });
  if (a.found || !readOk) return a.found;
  readOk = sdForEachLine(FARMERS_PATH, [&](char* line) {
    char* f[6];
    if (a.found || !isdigit((unsigned char)line[0]) || csvSplit(line, f, 6) < 5 || strcmp(f[1], username) != 0) return;
    a.found = true; a.role = ROLE_FARMER; a.userId = strtol(f[0], nullptr, 10);
    copyField(a.username, sizeof(a.username), f[1]);
    copyField(a.rounds, sizeof(a.rounds), f[2]);
    copyField(a.salt, sizeof(a.salt), f[3]);
    copyField(a.hash, sizeof(a.hash), f[4]);
  });
  return a.found;
}

// ------------------------------------------------- wrong passwords --
static Fail* failFor(const char* username, bool make) {
  Fail* oldest = &fails[0];
  for (Fail& f : fails) {
    if (f.username[0] && strcmp(f.username, username) == 0) return &f;
    if (!f.username[0] || millis() - f.lastMs > millis() - oldest->lastMs) oldest = &f;
  }
  if (!make) return nullptr;
  memset(oldest, 0, sizeof(*oldest));
  copyField(oldest->username, sizeof(oldest->username), username);
  return oldest;
}

// ----------------------------------------------------------- start --
void accountsBegin() {
  if (!sdReady()) { Serial.println("Accounts: no SD card -- nobody can log in (watching the machine still works)"); return; }
  if (!sdMakeDir(AUTH_DIR)) { Serial.println("Accounts: could not make " AUTH_DIR); return; }
  sdRecoverEdit(ADMIN_PATH);
  sdRecoverEdit(FARMERS_PATH);
  sdCreateIfMissing(FARMERS_PATH, FARMERS_HEADER);

  // An admin who never changed the first password has lost nothing, so the
  // first admin is made again from config.h (a changed ADMIN_USERNAME or
  // ADMIN_FIRST_PASSWORD takes effect at the next start-up).
  bool haveAdmin = false, firstPassword = false;
  uint32_t lineNo = 0;
  sdForEachLine(ADMIN_PATH, [&](char* line) {
    char* f[6];
    if (lineNo++ == 0) return;
    haveAdmin = true;
    firstPassword = csvSplit(line, f, 6) >= 5 && f[4][0] == '1';
  });
  if (!haveAdmin || firstPassword) {
    sdRemove(ADMIN_PATH);                       // missing, only a header, or still the first password: start it again
    uint32_t t0 = millis();
    char fields[160], line[SD_LINE_MAX + 1];
    makeHashFields(ADMIN_FIRST_PASSWORD, fields, sizeof(fields));
    snprintf(line, sizeof(line), "%s,%s,1", ADMIN_USERNAME, fields);
    bool ok = sdAppendLine(ADMIN_PATH, line, ADMIN_HEADER);
    Serial.printf("Accounts: first admin \"%s\" %s (password from config.h; the app asks for a new one at the first login). "
                  "One password hash takes %lu ms.\n", ADMIN_USERNAME, ok ? "made" : "COULD NOT be made", (unsigned long)(millis() - t0));
  }
  int farmers = 0;
  sdForEachLine(FARMERS_PATH, [&](char* line) { if (isdigit((unsigned char)line[0])) farmers++; });
  Serial.printf("Accounts: admin + %d farmer login(s)\n", farmers);
}

// --------------------------------------------------------- sessions --
static bool expired(const Session& s) { return millis() - s.startedMs > SESSION_HOURS * 3600000UL; }

// A free slot, else the oldest session (it is logged out).
static Session* newSession() {
  Session* slot = nullptr;
  for (Session& s : sessions) {
    if (!s.sid[0] || expired(s)) { slot = &s; break; }
    if (!slot || millis() - s.startedMs > millis() - slot->startedMs) slot = &s;
  }
  memset(slot, 0, sizeof(*slot));
  uint8_t r[16];
  esp_fill_random(r, sizeof(r));
  toHex(r, sizeof(r), slot->sid);
  slot->startedMs = millis();
  return slot;
}

const Session* accountsSession(const char* sid) {
  if (sid == nullptr || strlen(sid) != 32) return nullptr;
  for (Session& s : sessions) {
    if (!s.sid[0] || strcmp(s.sid, sid) != 0) continue;
    if (expired(s)) { s.sid[0] = 0; return nullptr; }
    return &s;
  }
  return nullptr;
}

void accountsLogout(const char* sid) {
  for (Session& s : sessions) if (s.sid[0] && sid && strcmp(s.sid, sid) == 0) s.sid[0] = 0;
}

// Logs out every session of one login, except `keep`.
static void logOutAll(Role role, long userId, const char* username, const Session* keep) {
  for (Session& s : sessions) {
    if (!s.sid[0] || &s == keep) continue;
    if (role == ROLE_FARMER ? (s.role == ROLE_FARMER && s.userId == userId) : strcmp(s.username, username) == 0) s.sid[0] = 0;
  }
}

bool accountsLogin(const char* usernameIn, const char* password, const Session*& session, String& error) {
  session = nullptr;
  if (!sdReady()) { error = "SD card problem: the logins are on the SD card"; return false; }
  char username[USERNAME_MAX + 1];
  if (!cleanUsername(usernameIn, username, sizeof(username))) { error = "Wrong username or password"; return false; }

  Fail* f = failFor(username, false);
  if (f && f->count >= LOGIN_FAILS_MAX) {
    uint32_t gone = (millis() - f->lastMs) / 1000;
    if (gone < LOGIN_LOCK_S) {
      error = "Too many wrong passwords for " + String(username) + ". Try again in " + String((LOGIN_LOCK_S - gone + 59) / 60) + " min.";
      return false;
    }
    f->count = 0;                                 // the lock is over
  }

  Account a;
  bool readOk;
  bool found = findAccount(username, a, readOk);
  if (!readOk) { error = "Could not read the SD card"; return false; }
  bool ok = false;
  if (found) {
    ok = passwordMatches(password, a.rounds, a.salt, a.hash);
  } else {                                        // hash anyway, so an unknown username takes as long as a wrong password
    uint8_t salt[SALT_BYTES] = {0}, dummy[HASH_BYTES];
    hashPassword(password, salt, PBKDF2_ROUNDS, dummy);
  }
  if (!ok) {
    f = failFor(username, true);
    f->count++;
    f->lastMs = millis();
    Serial.printf("Accounts: wrong username or password for \"%s\" (%u in a row)\n", username, f->count);
    error = "Wrong username or password";
    return false;
  }
  if (f) f->count = 0;

  Session* s = newSession();
  s->role = a.role;
  s->userId = a.userId;
  copyField(s->username, sizeof(s->username), a.username);
  s->mustChange = a.mustChange;
  session = s;
  Serial.printf("Accounts: %s logged in%s\n", s->username, s->mustChange ? " (must choose a new password)" : "");
  return true;
}

bool accountsChangePassword(const Session* s, const char* oldPassword, const char* newPassword, String& error) {
  if (!passwordOk(newPassword, error)) return false;
  if (strcmp(oldPassword, newPassword) == 0) { error = "The new password must be different from the old one"; return false; }
  Account a;
  bool readOk;
  if (!findAccount(s->username, a, readOk)) { error = readOk ? "This login no longer exists" : "Could not read the SD card"; return false; }
  if (!passwordMatches(oldPassword, a.rounds, a.salt, a.hash)) { error = "The current password is wrong"; return false; }

  char fields[160], line[SD_LINE_MAX + 1];
  makeHashFields(newPassword, fields, sizeof(fields));
  SdEditResult r;
  if (a.role == ROLE_ADMIN) {
    snprintf(line, sizeof(line), "%s,%s,0", a.username, fields);
    size_t len = strlen(a.username);
    r = sdReplaceLine(ADMIN_PATH, [&](const char* old, uint32_t lineNo) {
      return lineNo > 0 && strncmp(old, a.username, len) == 0 && old[len] == ',';
    }, line);
  } else {
    snprintf(line, sizeof(line), "%ld,%s,%s", a.userId, a.username, fields);
    long id = a.userId;
    r = sdReplaceLine(FARMERS_PATH, [id](const char* old, uint32_t) {
      return isdigit((unsigned char)old[0]) && strtol(old, nullptr, 10) == id;
    }, line);
  }
  if (r != SD_EDIT_OK) { error = "Could not write to the SD card"; return false; }
  logOutAll(a.role, a.userId, a.username, s);    // other phones with the old password must log in again
  const_cast<Session*>(s)->mustChange = false;
  Serial.printf("Accounts: %s chose a new password\n", s->username);
  return true;
}

// ------------------------------------------------- farmer logins --
static bool checkNewLogin(const char* usernameIn, const char* password, long userId, char* username, String& error) {
  if (!cleanUsername(usernameIn, username, USERNAME_MAX + 1)) {
    error = "The username must be " + String(USERNAME_MIN) + " to " + String(USERNAME_MAX) + " letters, digits, '.', '_' or '-'";
    return false;
  }
  if (!passwordOk(password, error)) return false;
  Account a;
  bool readOk;
  if (findAccount(username, a, readOk) && !(a.role == ROLE_FARMER && a.userId == userId)) {
    error = "The username " + String(username) + " is taken";
    return false;
  }
  if (!readOk) { error = "Could not read the SD card"; return false; }
  return true;
}

bool accountsCheckLogin(const char* username, const char* password, long userId, String& error) {
  char clean[USERNAME_MAX + 1];
  return checkNewLogin(username, password, userId, clean, error);
}

bool accountsSetFarmerLogin(long userId, const char* usernameIn, const char* password, String& error) {
  if (!sdReady()) { error = "SD card problem"; return false; }
  bool readOk;
  if (!cropUserExists(userId, readOk)) {
    error = readOk ? "User ID " + String(userId) + " is not registered" : String("Could not read the users file");
    return false;
  }
  char username[USERNAME_MAX + 1];
  if (!checkNewLogin(usernameIn, password, userId, username, error)) return false;

  char fields[160], line[SD_LINE_MAX + 1];
  makeHashFields(password, fields, sizeof(fields));
  snprintf(line, sizeof(line), "%ld,%s,%s", userId, username, fields);
  SdEditResult r = sdReplaceLine(FARMERS_PATH, [userId](const char* old, uint32_t) {
    return isdigit((unsigned char)old[0]) && strtol(old, nullptr, 10) == userId;
  }, line);
  bool ok = r == SD_EDIT_OK || (r == SD_EDIT_NOT_FOUND && sdAppendLine(FARMERS_PATH, line, FARMERS_HEADER));
  if (!ok) { error = "Could not write to the SD card"; return false; }
  logOutAll(ROLE_FARMER, userId, username, nullptr);
  Serial.printf("Accounts: login \"%s\" set for user %ld\n", username, userId);
  return true;
}

bool accountsDeleteFarmerLogin(long userId, String& error) {
  int dropped;
  SdEditResult r = sdDropLines(FARMERS_PATH, [userId](const char* line, uint32_t) {
    return isdigit((unsigned char)line[0]) && strtol(line, nullptr, 10) == userId;
  }, dropped);
  if (r == SD_EDIT_FAILED) { error = "Could not change the logins file"; return false; }
  logOutAll(ROLE_FARMER, userId, "", nullptr);
  return true;
}

bool accountsWriteLoginsJson(Print& out, String& error) {
  bool first = true;
  out.print('[');
  bool ok = sdForEachLine(FARMERS_PATH, [&](char* line) {
    char* f[3];
    if (!isdigit((unsigned char)line[0]) || csvSplit(line, f, 3) < 2) return;
    out.printf("%s{\"user_id\":%ld,\"username\":", first ? "" : ",", strtol(f[0], nullptr, 10));
    jsonString(out, f[1]);
    out.print('}');
    first = false;
  });
  out.print(']');
  if (!ok) error = "Could not read the SD card";
  return ok;
}
