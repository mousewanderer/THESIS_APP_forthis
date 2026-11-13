# AgriDose: link firmware and web dashboard

Written 28 Oct 2026. Everything here **compiles and passes its laptop tests,
but none of it has run on the real boards yet.** See "What still needs testing".

```
AgriDose_ESP32_Link/     ESP32: keypad, microSD, link to the Mega, web dashboard
  AgriDose_ESP32_Link.ino  keypad menus (the only place a dose is started)
  config.h                 every setting and pin (AP name/password, FAKE_MEGA, ...)
  AgriLink.h               the UART protocol (identical copy in AgriDose_Mega_Link/)
  mega_link.*              the only code that talks to the Mega (its own FreeRTOS task)
  live_state.*             latest weight / flag / last dose, shared between tasks
  sd_store.*               the microSD card and its one mutex
  dose_log.*               /dose_log.csv
  crop_records.*           farmers and crop records; crop_validate.h checks the input
  crop_photos.*            crop photos on the SD card (the original and a compressed copy)
  web_server.*             the dashboard (look-only); web_assets.h = the page, generated
  fake_mega.*              pretend Mega for testing (FAKE_MEGA 1)
  www/                     the page source: index.html, app.js, agri.js, app.css, manifest, icons
  tools/                   make_web_assets.py, make_icons.py, make_sticker.py, mock_esp32.py
AgriDose_Mega_Link/      Mega: load cells, augers, LCD (dosing engine from FINAL_agridose_mega)
AgriDose_Android/        Android app for the dashboard (git repo: github.com/mousewanderer/THESIS_APP_forthis)
tests/                   laptop tests: AgriLink.h, crop_validate.h, the recommendation (agri.js)
qr_sticker.html          printable QR sticker (regenerate after changing the password)
```

The old sketches in Downloads (`FINAL_agridose_*`) were not changed.

## 1. Flash the boards

**Libraries** (Library Manager). Already installed on this laptop:
- Async TCP **by ESP32Async** 3.5.0, and ESP Async WebServer **by ESP32Async** 3.12.1.
  Pick the ESP32Async ones; the old me-no-dev copies have the same names.
- Keypad, HX711 Arduino Library, LiquidCrystal_I2C (already used before).
- Blynk is no longer used.

**Before flashing the ESP32**, check `config.h`:
- The Wi-Fi is `AgriDose`, password `agridose2026`. If you change it, regenerate
  and reprint the sticker (section 2).
- `FAKE_MEGA` must be `0` on the machine.

**Mega:** open `AgriDose_Mega_Link`, board *Arduino Mega or Mega 2560*, upload.
The hoppers must be empty at the first power-on on a blank EEPROM (it zeroes them
and saves the zeros). After that they may stay loaded: see section 6, "Saved state".

**ESP32:** open `AgriDose_ESP32_Link`, board *ESP32 Dev Module*, default partition
scheme, upload. It uses 87 % of the app space (29 Oct, with the photo and
Recommend changes). If later changes push it past about 95 %, choose
*Huge APP (3MB No OTA/1MB SPIFFS)* (nothing uses SPIFFS).

Both Serial Monitors run at 115200. The ESP32 prints `Mega link: online`, the
Mega prints `ESP32 link OK`.

**Test without the Mega:** set `FAKE_MEGA 1` in `config.h` and upload to a bare
ESP32 on USB. A pretend Mega runs inside it: a demo dose every 40 s, every fourth
one ends in a fault. The keypad flow works from the Serial Monitor too:
`#A#600#` then `#`; `DB#` drains hopper B, `*` stops it. The dashboard shows a
yellow TEST MODE banner.

## 2. Connect a phone

1. Print `qr_sticker.html` at 100 % and stick it on the machine.
   After changing the password: `pip install segno`, then
   `python AgriDose_ESP32_Link/tools/make_sticker.py`. Scan both codes with a
   phone before printing.
2. Code 1 joins the Wi-Fi `AgriDose`. The dashboard usually opens by itself
   (captive portal). If Android says "no internet", tap **Stay connected**.
3. Code 2 opens `http://192.168.4.1/` in Chrome. Menu > **Add to Home screen**.
   Without HTTPS Chrome makes a shortcut (it opens in a Chrome tab), not an
   installed app. It still works with no internet: the ESP32 serves every file.

Up to 5 phones can join (`AP_MAX_PHONES`).

**Android app (optional, recommended):** `AgriDose_Android/` is a small app that
opens the same dashboard full-screen with its own icon. It also works with mobile
data switched on, which Chrome often does not (Android sends Chrome's traffic over
mobile data when the Wi-Fi has no internet). The APK is built by GitHub Actions;
see `AgriDose_Android/README.md`.

## 3. What each page shows

The page looks like the team's Blynk-style phone app (agridose-game, docs/05):
dark screen, lime accent, five tabs at the bottom. The header shows "Solid
Amendment Project", a lime dot when the Mega is online, a bell with a red dot
when there is an alert (fault, Mega offline, SD card problem, a hopper under
200 g), and the farmer
("Rosa · ID 2", or "Log in"); tap it to log in, or (admin) to change or add a farmer. Two switches, remembered
on each phone:
- **Sun** button: a bright, high-contrast look (black on white, dark green
  accent, outlined buttons) for reading outdoors.
- **Bisaya** button: Bisaya (Cebuano) lines under the English labels, tabs and
  buttons. On by default. The wording is a first draft and must be checked by
  the team, like the sticker (`BIS` in `www/app.js`).

**Logins (30 Oct).** Watching the machine needs no login: the Machine tab,
Batch Log and Crop Guide are open to anyone on the AgriDose Wi-Fi. Records,
photos and Recommend need a login. A farmer logs in with their own username and
password and sees only their own records; the admin sees every farmer, adds
farmers (with their login), renames them and sets or resets their logins, and
can **delete a farmer**: two taps (the first only shows what will be lost), then
the farmer, their login, crop records and photos are removed from the SD card.
That can't be undone; the Batch Log (the machine's dose history) stays. Tap
the name in the header to log in, change your password or log out. The first
admin login is `jomar` / `1234` (`config.h`); the app then allows
nothing but choosing a new admin password. See section 5 for how passwords are
stored and how to reset a forgotten admin password.

- **Machine**: the three hopper weights (gauges 0 to 10 kg, as on the team's
  Blynk screen) and their total, an arc gauge with the weight dispensed in the
  current dose (0 to 900 g), the machine state (Idle / Dosing / Fault / Offline), and the last dose:
  recipe, target, actual, error in g and %. **Enough stock for** (10 Nov): for each
  recipe, the biggest dose the hoppers can give, and which hopper to refill when it
  is short (`stock_max_g` / `stock_short` in the status, the keypad's rule below). Updates live over a WebSocket and
  reconnects by itself after the phone sleeps or the Wi-Fi drops. The bottom line
  shows the ESP32's free memory, lowest free memory since boot, and how many
  phones are on the live feed. Use it to measure memory with 3 to 5 phones.
- **Records**: the farmer's soil tests, seed type and planting, growth records
  and fertilizer applications, newest date first, 3 per group ("Showing 3 of N",
  **Show all**).
  **View details** opens a sheet with every field and the photos. **+ Add** opens
  a form: number boxes have - and + buttons like the Blynk step widget (on an
  empty box the first tap fills in the last saved value, shown greyed), Seed type
  and Growth stage are drop-downs (with "Other" to type a new one), and a growth
  record is picked by planting, which fills in the plot and the date planted and
  counts the growth days. The ESP32 still checks every value before saving and
  the form shows its reason if it refuses. At the bottom, **Download records
  (CSV)** gives each whole file from the SD card (every farmer) and the farmers
  list, to open in a spreadsheet app.
  **Edit** (at the bottom of View details, added 30 Oct) opens the same form
  filled in with the record, for a value entered wrong. Saving replaces that
  record on the SD card; it does not add a new one. Its photos stay (× takes one
  off the record, the file stays on the card) and more can be added up to 3. If
  the record changed since the list was loaded (another phone edited it), the
  ESP32 refuses and asks to open it again. A farmer's **name** is fixed in the
  farmer sheet (**Change the name**); the user ID cannot change, the records
  point to it.
  **Delete** (under Edit in View details, added 1 Nov) removes one record and
  its photos from the SD card after a second tap ("Yes, delete this record").
  It has the same row + rev check as Edit. A planting can't be deleted while its
  plot has growth records and no other planting; the reason says to delete those
  first.
- **Photos** (any record, up to 3): **Take photo** or **Choose photo**. The phone
  makes a compressed copy (at most 1024 px, JPEG, usually 50 to 200 KB), and both
  files go to the SD card when the record is saved: the original as the save file
  and the compressed copy for the dashboard. A photo over 10 MB, or one that is
  not JPEG, PNG or WebP, is re-saved by the phone as a 4096 px JPEG first. Tap a
  photo for a full view with **Save original** and **Save compressed**.
- **Recommend**: pick a planted plot. The phone ranks Recipes A, B and C with the
  rules and worked examples of agridose-game docs/03 (same text, same numbers),
  using the newest soil test of that plot, the crop's pH range, and this month's
  Maasin rainfall normal. Fertilizer comes from the plot's fertilizer
  applications on or after the soil test date (**+ Add fertilizer** saves one to
  the SD card); urea, ammonium sulfate and complete 14-14-14 are counted, other
  fertilizers are listed but have no pH factor. The rain in mm is typed here and
  starts at the Maasin normal for the days since the test. Shows the recommended
  recipe, the other two (C is "Not advised" above the crop's range), the split for
  a dose of 300 to 900 g (100 to 300 g until 1 Nov), the **keys to press at the machine** (e.g.
  `# B # 6 0 0 # #`), and **Why?** with every step. Nothing is sent to the
  machine: the recipe is still chosen at the keypad.
- **Crop Guide**: the 12 crops with local names, pH ranges and sources. (The
  "verify" tag on entries from secondary copies was taken off the page on 1 Nov;
  `verify` is still marked in `www/agri.js` for the citations.)
- **Batch Log**: the dose history from the SD card, newest first, each with the
  date and time it was made (or "Time not known"), **View details** (target split
  per feedstock) and **Download CSV**.
- **The clock** (1 Nov): the ESP32 has none, so the page gives it the phone's time
  (`POST /api/clock`): any phone while the ESP32 has no time (after every
  restart), and a logged-in phone if the ESP32 is more than 2 minutes off. The
  Machine tab shows the ESP32's clock at the bottom ("clock 14:05" or "clock not
  set yet").

All the heavy work runs on the phone (in the page): the recommendation
(`www/agri.js`), sorting, day counts and shrinking photos. The ESP32 only
stores files and serves them.

## 4. Look-only: how the code guarantees it

Doses, zeroing and stopping can only be sent through `linkRequest()` in
`mega_link.h`, and only the keypad code calls it
(`AgriDose_ESP32_Link.ino`: the STOP, DOSE and TARE key handlers). Checked on 28 Oct by searching the code:
- `web_server.cpp` and every header it includes never include `mega_link.h` or
  `AgriLink.h`, so a web handler calling `linkRequest()` would not even compile.
- Only `mega_link.cpp` writes to the Mega's UART.
- Messages a phone sends on the WebSocket are ignored (`web_server.cpp`, `onWsEvent`).
- The only things the web side writes are farmers, crop records and crop photos
  on the SD card.

## 5. Files on the microSD card

| File | Columns |
|---|---|
| `/dose_log.csv` | dose_no, boot, uptime_s, seq, recipe, target_g, actual_g, error_g, error_pct, date_time |
| `/crop/users.csv` | user_id, name |
| `/crop/soil.csv` | user_id, plot, date_tested, soil_ph, soil_moisture_pct, photos |
| `/crop/plantings.csv` | user_id, plot, crop, variety, date_planted, used_agridose, dose_numbers, photos |
| `/crop/growth.csv` | user_id, plot, stage, date_observed, date_planted, days_after_planting, height_cm, leaf_count, notes, photos |
| `/crop/fertilizer.csv` | user_id, plot, date_applied, fertilizer, amount_kg, notes |
| `/crop/photos.csv` | photo_id, user_id, file, bytes, compressed_file, compressed_bytes |
| `/crop/photos/0012.jpg` | photo 12 as it was taken (the save file; `.png` or `.webp` if it was one) |
| `/crop/photos/0012_compressed.jpg` | photo 12's compressed copy, made by the phone |

`photos` in a record holds photo numbers, e.g. `12 13`. A photo whose record was
never saved (the farmer closed the form) stays on the card, unused.

A CSV file written by the 28 Oct firmware has other columns. At start-up the
ESP32 renames it to `soil_old.csv` (`_old2`, ...) and starts a new one, so old
and new rows never mix under one header. Nothing is deleted.

**Logins** are in `/auth/admin.csv` (username, rounds, salt, hash, must_change)
and `/auth/farmers.csv` (user_id, username, rounds, salt, hash). No password is
stored anywhere: only a PBKDF2-HMAC-SHA256 hash (10,000 rounds, its own random
16-byte salt), so someone who reads the card still does not know the passwords.
No web address ever sends these files. Five wrong passwords in a row lock that
username for 5 minutes; a restart of the ESP32 logs every phone out.
**Forgot the admin password?** Put the card in a laptop and delete
`/auth/admin.csv`. At the next start-up the ESP32 makes the first admin again
(`jomar` / `1234`, must be changed). The farmers' logins stay.

**Data rules (30 Oct)**, checked by the ESP32 on every save, with the reason
shown on the phone: no two farmers with the same name (capitals and spaces
ignored); no second record of the same kind for one farmer with the same plot
and date (fertilizer: and the same fertilizer), so a wrong one is edited, not
added again; no date after the phone's today; a growth record on a plot that
has a planting, observed on or after the date planted; a planting's dose
numbers must be in the Batch Log.

An **edit** changes the record's line in place, so a corrected record keeps its
position and the CSV has no duplicate. The ESP32 writes the whole file again as
`soil.csv.new`, then swaps it in (`soil.csv` -> `soil.csv.bak`, `.new` ->
`soil.csv`, `.bak` removed). After a power cut in the middle, the next start-up
keeps whichever copy is whole and removes the other. Only the crop files and
`users.csv` are ever edited; `/dose_log.csv` is never changed (except once:
a log from before 1 Nov gets the new header with `date_time` at start-up; its
old rows stay as they are, without a time).

A dose is stamped with `boot` (how many times the ESP32 has started) and
`uptime_s`, and since 1 Nov with `date_time` ("2026-11-01 14:05", the time zone
of the phone that set the clock). The ESP32 has no clock of its own: it takes
the time from a phone (`wall_clock.*`, rules in `clock_rules.h`), and loses it
at every restart. A dose made before a phone opened the dashboard has an empty
`date_time`. A battery-backed clock module (DS3231) would remove that gap; none
is fitted. `dose_no` never repeats; plantings refer to doses by that number. Crop dates are the ones typed on the phone. Every SD access goes
through one mutex, taken once per small piece of work (a download takes it once
per 4 KB, a photo upload once per 8 KB, never for the whole file). One photo
upload runs at a time; a second phone gets "try again in a moment".

## 6. Decisions made while writing (not in CLAUDE.md; confirmed by the team)

Protocol (`AgriLink.h`, marked `[ADDED]` / `[DEFINED HERE]`):
1. **`DISP,<line1>,<line2>`** (new): the keypad menu text for the Mega's LCD. Needed
   because the keypad is on the ESP32 and the LCD on the Mega.
2. **WT `<dg>`** = mass dispensed so far in the current dose (all hoppers added);
   it holds after the dose and resets at TARE.
3. **I / D / F** = Idle / Dosing / Fault.
4. A fault (hopper empty, sensor, timeout) **pauses the dose** (changed 3 Nov; until
   then it ended it). The augers stop, flag F stays, and the LCD names the fault and
   goes round what to check (`FAULT_CHECKS` in the Mega sketch), then `HANDS CLEAR?`
   / `#=GO ON *=END`. `#` sends RESUME: the same hopper carries on, weighed afresh, so
   stock added or removed during the pause isn't counted. If the cause is still there,
   the fault comes back. `*` sends STOP: the dose ends and DONE reports what came out.
   DONE goes out only then, so a dose still has one log row however many faults it had.
   Top-ups (TRIM) stay best effort: a hopper that stalls in a top-up is accepted as it
   is, with no fault.
5. The Mega refuses a new DOSE (`NAK BUSY`) until the previous DONE got its GOT.
6. ACK timeout 500 ms, at most 6 sends (3 s). HELLO goes out again right before the
   ESP32's first command. If the Mega restarts while a command waits, that command
   is dropped, never re-sent.
7. Recipes live in `AgriLink.h`. Hull and ash are rounded to the nearest 0.1 g;
   compost gets the rest so the three add up exactly.
8. **`HW,<hull dg>,<compost dg>,<ash dg>`** (added 30 Oct): what is in each hopper,
   for the hopper gauges. To keep noise off the link and the phones, the Mega
   sends it only when a hopper moved **more than 5 g** from the value it last
   sent (compared with what was sent, so a slow drift still goes out once it adds
   up), at most every 500 ms while dosing, and every 10 s anyway. The ESP32
   pushes to the phones only when a value changed.
9. **`RESUME,<seq>` and WT flag `R`** (added 3 Nov): see "Saved state" below.

Stock check before a dose (added 10 Nov, `AgriDose_ESP32_Link/stock_check.h`; the
protocol did not change):
- **The rule:** a hopper is short when its weight (the last `HW`) is under its share of
  the dose (the same `agriSplitDose` the Mega uses) plus `STOCK_SPARE_G` (config.h, 20 g
  each, a placeholder: material the auger can't reach, and the 5 g an `HW` may be
  behind). A hopper the recipe doesn't use is never short.
- **Keypad:** checked after the mass is typed and again at `#` on CONFIRM. A short
  hopper gives `LOW COMPOST` / `NEED 650 HAS 410` (NEED includes the spare) for each
  short hopper in turn, then `REFILL HOPPER OR` / `#=GO ON *=BACK`. Refilling turns it
  into CONFIRM by itself; `#` starts the dose anyway (a hopper that runs out only
  pauses it); `*` goes back to the mass. No hopper weights yet (Mega offline, or no
  `HW` since start-up): no check, as before.
- **Dashboard:** the Machine tab's **Enough stock for** uses the same rule, worked out
  on the ESP32 so the two never disagree. `FAKE_MEGA` now sends pretend `HW` too.
- **Known limit:** C reads about 1.5 % of B's load light, so a full B can make ash
  look a few grams lower than it is. That only makes the warning come a little early.

Low hopper on the bell (added 10 Nov, later; `www/app.js`, `LOW_HOPPER_G`):
- A hopper whose last `HW` weight is under **200 g** puts the red dot on the bell, and
  the bell lists it: `Hopper B (compost) is low: 150 g left, under 200 g. Refill it
  before the next dose.` One line per low hopper. Worked out on the phone; the ESP32
  did not change for it. A warning only: whether a dose fits is still the stock check
  above. No hopper weights yet, or the Mega offline: no low alert.

Drain a hopper (added 10 Nov, later; both boards, and `AgriLink.h`):
- **Keypad:** `D` on HOME (`AGRIDOSE D=DRAIN`) opens `DRAIN  A=HULL` / `B=COMP C=ASH`.
  `A`, `B` or `C` shows what that hopper holds (`COMP HAS 4123g` / `#=DRAIN *=BACK`).
  `#` starts it. `*` goes back, `D` goes home.
- **Mega:** runs that one auger at its `PWM_MAX`, no pulsing (there is no target).
  LCD `DRAINING COMP` / `OUT 1234g *=STOP`. It stops on `*` (`DRAIN STOPPED`), by
  itself when under 1 g came out in 15 s (`NO FLOW: EMPTY?`, the same window as the
  EMPTY fault: the hopper is empty, or stuck), on a dead load cell (`DRAIN: NO SENSOR`)
  and after 10 minutes (`DRAIN TIME UP`, `DRAIN_MAX_MS`; `D` again to go on). Then
  `COMP 1234g OUT` for 3 s.
- **Protocol:** `DRAIN,<seq>,<A|B|C>` (idle only, NAK `BUSY` otherwise, `ARGS` for any
  other letter) and WT flag `E` while it runs. Flash **both** boards and keep both
  `AgriLink.h` copies the same: an older ESP32 reads flag `E` as a bad frame and
  shows *Mega offline* while a drain runs.
- **Not a dose:** no DONE, no log row, the WT mass stays the last dose's, the learned
  stop-early grams don't change, and nothing is saved, so after a power cut the
  auger stays off. While draining, `DOSE`, `TARE` and `RESUME` are refused (`BUSY`).
- **Dashboard:** Machine Status says **Draining** (A hopper is being emptied); the
  hopper gauge goes down as it drains.
- **Decided by Claude, to confirm with the team:** the screen texts, full speed, the
  15 s no-flow stop, the 10-minute limit, and that the material comes out where a
  dose does (put a container under the outlet).

Saved state (added 3 Nov, `AgriDose_Mega_Link/dose_state.h`):
- **Why it saves ahead of time:** the Mega gets no warning of a power cut (no pin
  senses it), so it saves to its EEPROM while it runs. With the auger off: before
  every hopper start and top-up, when a hopper finishes, when a dose ends, and after
  zeroing. While an auger runs: every 5 s, one byte per loop pass, so dosing never
  waits for the EEPROM. An auger never starts before the save that accounts for it.
- **What comes back:** the scale zeros, so a loaded hopper keeps its true weight and
  the hoppers no longer have to be empty at power-on; the learned stop-early grams;
  and the dose. Only a hopper with no saved zero is zeroed at power-on, which means
  a blank EEPROM or a changed record layout (`STATE_MAGIC`).
- **Power back during a dose:** no auger moves. The Mega weighs the hopper that was
  running, reports flag R and asks on the LCD (`RESUME A 600g?` / `201g #=YES *=NO`).
  `#` sends RESUME and the dose carries on with its old seq. `*` sends STOP: the dose
  ends and is logged with what came out. **Decision:** it asks instead of restarting
  by itself, because hands may be near an auger after a power cut.
- **The grams** come from the scale (`base + startG - weight now`), which also counts
  what came out after the last save. If that is more than 10 g under the save or
  150 g over it, material probably changed while the power was off, or the reading
  is bad. The saved grams are used then, and the LCD says `WEIGHT CHANGED!`.
- **Power back after a dose that had no GOT:** DONE goes out again, so the dose log
  gets its row. The ESP32 ignores it if that row is already there.
- **A power cut during a save:** the records go round 16 slots, each with a CRC. A
  half-written one fails the CRC and the one before it is used. The ring takes about
  1.6 million saves, which is over 50,000 doses.
- **A power cut during a fault pause:** the save says so (`STATE_PHASE_FAULT`), and at
  power-on the saved grams are used without asking the scale, because the auger was
  off and the operator may have refilled the hopper.
- **Known limits:** only the Mega's own power counts. With the motor supply off and
  the Mega still on (on USB, say), the dose pauses with an EMPTY fault after 15 s:
  switch the motor supply on and press `#`. Material added or removed while off goes unnoticed if the change is small
  (under 10 g added, under 150 g removed). A saved zero can't know that a hopper was
  moved or rebuilt: press `0` with the hoppers empty. A resumed dose has no
  `date_time` until a phone has set the clock again. How far the real load cells'
  zero moves across a power-off has not been measured.

Other:
- Keypad: on HOME, `0` = zero the scales, `D` = drain a hopper. `*` during a dose or
  a drain = STOP.
- Farmers are added on the dashboard ("Add a farmer") or by editing `users.csv`;
  IDs 1 to 9999. Growth records are linked to a planting by plot number.
- Crop fields come from `agridose-v2/docs/05-phone-app.md`.
- The page is stored in the ESP32's flash, not on the SD card, so it works without a
  card. For Chapter 3: "The dashboard page is stored in the ESP32's flash memory;
  the microSD card stores the dose log and crop records."
- The Mega sketch stays a plain `loop()` (the bench-tested 3-hopper code), not
  FreeRTOS as CLAUDE.md says. The mixer and gate are not driven (no pins known);
  the old 3 s "MIXING" placeholder was removed. The idle LCD no longer shows the
  three hopper stocks, because WT now carries one value.
- The Bisaya lines on the sticker were checked by the team.

Added 29 Oct (photos, and the look and tabs of the agridose-game phone app).
Not confirmed by the team yet:
- The real machine has no plot area and no rain gauge. Fertilizer is a fourth
  record type on the SD card; Recommend counts a plot's applications dated on or
  after its newest soil test (a test and an application on the same day: the
  application counts). The rain is typed on the Recommend tab (kept on that phone
  only) and starts at the Maasin normal for those days, spread evenly over each
  month. With no soil test it counts from the planting date and uses the default
  pH 5.5, as in docs/03.
- Fertilizer names: "Urea (46-0-0)", "Ammonium sulfate (21-0-0)", "Complete
  (14-14-14)", or "Other" typed by the farmer (listed, not counted). The page
  knows them by their first word (`fertKey` in `www/agri.js`).
- The Bisaya lines are a first draft by the developers, not checked yet. That
  includes the ones added with Edit on 30 Oct ("Usba", "I-save ang kausaban",
  "Usba ang ngalan", "Usba ang pagsusi sa yuta", ...).
- The game's "Suggested batch" (plot area x 500 g/m², 500 to 2500 g) is replaced
  by a dose split for 300 to 900 g (100 to 300 g until 1 Nov 2026), the real machine's range (AgriLink.h).
- Seed types are the Crop Guide's 12 crops, so every planting has a pH range.
  Growth stages: Seedling, Vegetative, Flowering, Fruiting, Harvest.
- A growth record is added from a planting (as in the game); it stores the plot,
  the date planted and the day count.
- The Batch Log has no "accepted/rejected": the real dose log does not record
  it (no accept step on the keypad). It has a date since 1 Nov, from the phone's
  clock (section 5).
- Photo limits: 3 per record, 10 MB for the original, 1 MB for the compressed copy.

## 7. What was tested, and what still needs testing on the real boards

Done on the laptop (28 Oct):
- Both sketches compile with arduino-cli (ESP32 core 3.3.2, AVR core 1.8.7);
  ESP32 with `FAKE_MEGA` 0 and 1. No compiler warnings from our files.
- `tests/test_agrilink.cpp`: CRC, frames checked against an independent CRC
  (Python), noise, cut and corrupted frames, recipe split for every gram 100 to 300.
- `tests/test_validate.cpp`: every crop input check.
- The page in a phone-sized browser (Edge, 412 px) against `tools/mock_esp32.py`:
  no JavaScript errors, no sideways scrolling, a soil test saved through the form,
  and the WebSocket reconnected after the server went away and came back.

Done on the laptop (29 Oct, photos and the new tabs):
- Both sketches compile again, ESP32 with `FAKE_MEGA` 0 and 1, no warnings from
  our files; 87 % of the app space.
- `tests/test_validate.cpp` now also checks photo numbers;
  `node tests/test_recommend.mjs` checks the 9 worked examples of docs/03, the
  dose split against `agriSplitDose()`, and the rain estimate.
- `node tests/test_dashboard.mjs` (headless Edge at 390 x 844 against the mock):
  every tab, farmer switch, a soil test saved with a photo (original and
  compressed copy uploaded, thumbnail shown, photo view with save links), a growth
  record from a planting with its day count, Rosa plot 1 giving Recipe B, the
  fertilizer form (opened from Recommend on the right plot) turning it into
  Recipe C, the keypad steps following the recipe, the Bisaya switch, sun mode,
  the CSV downloads, 12 crops in the guide, no sideways scrolling and no
  JavaScript errors.

Done on the laptop (3 Nov, saved state). On the machine so far, only bench step 1
below has been run: a dose has not been cut off yet.
- `tests/test_dose_state.cpp`: the record and the ring, with a power cut at every
  byte of a save, a torn byte, and a flipped bit anywhere in a record.
- `python tests/test_power_cut.py`: compiles the real Mega sketch against a pretend
  Arduino (`tests/mega_sim/`) and runs one process per power-on, with modelled
  hoppers. 342 cuts across a 600 g dose, each followed by `#`: no auger moved before
  the answer, every hopper landed on target (worst 4.2 g, the same as a dose with no
  cut) and there was one DONE. Also `*` at the question, two cuts in one dose,
  repeated power-offs, a zero 3 g off, material added or removed, and DONE with no
  GOT. It checks the firmware's logic, not the accuracy of the real machine. Eight
  deliberately broken copies of the sketch were all caught.

Done on the laptop (10 Nov, drain). Not yet run on the machine: `tests/test_power_cut.py`
(80 checks) drains each hopper on the real Mega sketch: B runs until empty and stops
by itself 15 s later, showing the grams within 3 g; `*` stops it at once; while
draining, DOSE, TARE and RESUME are refused, and a DRAIN during a dose is refused
with the dose still on target; a jammed auger and a dead load cell stop it; 10 kg of
hull stops at the 10-minute limit; a power cut during a drain leaves every auger off
at power-on; a dose straight after a drain lands on target. No DONE in any of them,
and no other auger ever turns. `tests/test_agrilink.cpp` checks flag `E` and the
hopper letter. Both sketches compile (ESP32 91 %, also with `FAKE_MEGA`; Mega 12 %
of flash, 48 % of RAM).

Done on the laptop (3 Nov, faults pause the dose). Not yet run on the machine:
`tests/test_power_cut.py` also runs each fault on the real sketch: a hopper running
out in each position and being refilled, motor power off and back on, a dead load
cell, a 4-minute TIMEOUT, `#` with nothing fixed (the fault comes back), `*`, a power
cut during the pause and after it, and one bad reading just before a fault (without
the re-weigh it dosed 308 g of hull instead of 150 g). Nine more deliberately broken
copies of the sketch were all caught.

To test on the bench (hoppers may be empty for 1 to 4, and for 6):
1. First power-on after flashing, hoppers empty: `ZEROING SCALES`, then `Saved state
   found` on the next power-on, with no zeroing. **Passed 3 Nov** on a reset, along
   with a keypad zero being saved. A reset keeps the HX711s powered, so step 2 still
   needs a real power-off.
2. Put the 261 g wrench in a hopper, switch off and on: it still reads about 261 g.
   The difference is how far the zero moves across a power-off, which is what
   `STATE_DRIFT_G` allows for.
3. Start a dose, cut the Mega's power within 5 s of the first auger starting, switch
   on: no auger moves, and the LCD asks. `*` ends it and logs one row.
4. The same again, but answer `#`: with an empty hopper it runs on to `EMPTY: HULL`.
5. With material: cut the power in the middle of a dose, answer `#`, and weigh the
   output on a reference scale.
6. Start a dose with hopper A empty: `EMPTY: HULL` after 15 s, and the LCD goes round
   `REFILL HOPPER?`, `STUCK? TAP JUG`, `MOTOR POWER ON?`, `HANDS CLEAR?`. No auger
   moves and no row is logged yet. `#` gives the same fault again after 15 s; `*`
   logs one row.
7. With material: start a dose with the motor supply off. At `EMPTY: HULL` switch it
   on and press `#`: the dose finishes. Weigh the output on a reference scale.
8. [10 Nov] Keeping the grams: in a dose with material, switch the motor supply off
   once some grams are out, and push down on that jug with a hand as the fault
   appears. Let go, switch the motor on, press `#`: the LCD must carry on from the
   grams it showed before the fault, not from 0 g, and Serial Monitor prints
   `jug touched? keeping those`. Weigh the output on a reference scale.
9. [10 Nov] Stock check: with less compost than a dose needs (say 400 g for `A 900`),
   type the dose: `LOW COMPOST` / `NEED 650 HAS ...` instead of the check screen. Add
   compost: it turns into the check screen by itself. Then measure how much each jug
   still holds when its auger feeds nothing, and set `STOCK_SPARE_G` from that.
10. [10 Nov] Drain: with a container under the outlet and about 500 g in hopper B,
    press `D`, `B`, `#`. Only auger B turns, the LCD counts `OUT ...g`, and the
    dashboard says *Draining*. `*` stops it at once (`DRAIN STOPPED`). Again with
    `#`: when B is empty it stops by itself 15 s later (`NO FLOW: EMPTY?`). Weigh
    the container against the grams shown. No row lands in `/dose_log.csv`.
11. [10 Nov] Low alert: take hopper C under 200 g: the bell gets its red dot and
    lists `Hopper C (wood ash) is low`. Refill it: the dot goes.

Done on the bench (30 Oct, both boards on the Link firmware; the augers are not built yet, only the outlet pipes):
the link comes up, a dose typed as keys in the ESP32's Serial Monitor runs and
is logged to `/dose_log.csv` (still there after a restart), and a soil test
saved from the phone app lands on the SD card. The Android app's **Take photo**
did not respond on the phone (not solved yet).

Done on the laptop (30 Oct, Edit):
- The ESP32 sketch compiles with the edit routes; 88 % of the app space.
- `node tests/test_dashboard.mjs` also edits the new soil test (form filled in,
  photo kept, no record added), refuses an edit with an old `rev`, edits a growth
  record's stage, and fixes a farmer's name.
- Run on the ESP32 the same evening: a soil test edited from a phone (same line
  each time) and a 3.1 MB photo saved with it.

Done on the laptop (30 Oct, hopper weights, data rules, logins):
- Both sketches compile (ESP32 89 % of the app space, Mega 8 %).
- `tests/test_stock.cpp` (10 Nov): the stock check, against shares worked out by hand,
  and 3000 random stocks. `node tests/test_dashboard.mjs` checks the Machine tab's
  Enough stock for.
- `tests/test_agrilink.cpp`: the HW deadband (+-5 g not sent, 5.1 g sent, a slow
  drift sent once it adds up, the longest HW frame fits).
- `node tests/test_dashboard.mjs` (104 checks since 10 Nov, with the low-hopper bell and
  Draining): the hopper gauges and total; each
  data rule refused with its reason; the machine open without a login and the
  records closed (401); a wrong password refused; the first admin login forced
  to a new password (and nothing else allowed meanwhile); the admin setting a
  farmer's login (a username can't belong to two farmers); Rosa logging in and
  seeing only her own records, downloads and name, and refused (403) for Juan's.
- Not yet run on the boards: the HW frames from the real load cells; a login on
  the ESP32 (Serial prints how long one password hash takes: about a second or
  less is fine, else lower `PBKDF2_ROUNDS`); the logins after a restart.

Done on the bench (1 Nov, LCD and keypad; auger B built, A and C not):
- **LCD fix in `AgriDose_Mega_Link`:** the LCD had stayed blank with this
  firmware. `lcdShow()` padded with `snprintf("%-*s")`, and the Mega's
  snprintf (avr-libc) doesn't support the `*` width, so it printed an empty
  string and nothing was ever written. It now pads by hand in `padLine()`.
- **Standalone tests** (`calibration_center/lcd_test`, `keypad_test`):
  - The LCD answers at 0x27 (500/500), and a hidden code was read correctly
    off the glass.
  - The keypad gave 16/16 keys, with no shorts and no bounces.
- **Keypad and LCD together:** the menu went
  `#` `A` `#` `100` `#` to `A 100g 25/70/5%` on the glass, and `#` started the
  dose. HULL gave `EMPTY` after 5 s (no auger A), and compost never ran. `*`
  cleared the fault, the result `DONE 0.0/100g` showed, and it was logged to
  `/dose_log.csv`. `D` returns to HOME, and `0` opens ZERO SCALES?.
- **Not done (the team called it done):** `*` pressed while an auger is
  running. It was only pressed after the fault so far.

Done on the laptop (1 Nov, the clock, record Delete, no verify tag):
- `tests/test_clock.cpp` (26 checks): the clock rules (any phone sets an unset
  clock; once set, only a logged-in phone more than 2 min off; 1970/1980,
  after 2099 and bad time zones refused) and the time text (leap day, time
  zones west and east, new year).
- `node tests/test_dashboard.mjs`, now also checking:
  - **The clock:** opening the page sets the mock's clock; a phone that isn't
    logged in can't move it once set; a logged-in page puts back a clock moved
    an hour off.
  - **Dose times:** the Batch Log shows a time or "Time not known" for each
    dose; the CSV has `date_time`; dose details say when.
  - **No verify tags** in the Crop Guide.
  - **Delete:** a growth record is deleted from its details after the second
    tap. A planting with growth records and a delete with an old rev are refused.
- The ESP32 sketch compiles (90 % of the app space).
- **Not yet run on the ESP32:** the clock from a real phone, a dose logged
  with its time, the old `/dose_log.csv` header being upgraded at start-up, and
  a Delete on the real SD card (with a photo).

Changed on 1 Nov (evening): **the dose range is 300 to 900 g** (was 100 to 300 g),
in both `AgriLink.h` copies, the ESP32 menu (from the same constants), and the
page (`agri.js`, Recommend starts at 600 g; a mass saved under the old range
starts at 600 g). The per-hopper safety cutoff `HOPPER_TIMEOUT_MS` is now 240 s
(was 120 s): the biggest share, 630 g of compost for recipe A at 900 g, takes
about 90 s at 10 g/s. All laptop tests pass again (`test_agrilink` checks the
recipe split for every gram 300 to 900). Not checked yet: whether each jug holds
its share of a 900 g dose, and the load cells' rated capacity.

Not done: nothing ran on an ESP32 or a Mega, so **free heap with 3 phones was not
measured** (use the readout at the bottom of the Machine tab), and **photo
uploads to a real SD card were not timed** (expect several seconds for a 5 MB
photo). The Android app's camera and gallery buttons have not run on a phone. To
test on the bench:
1. Link up: both Serial Monitors report the link. Pull the ESP32 RX wire: the
   dashboard says *Mega offline* within 2 s.
2. A dose from the keypad (`#`, `A`, `#`, `250`, `#`, `#`): it runs, the result
   shows on the LCD, one row lands in `/dose_log.csv`, the dashboard updates.
3. `*` during a dose stops the augers at once; an empty hopper gives flag F, and
   `#` continues the dose or `*` ends it. The Mega can take up to about 1 s to read `*` while it is taking
   the first weight of a hopper.
4. `0`, `#` on HOME zeroes the scales (about 6 s; the dashboard shows *Mega
   offline* meanwhile, which is expected).
5. Restart the ESP32 in the middle of a dose: the keypad follows the Mega, and the
   dose is logged once.
6. No SD card: the keypad and the dosing still work, the dashboard shows the banner.
7. 3 to 5 phones on the dashboard during a dose: live updates keep coming, the
   CSV download works, and note the free and lowest heap.
8. Watch the Mega's weight readings with the ESP32 access point on and phones
   connected (Wi-Fi noise; the boards have separate supplies).
9. **Tune the dosing engine for 300 to 900 g doses** (100 to 300 g until 1 Nov). It was tuned for 2 to 3 kg
   batches. Wood ash is only 15 to 90 g per dose, while `TOLERANCE_G` is 2 g and
   the slow zone is at least 25 g.
10. "Actual" is the sum of the three hoppers' loss-in-weight readings. For the
    accuracy chapter, weigh the output on a reference scale as well.
11. Photos: save a record with a phone camera photo (3 to 8 MB) and time it; a
    second phone uploading at the same time gets "try again in a moment"; walk out
    of Wi-Fi range mid-upload and check that no half file stays in `/crop/photos/`;
    start a dose during an upload and check that the dose is still logged.

## 8. Changing the page

Edit the files in `www/`, then check them on the laptop:
`python AgriDose_ESP32_Link/tools/mock_esp32.py` and open `http://127.0.0.1:8765/`
(made-up data: Juan and Rosa from the game's sample users; it takes photo uploads
but does not check form input like the ESP32 does). Then run
`python AgriDose_ESP32_Link/tools/make_web_assets.py` and upload the sketch again.

The crop table, rainfall normals, recipes and the ranking rules are in
`www/agri.js`, copied from agridose-game `data/` and docs/03. Change both
together, then run `node tests/test_recommend.mjs` (the 9 worked examples).
The record fields (and the Seed type and Growth stage lists) come from
`crop_records.cpp`; the mock copies them by hand.
