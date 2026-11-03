// =====================================================================
//  config.h -- every ESP32 setting in one place.
//  The pin numbers come from FINAL_agridose_esp32.ino, whose header says
//  "all verified against your list". Do not change them without checking
//  the bench wiring.
// =====================================================================
#pragma once

// ---- test mode ----
// 1 = no Mega needed: fake_mega.cpp pretends to be the Mega inside the
//     ESP32 (WT frames, a demo dose every FAKE_AUTO_DOSE_S, answers the
//     keypad). For testing the dashboard with only the ESP32 on USB.
// 0 = the real Mega on the UART. MUST BE 0 ON THE MACHINE.
#ifndef FAKE_MEGA
#define FAKE_MEGA 0
#endif
#define FAKE_AUTO_DOSE_S 40        // fake Mega starts a demo dose this often when idle (0 = never)

// ---- Wi-Fi access point (the phones join this network) ----
#define AP_SSID        "AgriDose"
#define AP_PASSWORD    "agridose2026"       // 8 to 63 characters; after a change, run tools/make_sticker.py and reprint
#define AP_CHANNEL     6
#define AP_MAX_PHONES  5                    // the ESP32's default is only 4
#define AP_TX_POWER    WIFI_POWER_11dBm     // [30 Oct] less than the full 19.5 dBm: the phones are next to the
                                            // machine, and a cooler ESP32 with smaller current peaks drops
                                            // phones less on cheap boards. More range: WIFI_POWER_15dBm.
#define AP_IP          192, 168, 4, 1
#define AP_IP_TEXT     "192.168.4.1"
#define CAPTIVE_PORTAL 1                    // 1 = a phone opens the dashboard by itself after joining

// ---- keypad (4x4) ----
#define KEYPAD_ROW_PINS    {4, 16, 32, 5}     // R1, R2, R3, R4
#define KEYPAD_COL_PINS    {18, 19, 21, 33}   // C1, C2, C3, C4
#define KEYPAD_DEBOUNCE_MS 40

// ---- microSD card on the VSPI bus ----
#define SD_MISO_PIN 25
#define SD_SCK_PIN  14
#define SD_MOSI_PIN 23
#define SD_CS_PIN   22

// ---- UART to the Mega (baud rate is in AgriLink.h) ----
#define MEGA_SERIAL Serial2
#define MEGA_RX_PIN 27             // <- 1k/2k divider <- Mega TX2 (D16)
#define MEGA_TX_PIN 26             // -> Mega RX2 (D17)
#define MEGA_RX_BUF 1024           // bytes; holds several seconds of Mega frames

// ---- the Mega link task ----
#define LINK_TASK_CORE     1
#define LINK_TASK_PRIORITY 12      // above the web server's task (AsyncTCP uses 10)
#define LINK_TASK_STACK    6144
#define LINK_OFFLINE_MS    2000UL  // no valid frame for this long -> "Mega offline"

// ---- files on the microSD card ----
#define DOSE_LOG_PATH  "/dose_log.csv"
#define CROP_DIR       "/crop"
#define SD_WAIT_MS     200         // longest wait for the SD mutex before giving up

// ---- app logins (accounts.h): records need a login, watching the machine does not ----
#define ADMIN_USERNAME       "jomar"            // a-z 0-9 . _ - only
#define ADMIN_FIRST_PASSWORD "1234"             // used only until the admin picks a new one (the app insists).
                                                // Until then, changing these two and restarting replaces the first admin.
                                                // Forgot it? Delete /auth/admin.csv on a laptop: this one works again.
#define PASSWORD_MIN         6
#define PASSWORD_MAX         64
#define PBKDF2_ROUNDS        10000     // password hashing work; Serial prints how long one hash takes
#define SESSION_HOURS        12        // a phone stays logged in this long (a restart logs everyone out)
#define SESSIONS_MAX         10
#define LOGIN_FAILS_MAX      5         // wrong passwords in a row for one username...
#define LOGIN_LOCK_S         300       // ...lock it for this long

// ---- crop photos (the phone compresses them; the ESP32 only stores them) ----
#define PHOTOS_PER_RECORD    3
#define PHOTO_FULL_MAX       (10UL * 1024 * 1024)   // bigger photos are shrunk by the phone first
#define PHOTO_COMPRESSED_MAX (1024UL * 1024)        // the phone's copy is about 50 to 200 KB

// ---- web dashboard ----
#define WS_PERIOD_MS    250UL      // live updates at most this often...
#define WS_HEARTBEAT_MS 1000UL     // ...and at least this often
#define WS_MAX_CLIENTS  6          // extra WebSocket connections are closed (oldest first)

// ---- Serial Monitor helpers (115200 baud) ----
#define MIRROR_LCD  1              // print every menu screen as [LCD] |line 1|line 2|
#define SERIAL_KEYS 1              // typing 0-9 A-D # * in the Serial Monitor presses keys
#define DEBUG_LINK  0              // print every frame sent to the Mega
