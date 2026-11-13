#include "web_server.h"
#include "config.h"
#include "live_state.h"
#include "sd_store.h"
#include "dose_log.h"
#include "crop_records.h"
#include "crop_photos.h"
#include "accounts.h"
#include "wall_clock.h"
#include "json_util.h"
#include "web_assets.h"
#include "stock_check.h"      // [10 Nov] the biggest dose each recipe's stock covers
#include <new>
#include <WiFi.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

static_assert(sizeof(AP_PASSWORD) - 1 >= 8, "AP_PASSWORD in config.h needs at least 8 characters");

#define STATUS_JSON_MAX 640   // [10 Nov] 512 -> 640 for stock_max_g / stock_short
#define DOSES_LIMIT_MAX 200
#define SD_PIECE_MAX    4096    // bytes per SD read when sending a file (one mutex turn each)

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
#if CAPTIVE_PORTAL
static DNSServer dns;
#endif

// ------------------------------------------------------- live status --
// The same JSON goes to /api/status and to every WebSocket client.
static size_t buildStatusJson(const LiveState& s, char* out, size_t size) {
  bool online = liveMegaOnline(s);
  unsigned long age = s.everHeard ? millis() - s.lastFrameMs : 0;
  int n = snprintf(out, size,
      "{\"online\":%s,\"weight_dg\":%ld,\"flag\":\"%c\",\"last_frame_ms\":%lu,"
      "\"done_count\":%lu,\"dose_count\":%lu,\"sd_ok\":%s,\"fake_mega\":%s,"
      "\"boot\":%lu,\"uptime_s\":%lu,\"heap_free\":%lu,\"heap_min\":%lu,\"clients\":%u",
      online ? "true" : "false", (long)s.weightDg, s.flag, age,
      (unsigned long)s.doneCount, (unsigned long)doseLogCount(), sdReady() ? "true" : "false",
      FAKE_MEGA ? "true" : "false", (unsigned long)doseLogBoot(), (unsigned long)(millis() / 1000),
      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(), (unsigned)ws.count());
  if (s.lastDone.valid) {
    n += snprintf(out + n, size - n, ",\"last\":{\"seq\":%u,\"recipe\":\"%c\",\"target_dg\":%ld,\"actual_dg\":%ld}",
                  s.lastDone.seq, s.lastDone.recipe, (long)s.lastDone.targetDg, (long)s.lastDone.actualDg);
  }
  if (s.haveHoppers) {
    n += snprintf(out + n, size - n, ",\"hoppers_dg\":[%ld,%ld,%ld]",
                  (long)s.hopperDg[0], (long)s.hopperDg[1], (long)s.hopperDg[2]);
    // [10 Nov] For each recipe A, B, C: the biggest dose the stock covers (0 = not
    // even the smallest), and which hoppers stop it going higher (bit 0 = A).
    // The same rule as the keypad's LOW <hopper> screen (stock_check.h).
    static const int32_t SPARE_G[3] = STOCK_SPARE_G;
    int32_t maxG[3];
    uint8_t shortMask[3];
    for (uint8_t i = 0; i < 3; i++) maxG[i] = stockMaxDoseG(AGRI_RECIPES[i], s.hopperDg, SPARE_G, shortMask[i]);
    n += snprintf(out + n, size - n, ",\"stock_max_g\":[%ld,%ld,%ld],\"stock_short\":[%u,%u,%u]",
                  (long)maxG[0], (long)maxG[1], (long)maxG[2], shortMask[0], shortMask[1], shortMask[2]);
  }
  // The ESP32's time (wall_clock.h): epoch = UTC seconds, 0 while not set.
  // The page sends the phone's time when clock_set is false.
  bool clockOk = clockIsSet();
  n += snprintf(out + n, size - n, ",\"clock_set\":%s,\"epoch\":%lld}",
                clockOk ? "true" : "false", (long long)clockNow());
  return n;
}

// ---------------------------------------------------------- helpers --
static void sendJsonError(AsyncWebServerRequest* r, int code, const String& message) {
  AsyncResponseStream* res = r->beginResponseStream("application/json");
  res->setCode(code);
  res->print("{\"ok\":false,\"error\":");
  jsonString(*res, message.c_str());
  res->print('}');
  r->send(res);
}

static long queryLong(AsyncWebServerRequest* r, const char* name, long fallback) {
  const AsyncWebParameter* p = r->getParam(name);
  return p ? p->value().toInt() : fallback;
}

// Reads one field from a submitted form ("" if it is missing).
static FieldGetter formFields(AsyncWebServerRequest* r) {
  return [r](const char* name) -> String {
    const AsyncWebParameter* p = r->getParam(name, true);   // true = from the form body
    return p ? p->value() : String();
  };
}

// ------------------------------------------------------------ logins --
// The session id: the "sid" cookie, or ?sid= in the address (the Android
// app downloads CSV files and photos itself, without the page's cookie).
static String sessionId(AsyncWebServerRequest* r) {
  const AsyncWebParameter* p = r->getParam("sid");
  if (p) return p->value();
  const String& cookies = r->header("Cookie");
  int at = cookies.indexOf("sid=");
  while (at > 0 && cookies[at - 1] != ' ' && cookies[at - 1] != ';') at = cookies.indexOf("sid=", at + 1);
  if (at < 0) return String();
  int end = cookies.indexOf(';', at);
  return cookies.substring(at + 4, end < 0 ? cookies.length() : end);
}

static const Session* whoIs(AsyncWebServerRequest* r) { return accountsSession(sessionId(r).c_str()); }

// The session, if this request may go on. If not, answers 401 / 403 itself
// and returns nullptr.
static const Session* need(AsyncWebServerRequest* r, bool adminOnly = false) {
  const Session* s = whoIs(r);
  if (!s) { sendJsonError(r, 401, "Log in first"); return nullptr; }
  if (s->mustChange) { sendJsonError(r, 403, "Choose a new admin password first"); return nullptr; }
  if (adminOnly && s->role != ROLE_ADMIN) { sendJsonError(r, 403, "Only the admin can do this"); return nullptr; }
  return s;
}

// A farmer reaches only their own records; the admin reaches everybody's.
static bool mayUse(AsyncWebServerRequest* r, const Session* s, long userId) {
  if (s->role == ROLE_ADMIN || s->userId == userId) return true;
  sendJsonError(r, 403, "Those are another farmer's records");
  return false;
}

// Who is logged in on this phone. sid goes to the page for download links;
// usernames are only a-z 0-9 . _ - (accounts.cpp), safe in JSON as they are.
static void sendMe(AsyncWebServerRequest* r, const Session* s, const String& setCookie = String()) {
  AsyncResponseStream* res = r->beginResponseStream("application/json");
  res->addHeader("Cache-Control", "no-store");
  if (setCookie.length()) res->addHeader("Set-Cookie", setCookie);
  if (!s) res->print("{\"logged_in\":false}");
  else res->printf("{\"logged_in\":true,\"username\":\"%s\",\"role\":\"%s\",\"user_id\":%ld,\"must_change\":%s,\"sid\":\"%s\"}",
                   s->username, s->role == ROLE_ADMIN ? "admin" : "farmer", s->userId, s->mustChange ? "true" : "false", s->sid);
  r->send(res);
}

static void sendOk(AsyncWebServerRequest* r) { r->send(200, "application/json", "{\"ok\":true}"); }

static void serveAsset(AsyncWebServerRequest* r, const WebAsset& a) {
  AsyncWebServerResponse* res = r->beginResponse(200, a.type, a.data, a.len);
  if (a.gzipped) res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", a.gzipped ? "no-cache" : "max-age=86400");
  r->send(res);
}

// ------------------------------------------------------------- API --
static void handleDoses(AsyncWebServerRequest* r) {
  long limit = constrain(queryLong(r, "limit", 20), 1L, (long)DOSES_LIMIT_MAX);
  DoseRow* rows = new (std::nothrow) DoseRow[limit];
  if (rows == nullptr) { sendJsonError(r, 503, "The ESP32 is out of memory, try again"); return; }
  int n = doseLogNewest(rows, (int)limit);
  if (n < 0) { delete[] rows; sendJsonError(r, 503, "Could not read the SD card"); return; }

  AsyncResponseStream* res = r->beginResponseStream("application/json");
  res->printf("{\"total\":%lu,\"items\":[", (unsigned long)doseLogCount());
  for (int i = 0; i < n; i++) {
    const DoseRow& d = rows[i];
    // time: "2026-11-01 14:05", or "" for a dose made while the ESP32 had no time
    res->printf("%s{\"no\":%lu,\"boot\":%lu,\"uptime_s\":%lu,\"seq\":%u,\"recipe\":\"%c\",\"target_dg\":%ld,\"actual_dg\":%ld,\"time\":\"%s\"}",
                i ? "," : "", (unsigned long)d.no, (unsigned long)d.boot, (unsigned long)d.uptimeS,
                d.seq, d.recipe, (long)d.targetDg, (long)d.actualDg, d.time);
  }
  res->print("]}");
  delete[] rows;
  r->send(res);
}

// Sends a file from the SD card. The size is taken once, at the start
// (so a log that grows meanwhile never arrives cut in the middle of a row).
// Each piece is read with its own short turn on the SD mutex.
static AsyncWebServerResponse* sdFileResponse(AsyncWebServerRequest* r, const char* path, int32_t size, const char* type) {
  String file(path);
  return r->beginResponse(type, (size_t)size,
      [file, size](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
        if (index >= (size_t)size) return 0;
        size_t want = (size_t)size - index;
        if (want > maxLen) want = maxLen;
        if (want > SD_PIECE_MAX) want = SD_PIECE_MAX;
        int n = sdReadAt(file.c_str(), index, buf, want);
        if (n < 0) return sdReady() ? RESPONSE_TRY_AGAIN : 0;   // busy: the server asks again shortly
        return (size_t)n;
      });
}

// A whole CSV file from the card as a download (the dose log, crop records).
static void sendCsvFile(AsyncWebServerRequest* r, const char* path, const String& downloadName) {
  int32_t size = sdFileSize(path);
  if (size < 0) { sendJsonError(r, 503, "Could not read the SD card"); return; }
  AsyncWebServerResponse* res = sdFileResponse(r, path, size, "text/csv");
  res->addHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  r->send(res);
}

static void handleDosesCsv(AsyncWebServerRequest* r) { sendCsvFile(r, DOSE_LOG_PATH, "agridose_dose_log.csv"); }

// POST /api/clock  epoch=<UTC seconds>&tz_min=<minutes east of UTC>
// The page sends its phone's time; clock_rules.h decides whether to take it
// (any phone while the ESP32 has no time, else only a logged-in one).
// Answers 200 either way, with set = whether the time was taken.
static bool wholeNumber(const String& v, long long& out) {
  const char* s = v.c_str();
  char* end;
  if (*s == 0) return false;
  out = strtoll(s, &end, 10);
  return *end == 0;
}

static void handleClock(AsyncWebServerRequest* r) {
  const AsyncWebParameter* e = r->getParam("epoch", true);
  const AsyncWebParameter* z = r->getParam("tz_min", true);
  long long epoch = 0, tz = 0;
  if (!e || !z || !wholeNumber(e->value(), epoch) || !wholeNumber(z->value(), tz)) { sendJsonError(r, 400, "Send epoch and tz_min"); return; }
  const Session* s = whoIs(r);
  bool loggedIn = s && !s->mustChange;
  ClockVerdict v = clockOffer((int64_t)epoch, (long)tz, loggedIn);
  if (v == CLOCK_BAD) { sendJsonError(r, 400, "That time or time zone can't be right"); return; }
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"set\":%s,\"epoch\":%lld}",
           (v == CLOCK_SET || v == CLOCK_CORRECTED) ? "true" : "false", (long long)clockNow());
  r->send(200, "application/json", buf);
}

// GET /api/photo?id=N            the compressed copy (what the dashboard shows)
//     &size=full                 the photo as it was taken (the save file)
//     &download=1                as a file to save on the phone
static void handlePhoto(AsyncWebServerRequest* r) {
  const Session* s = need(r);
  if (!s) return;
  long id = queryLong(r, "id", 0);
  if (s->role != ROLE_ADMIN && photoOwner(id) != s->userId) { sendJsonError(r, 403, "That is another farmer's photo"); return; }
  const AsyncWebParameter* sizeParam = r->getParam("size");
  bool full = sizeParam && sizeParam->value() == "full";
  char path[48];
  const char* type;
  int32_t bytes;
  if (!photoFile(id, full, path, sizeof(path), type, bytes)) { sendJsonError(r, 404, "Photo not found"); return; }
  AsyncWebServerResponse* res = sdFileResponse(r, path, bytes, type);
  res->addHeader("Cache-Control", "private, max-age=31536000, immutable");   // a photo number never changes its file
  res->addHeader("X-Content-Type-Options", "nosniff");
  if (r->hasParam("download")) {
    const char* name = strrchr(path, '/') + 1;                         // "0012.jpg"
    res->addHeader("Content-Disposition", String("attachment; filename=\"agridose_photo_") + name + "\"");
  }
  r->send(res);
}

// POST /api/photos?user_id=N, a form with two files: compressed.jpg, then
// full.jpg (or .png / .webp). crop_photos.cpp writes them as they arrive.
// The farmer an upload is for, if this login may add photos for them;
// else 0, which crop_photos.cpp refuses before it writes anything.
static long uploadUser(AsyncWebServerRequest* r) {
  const Session* s = whoIs(r);
  long userId = queryLong(r, "user_id", 0);
  return (s && !s->mustChange && (s->role == ROLE_ADMIN || s->userId == userId)) ? userId : 0;
}

static void handlePhotoPiece(AsyncWebServerRequest* r, const String& filename, size_t index,
                             uint8_t* data, size_t len, bool final) {
  if (index == 0) r->onDisconnect([r]() { photoUploadGone(r); });
  photoUploadPiece(r, uploadUser(r), filename.c_str(), index, data, len, final);
}

static void handlePhotoDone(AsyncWebServerRequest* r) {
  long id;
  String error;
  bool ok = photoUploadFinish(r, id, error);       // always, so the next upload can start
  if (!uploadUser(r)) { if (need(r)) sendJsonError(r, 403, "Those are another farmer's records"); return; }
  if (!ok) { sendJsonError(r, 400, error); return; }
  r->send(200, "application/json", "{\"ok\":true,\"id\":" + String(id) + "}");
}

static void addRecordRoutes(RecordType t) {
  String path = String("/api/records/") + cropTypeName(t);

  // e.g. /api/records/soil.csv: the admin gets the whole file (every
  // farmer), a farmer only their own rows.
  server.on((path + ".csv").c_str(), HTTP_GET, [t](AsyncWebServerRequest* r) {
    const Session* s = need(r);
    if (!s) return;
    String name = String("agridose_") + cropTypeName(t) + ".csv";
    if (s->role == ROLE_ADMIN) { sendCsvFile(r, cropTypePath(t), name); return; }
    AsyncResponseStream* res = r->beginResponseStream("text/csv");
    res->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    String error;
    if (!cropWriteRecordsCsv(t, s->userId, *res, error)) { delete res; sendJsonError(r, 503, error); return; }
    r->send(res);
  });

  server.on(path.c_str(), HTTP_GET, [t](AsyncWebServerRequest* r) {
    const Session* s = need(r);
    if (!s) return;
    long userId = queryLong(r, "user_id", 0);
    if (userId <= 0) { sendJsonError(r, 400, "user_id is missing"); return; }
    if (!mayUse(r, s, userId)) return;
    AsyncResponseStream* res = r->beginResponseStream("application/json");
    String error;
    if (!cropWriteRecordsJson(t, userId, (int)queryLong(r, "limit", 3), *res, error)) {
      delete res;
      sendJsonError(r, 503, error);
      return;
    }
    r->send(res);
  });

  server.on(path.c_str(), HTTP_POST, [t](AsyncWebServerRequest* r) {
    const Session* s = need(r);
    FieldGetter get = formFields(r);
    if (!s || !mayUse(r, s, get("user_id").toInt())) return;
    String error;
    if (!cropAddRecord(t, get, error)) { sendJsonError(r, 400, error); return; }
    sendOk(r);
  });

  server.on((String("/api/edit/") + cropTypeName(t)).c_str(), HTTP_POST, [t](AsyncWebServerRequest* r) {
    const Session* s = need(r);
    FieldGetter get = formFields(r);
    if (!s || !mayUse(r, s, get("user_id").toInt())) return;
    String error;
    if (!cropEditRecord(t, get, error)) { sendJsonError(r, 400, error); return; }
    sendOk(r);
  });

  // 1 Nov 2026: delete one record (and its photos). Same access as an edit:
  // a farmer only their own, the admin anybody's. Not under /api/records/:
  // that route would answer this address too (the routing gotcha).
  server.on((String("/api/delete/") + cropTypeName(t)).c_str(), HTTP_POST, [t](AsyncWebServerRequest* r) {
    const Session* s = need(r);
    FieldGetter get = formFields(r);
    if (!s || !mayUse(r, s, get("user_id").toInt())) return;
    String error;
    if (!cropDeleteRecord(t, get, error)) { sendJsonError(r, 400, error); return; }
    sendOk(r);
  });
}

// --------------------------------------------------------- WebSocket --
static void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType type, void*, uint8_t*, size_t) {
  if (type == WS_EVT_CONNECT) {
    char buf[STATUS_JSON_MAX];
    buildStatusJson(liveGet(), buf, sizeof(buf));
    c->text(buf);                    // a new phone gets the current state at once
    Serial.printf("Dashboard: phone connected (%u open)\n", (unsigned)s->count());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.println("Dashboard: phone disconnected");
  }
  // WS_EVT_DATA: anything a phone sends here is ignored on purpose.
}

// What counts as "changed" for the live feed (timers and heap numbers do not).
// The hopper weights only change when the Mega sends HW, which it does only
// for a move over 5 g, so load-cell noise never triggers a push.
struct LiveKey {
  bool online, sdOk;
  int32_t weightDg;
  char flag;
  uint32_t doneCount, doseCount;
  int32_t hopperDg[3];
  bool clockSet;                       // a phone just set the time: tell every phone at once
  bool operator!=(const LiveKey& o) const {
    return online != o.online || sdOk != o.sdOk || weightDg != o.weightDg || flag != o.flag ||
           doneCount != o.doneCount || doseCount != o.doseCount || memcmp(hopperDg, o.hopperDg, sizeof(hopperDg)) != 0 ||
           clockSet != o.clockSet;
  }
};

void webLoop() {
  static uint32_t lastCheckAt = 0, lastSentAt = 0, lastCleanupAt = 0;
  static LiveKey sent = {};
  uint32_t now = millis();

  if (now - lastCleanupAt >= 1000) {
    lastCleanupAt = now;
    ws.cleanupClients(WS_MAX_CLIENTS);   // drops dead connections and any over the limit
  }
  if (now - lastCheckAt < WS_PERIOD_MS) return;
  lastCheckAt = now;
  if (ws.count() == 0) return;

  LiveState s = liveGet();
  LiveKey key = {liveMegaOnline(s), sdReady(), s.weightDg, s.flag, s.doneCount, doseLogCount(),
                 {s.hopperDg[0], s.hopperDg[1], s.hopperDg[2]}, clockIsSet()};
  if (!(key != sent) && now - lastSentAt < WS_HEARTBEAT_MS) return;   // no change, heartbeat not due

  char buf[STATUS_JSON_MAX];
  buildStatusJson(s, buf, sizeof(buf));
  ws.textAll(buf);
  sent = key;
  lastSentAt = now;
}

// ----------------------------------------------------------- start --
void webBegin() {
  IPAddress ip(AP_IP);
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  // Every phone that joins or leaves the Wi-Fi, with the reason, so drop-outs
  // can be told apart: the phone leaving by itself (Android: "no internet")
  // or the radio losing it (weak signal, power).
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
      const uint8_t* m = info.wifi_ap_staconnected.mac;
      Serial.printf("Wi-Fi: phone ..%02X:%02X:%02X joined (%u on the Wi-Fi)\n", m[3], m[4], m[5], (unsigned)WiFi.softAPgetStationNum());
    } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
      const uint8_t* m = info.wifi_ap_stadisconnected.mac;
      uint16_t why = info.wifi_ap_stadisconnected.reason;
      const char* text = (why == 3 || why == 8) ? "the phone left by itself (Android: no internet / another network?)"
                       : (why == 2 || why == 4) ? "idle too long"
                       : (why == 15 || why == 16 || why == 34 || why >= 200) ? "radio: weak signal or no answer from the phone"
                       : "see the ESP-IDF wifi_err_reason_t list";
      Serial.printf("Wi-Fi: phone ..%02X:%02X:%02X left, reason %u: %s\n", m[3], m[4], m[5], why, text);
    }
  });

  if (WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_PHONES)) {
    WiFi.setTxPower(AP_TX_POWER);
    Serial.printf("Wi-Fi access point \"%s\" is on (transmit power %.1f dBm). Dashboard: http://%s/\n",
                  AP_SSID, WiFi.getTxPower() / 4.0f, AP_IP_TEXT);
  } else {
    Serial.println("Wi-Fi access point FAILED to start");
  }

#if CAPTIVE_PORTAL
  dns.start(53, "*", ip);            // every name a phone looks up points to the ESP32
#endif

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // Page files, from web_assets.h (made by tools/make_web_assets.py)
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    const WebAsset* a = &WEB_ASSETS[i];
    server.on(a->path, HTTP_GET, [a](AsyncWebServerRequest* r) { serveAsset(r, *a); });
  }

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    char buf[STATUS_JSON_MAX];
    buildStatusJson(liveGet(), buf, sizeof(buf));
    r->send(200, "application/json", buf);
  });
  server.on("/api/doses", HTTP_GET, handleDoses);
  server.on("/api/doses.csv", HTTP_GET, handleDosesCsv);
  server.on("/api/clock", HTTP_POST, handleClock);

  server.on("/api/record-types", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncResponseStream* res = r->beginResponseStream("application/json");
    cropWriteTypesJson(*res);
    r->send(res);
  });
  // ---- logins (accounts.h) ----
  server.on("/api/me", HTTP_GET, [](AsyncWebServerRequest* r) { sendMe(r, whoIs(r)); });
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* r) {
    FieldGetter get = formFields(r);
    const Session* s;
    String error;
    if (!accountsLogin(get("username").c_str(), get("password").c_str(), s, error)) { sendJsonError(r, 401, error); return; }
    sendMe(r, s, String("sid=") + s->sid + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + String(SESSION_HOURS * 3600UL));
  });
  server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest* r) {
    accountsLogout(sessionId(r).c_str());
    sendMe(r, nullptr, "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  });
  server.on("/api/account/password", HTTP_POST, [](AsyncWebServerRequest* r) {
    const Session* s = whoIs(r);                  // also with the first admin password: this is how it is changed
    if (!s) { sendJsonError(r, 401, "Log in first"); return; }
    FieldGetter get = formFields(r);
    String error;
    if (!accountsChangePassword(s, get("old").c_str(), get("new").c_str(), error)) { sendJsonError(r, 400, error); return; }
    sendMe(r, s);
  });
  server.on("/api/logins", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!need(r, true)) return;
    AsyncResponseStream* res = r->beginResponseStream("application/json");
    String error;
    if (!accountsWriteLoginsJson(*res, error)) { delete res; sendJsonError(r, 503, error); return; }
    r->send(res);
  });
  server.on("/api/farmer-login", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!need(r, true)) return;
    FieldGetter get = formFields(r);
    String error;
    if (!accountsSetFarmerLogin(get("user_id").toInt(), get("username").c_str(), get("password").c_str(), error)) {
      sendJsonError(r, 400, error);
      return;
    }
    sendOk(r);
  });

  // ---- farmers: a farmer sees only themself; only the admin changes the list ----
  server.on("/api/users", HTTP_GET, [](AsyncWebServerRequest* r) {
    const Session* s = need(r);
    if (!s) return;
    AsyncResponseStream* res = r->beginResponseStream("application/json");
    String error;
    if (!cropWriteUsersJson(*res, error, s->role == ROLE_ADMIN ? 0 : s->userId)) { delete res; sendJsonError(r, 503, error); return; }
    r->send(res);
  });
  server.on("/api/users.csv", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (need(r, true)) sendCsvFile(r, cropUsersPath(), "agridose_farmers.csv");
  });
  // A new farmer, with their login if a username is given (checked first, so
  // a bad login does not leave a farmer half made).
  server.on("/api/users", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!need(r, true)) return;
    FieldGetter get = formFields(r);
    String error, username = get("username"), password = get("password");
    long id = get("user_id").toInt();
    if (username.length() && !accountsCheckLogin(username.c_str(), password.c_str(), id, error)) { sendJsonError(r, 400, error); return; }
    if (!cropAddUser(get, error)) { sendJsonError(r, 400, error); return; }
    if (username.length() && !accountsSetFarmerLogin(id, username.c_str(), password.c_str(), error)) {
      sendJsonError(r, 500, "The farmer was saved, but not the login: " + error);
      return;
    }
    sendOk(r);
  });
  server.on("/api/edit/user", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!need(r, true)) return;
    String error;
    if (!cropEditUser(formFields(r), error)) { sendJsonError(r, 400, error); return; }
    sendOk(r);
  });
  // A farmer and everything that is theirs. The login goes first, so a
  // failure part-way never leaves a login for a farmer that is gone.
  server.on("/api/delete/user", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!need(r, true)) return;
    long id = formFields(r)("user_id").toInt();
    String error;
    bool readOk;
    if (!cropUserExists(id, readOk)) { sendJsonError(r, 400, readOk ? "User ID " + String(id) + " is not registered" : String("Could not read the users file")); return; }
    if (!accountsDeleteFarmerLogin(id, error) || !cropDeleteUser(id, error)) { sendJsonError(r, 500, error); return; }
    sendOk(r);
  });
  for (uint8_t t = 0; t < REC_TYPE_COUNT; t++) addRecordRoutes((RecordType)t);
  server.on("/api/photos", HTTP_POST, handlePhotoDone, handlePhotoPiece);
  server.on("/api/photo", HTTP_GET, handlePhoto);

#if CAPTIVE_PORTAL
  // Phones open these addresses to test for internet. Sending them to the
  // dashboard makes the phone show it right after joining the Wi-Fi.
  static const char* const PROBES[] = {"/generate_204", "/gen_204", "/hotspot-detect.html",
                                       "/library/test/success.html", "/connecttest.txt", "/ncsi.txt",
                                       "/canonical.html", "/success.txt"};
  for (const char* p : PROBES) {
    server.on(p, HTTP_ANY, [](AsyncWebServerRequest* r) { r->redirect("http://" AP_IP_TEXT "/"); });
  }
#endif
  server.onNotFound([](AsyncWebServerRequest* r) {
#if CAPTIVE_PORTAL
    if (r->host() != AP_IP_TEXT) { r->redirect("http://" AP_IP_TEXT "/"); return; }   // another website name
#endif
    sendJsonError(r, 404, "Not found");
  });

  server.begin();
}
