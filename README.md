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
- **Download CSV** saves the dose log wherever you choose on the phone.

It is still look-only: doses are started only from the machine's keypad. The app
gives the page no extra powers.

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
| `app/src/main/java/ph/agridose/dashboard/MainActivity.java` | the whole app: web view, Wi-Fi binding, help page, CSV save |
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
