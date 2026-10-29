# AgriDose dashboard app (Android)

An Android app for the AgriDose machine's **look-only** web dashboard. The
dashboard itself runs on the machine's ESP32 at `http://192.168.4.1/`; this app
opens it full-screen with its own icon, instead of a Chrome tab.

What the app adds over Chrome:
- **It works with mobile data on.** The AgriDose Wi-Fi has no internet, so Android
  often sends traffic over mobile data instead, and the dashboard never loads. The
  app binds itself to the Wi-Fi network, so it always reaches the machine.
- **A help page** when the machine cannot be reached (join the Wi-Fi, tap *Stay
  connected*), which retries by itself every 5 seconds.
- **Download CSV** (dose log), **Download records (CSV)** (crop records, farmers),
  **Save original** and **Save compressed** (crop photos) save the file wherever
  you choose on the phone.
- **Take photo** on a crop record opens the phone's camera app, **Choose photo**
  the gallery. (A web view does nothing with these buttons unless the app answers
  them.)

The dashboard looks like the team's Blynk-style app: Machine, Records, Recommend,
Crop Guide and Batch Log tabs. The phone does the heavy work inside the page:
it compresses photos, ranks the recipes and sorts the records. The ESP32 only
stores the files on its SD card and serves them back.

It is still look-only: doses are started only from the machine's keypad. The app
gives the page no extra powers (no JavaScript bridge, no camera permission: the
phone's own camera app takes the photo).

## Get the APK

Every push to this repository builds the app on GitHub (`.github/workflows/build-apk.yml`).
1. Open the **Actions** tab, then the newest **Build APK** run with a green tick.
2. Under **Artifacts**, download **agridose-app-apk** (a zip) and unzip it:
   `app-debug.apk`.
3. To build again without changing anything: Actions > Build APK > **Run workflow**.

## Install on a phone

1. Copy `app-debug.apk` to the phone and open it. Allow "install unknown apps"
   for the app you opened it with (Files, Chrome, ...).
2. Updating later: every GitHub build signs the app with a new test key, so
   Android may say "App not installed". Uninstall the old AgriDose app first.
3. Android 7.0 or newer.

## Use

1. Join the **AgriDose** Wi-Fi (code 1 on the machine's QR sticker).
2. Open the **AgriDose** app. If the machine is off or out of range, the help page
   shows and the app keeps trying.

## Files

| File | What it does |
|---|---|
| `app/src/main/java/ph/agridose/dashboard/MainActivity.java` | the app: web view, Wi-Fi binding, help page, downloads, photo buttons |
| `app/src/main/java/ph/agridose/dashboard/PhotoProvider.java` | the one file the camera app may write a new photo to (app cache) |
| `app/src/main/AndroidManifest.xml` | permissions (internet, network state), portrait screen |
| `app/src/main/res/xml/network_security_config.xml` | plain HTTP allowed to 192.168.4.1 only |
| `app/src/main/assets/offline.html` | the help page |
| `tools/make_app_icons.py` | draws the icon (`python tools/make_app_icons.py`) |

The address `192.168.4.1` must match `AP_IP_TEXT` in the firmware's `config.h`.
The Wi-Fi name on the help page must match `AP_SSID`.

Android Studio can also open this folder and build it (it may offer to add a
Gradle wrapper; accept).

## Not tested yet

The app has not run on a phone yet. Check on the team's phones: the dashboard
opens with mobile data on, the help page appears and recovers when the ESP32 is
switched off and on, crop forms work with the keyboard open, and Download CSV
saves a file that opens in a spreadsheet app.

Version 1.1 (photos), also not run on a phone yet:
- **Take photo** opens the camera, and after the shot the photo shows in the form.
  Try two camera apps if the phone has them.
- **Choose photo** opens the gallery; cancelling it leaves the form as it was,
  and the buttons still work afterwards.
- Save a record with a photo, then check the SD card on a laptop: `/crop/photos/`
  has the original and `_compressed.jpg`.
- **Save original** in the photo view saves the full photo on the phone.
- Android's Back button closes an open sheet or photo before it leaves the app.
