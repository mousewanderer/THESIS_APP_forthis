#!/usr/bin/env python3
"""Makes the printable QR sticker for the machine: qr_sticker.html.

Two QR codes:
  1. join the AgriDose Wi-Fi (network name and password from config.h)
  2. open the dashboard in the browser (http://192.168.4.1/)
The name and password are read from config.h, so the sticker always
matches the firmware. Run it again whenever AP_SSID or AP_PASSWORD change,
then print the page at 100 % (no "fit to page").

Needs one package:  pip install segno
Run:                python AgriDose_ESP32_Link/tools/make_sticker.py
"""
import html
import pathlib
import re

import segno
import segno.helpers

SKETCH = pathlib.Path(__file__).resolve().parent.parent
CONFIG = SKETCH / "config.h"
OUT = SKETCH.parent / "qr_sticker.html"
QR_MM = 40          # printed size of each code; 3 cm or more scans easily


def config_value(name):
    m = re.search(rf'#define\s+{name}\s+"([^"]*)"', CONFIG.read_text(encoding="utf-8"))
    if not m:
        raise SystemExit(f"{name} not found in {CONFIG}")
    return m.group(1)


def qr_svg(qr):
    modules = qr.symbol_size(scale=1, border=4)[0]
    return qr.svg_inline(scale=QR_MM / modules, unit="mm", border=4, dark="#000", light="#fff")


def main():
    ssid, password, ip = config_value("AP_SSID"), config_value("AP_PASSWORD"), config_value("AP_IP_TEXT")
    url = f"http://{ip}/"
    wifi = segno.helpers.make_wifi(ssid=ssid, password=password, security="WPA")
    page = segno.make(url, error="m")
    e = html.escape
    OUT.write_text(f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>AgriDose QR sticker</title>
<style>
  @page {{ margin: 10mm; }}
  body {{ margin: 0; font: 12pt/1.35 Arial, sans-serif; color: #000; background: #fff; }}
  .card {{ width: 120mm; margin: 10mm auto; padding: 6mm; border: 0.6mm solid #000; border-radius: 3mm; }}
  h1 {{ margin: 0 0 3mm; font-size: 18pt; text-align: center; }}
  .row {{ display: flex; justify-content: space-between; gap: 4mm; }}
  .step {{ width: 54mm; text-align: center; }}
  .step b {{ display: block; font-size: 13pt; }}
  .ceb {{ display: block; min-height: 2.7em; font-style: italic; }}
  .step svg {{ display: block; margin: 2mm auto; }}
  .text {{ font-family: Consolas, monospace; font-size: 11pt; overflow-wrap: anywhere; }}
  .note {{ margin-top: 4mm; padding-top: 3mm; border-top: 0.3mm solid #000; font-size: 10.5pt; }}
</style></head><body>
<div class="card">
  <h1>AgriDose dashboard</h1>
  <div class="row">
    <div class="step">
      <b>1. Join the Wi-Fi</b><span class="ceb">I-scan aron makakonek sa Wi-Fi</span>
      {qr_svg(wifi)}
      <div class="text">Wi-Fi: {e(ssid)}<br>Password:<br>{e(password)}</div>
    </div>
    <div class="step">
      <b>2. Open the dashboard</b><span class="ceb">I-scan aron maablihan ang dashboard</span>
      {qr_svg(page)}
      <div class="text">{e(url)}</div>
    </div>
  </div>
  <div class="note">
    If the phone says <b>"no internet"</b>, tap <b>Stay connected</b>.<br>
    <span class="ceb">Kon moingon ang telepono og "no internet", pindota ang "Stay connected".</span>
    Look-only: doses are started on the machine's keypad.<br>
    In Chrome: menu &#8942; &rarr; <b>Add to Home screen</b>.
  </div>
</div>
</body></html>
""", encoding="utf-8")
    print(f"wrote {OUT}  (Wi-Fi '{ssid}', {url})")


if __name__ == "__main__":
    main()
