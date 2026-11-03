// =====================================================================
//  accounts -- app logins: the admin, and one login per farmer.
//
//  PRIVACY RULE (30 Oct): anyone on the AgriDose Wi-Fi may watch the
//  machine (status, hopper weights, Batch Log, Crop Guide), but a farmer's
//  records, photos and recommendation need that farmer's login, or the
//  admin's. Only the admin adds farmers, renames them and sets their logins.
//
//  Files on the microSD card (no web route ever sends them):
//    /auth/admin.csv     username,rounds,salt,hash,must_change
//    /auth/farmers.csv   user_id,username,rounds,salt,hash
//  A password is NEVER written anywhere. Only its PBKDF2-HMAC-SHA256 hash
//  is kept (PBKDF2_ROUNDS rounds, a new random 16-byte salt for every
//  password, 32-byte hash), so reading the card does not give it away.
//
//  FIRST ADMIN: if admin.csv is missing, start-up makes it from
//  ADMIN_USERNAME / ADMIN_FIRST_PASSWORD in config.h with must_change = 1,
//  and the app allows nothing but choosing a new password until that is
//  done. Admin password forgotten? Delete /auth/admin.csv on a laptop: the
//  next start-up makes the first admin again. The farmers' logins stay.
//
//  Sessions are kept in RAM only (a restart logs everybody out): a random
//  128-bit id, sent as a cookie, valid for SESSION_HOURS. LOGIN_FAILS_MAX
//  wrong passwords for one username lock it for LOGIN_LOCK_S.
// =====================================================================
#pragma once
#include <Arduino.h>

enum Role : uint8_t { ROLE_ADMIN = 1, ROLE_FARMER = 2 };

struct Session {
  char     sid[33];        // 32 hex characters; "" = a free slot
  Role     role;
  long     userId;         // the farmer's user ID (0 for the admin)
  char     username[21];
  bool     mustChange;     // still the first admin password: only a new password is allowed
  uint32_t startedMs;
};

void accountsBegin();                                   // after sdBegin(): files, and the first admin
bool accountsLogin(const char* username, const char* password, const Session*& session, String& error);
const Session* accountsSession(const char* sid);        // nullptr = no such session, or it expired
void accountsLogout(const char* sid);
bool accountsChangePassword(const Session* s, const char* oldPassword, const char* newPassword, String& error);

// Admin only (web_server.cpp checks the role). Check a farmer's new login
// before anything is saved; then set it (a new login, or a new username /
// password for one the farmer has: that farmer is logged out everywhere).
bool accountsCheckLogin(const char* username, const char* password, long userId, String& error);
bool accountsSetFarmerLogin(long userId, const char* username, const char* password, String& error);
bool accountsDeleteFarmerLogin(long userId, String& error);   // a deleted farmer: login gone, phones logged out
bool accountsWriteLoginsJson(Print& out, String& error); // [{"user_id":1,"username":"rhea"}], no hashes
