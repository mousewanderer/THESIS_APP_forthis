// =====================================================================
//  web_server -- the online mode: a look-only dashboard served by the
//  ESP32 itself as a Wi-Fi access point (no internet needed).
//
//  Pages      /  (index.html, app.js, app.css, manifest, icons; from web_assets.h)
//  Live feed  /ws  WebSocket, the live state as JSON, at most every 250 ms
//             and only on a change, plus once a second as a heartbeat
//  LOGINS (accounts.h). PUBLIC: the page, /ws, /api/status, /api/doses,
//  /api/doses.csv, /api/record-types, /api/me, /api/login. Everything else
//  needs a login [L]: a farmer reaches only their own records and photos,
//  the admin everybody's; [A] = the admin only. The session id travels in
//  the "sid" cookie, or as ?sid= (downloads the Android app makes itself).
//
//  API        GET  /api/status                  live state (same JSON as /ws)
//             GET  /api/doses?limit=N           newest dose rows, newest first
//             GET  /api/doses.csv               the whole dose log as a download
//             GET  /api/record-types            crop record fields (to build the forms)
//             GET  /api/me                      who is logged in on this phone
//             POST /api/login                   username, password -> sets the cookie
//             POST /api/logout
//             POST /api/account/password    [L] old, new (also the first admin password)
//             GET  /api/logins              [A] which farmers have a login (usernames, no hashes)
//             POST /api/farmer-login        [A] user_id, username, password: set a farmer's login
//             GET  /api/users               [L] farmers (a farmer: only themself)
//             POST /api/users               [A] add a farmer (user_id, name [, username, password])
//             GET  /api/users.csv           [A] the farmers file, as a download
//             GET  /api/records/<type>?user_id=N&limit=N   [L] type = soil | planting | growth | fertilizer
//             GET  /api/records/<type>.csv  [L] as a download: the admin the whole file, a farmer their rows
//             POST /api/records/<type>      [L] add a record for a user ID
//             POST /api/edit/<type>         [L] correct a record (row and rev from the GET, user_id, every field)
//             POST /api/edit/user           [A] correct a farmer's name (user_id, name)
//             POST /api/delete/user         [A] user_id: the farmer, their login, records and photos
//                                               (the dose log is the machine's and stays)
//                                               (not /api/records/<type>/edit: a route also answers every
//                                               address below it, so the add route would take it)
//             POST /api/photos?user_id=N    [L] one photo: compressed.jpg + full.jpg/png/webp
//                                               (the phone makes the compressed copy)
//             GET  /api/photo?id=N[&size=full][&download=1]   [L] the compressed copy, or the save file
//
//  LOOK-ONLY RULE: web_server.cpp never includes mega_link.h or AgriLink.h.
//  It can read the live state and the SD card, and add or correct farmers
//  and crop records and add photos on the card, nothing else. No request can send DOSE,
//  TARE or STOP; messages that phones send on /ws are ignored.
// =====================================================================
#pragma once

void webBegin();   // starts the access point, the DNS for the captive portal, and the server
void webLoop();    // call from loop(): pushes live updates, tidies WebSocket clients
