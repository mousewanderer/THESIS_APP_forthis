#!/usr/bin/env python3
"""A pretend ESP32 for working on the dashboard page on a laptop.

It serves the files in www/ and answers the same API as web_server.cpp
(/api/..., and the live feed on /ws) with made-up data: a dose runs every
20 s. Edit www/, refresh the browser, no flashing needed. Plain Python,
no extra packages.

This is NOT the firmware: it does not check form input the way the ESP32
does (crop_validate.h). Use it for the look and feel only.

1 Nov 2026: it also copies the clock (POST /api/clock, the same rules as
clock_rules.h; it starts with no time, like a freshly started ESP32), the
date_time of each dose, and the record Delete (POST /api/delete/<type>).

Run:   python AgriDose_ESP32_Link/tools/mock_esp32.py
Open:  http://127.0.0.1:8765/
"""
import base64
import hashlib
import hmac
import json
import os
from email.parser import BytesParser
from email.policy import default as email_policy
import pathlib
import random
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

WWW = pathlib.Path(__file__).resolve().parent.parent / "www"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765    # optional: another port
TYPES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/agri.js": ("agri.js", "application/javascript; charset=utf-8"),
    "/app.js": ("app.js", "application/javascript; charset=utf-8"),
    "/app.css": ("app.css", "text/css; charset=utf-8"),
    "/manifest.webmanifest": ("manifest.webmanifest", "application/manifest+json"),
    "/icon-192.png": ("icon-192.png", "image/png"),
    "/icon-512.png": ("icon-512.png", "image/png"),
}

# Same fields as crop_records.cpp (copied by hand; keep them in step).
SEED_TYPES = ["Pechay", "Rice (lowland)", "Corn", "Tomato", "Eggplant", "Okra", "Bitter gourd", "String bean",
              "Squash", "Cabbage", "Water spinach", "Sweet potato"]
STAGES = ["Seedling", "Vegetative", "Flowering", "Fruiting", "Harvest"]
FERTILIZERS = ["Urea (46-0-0)", "Ammonium sulfate (21-0-0)", "Complete (14-14-14)"]


def fld(name, label, kind, required, lo=0, hi=0, size=0, step=None, options=None):
    f = {"name": name, "label": label, "kind": kind, "required": required, "min": lo, "max": hi, "size": size}
    if step:
        f["step"] = step
    if options:
        f["options"] = options
    return f


PHOTOS = fld("photos", "Photos", "photos", False, size=3)
RECORD_TYPES = [
    {"type": "soil", "title": "Soil tests", "fields": [
        fld("plot", "Plot number", "int", True, 1, 99),
        fld("date_tested", "Soil test date", "date", True),
        fld("soil_ph", "Soil pH", "decimal", True, 0, 14, 2, "0.1"),
        fld("soil_moisture_pct", "Soil moisture (%)", "decimal", False, 0, 100, 1),
        PHOTOS]},
    {"type": "planting", "title": "Seed type and planting", "fields": [
        fld("plot", "Plot number", "int", True, 1, 99),
        fld("crop", "Seed type", "text", True, size=30, options=SEED_TYPES),
        fld("variety", "Variety", "text", False, size=30),
        fld("date_planted", "Date planted", "date", True),
        fld("used_agridose", "Used AgriDose amendment", "yesno", True),
        fld("dose_numbers", "AgriDose dose numbers", "numlist", False, size=40),
        PHOTOS]},
    {"type": "growth", "title": "Growth records", "fields": [
        fld("plot", "Plot number", "int", True, 1, 99),
        fld("stage", "Growth stage", "text", True, size=20, options=STAGES),
        fld("date_observed", "Growth date observed", "date", True),
        fld("date_planted", "Date planted", "date", False),
        fld("days_after_planting", "Growth day count", "int", False, 0, 365),
        fld("height_cm", "Plant height (cm)", "decimal", False, 0, 500, 1, "0.5"),
        fld("leaf_count", "Leaf count", "int", False, 0, 999),
        fld("notes", "Notes", "text", False, size=80),
        PHOTOS]},
    {"type": "fertilizer", "title": "Fertilizer applications", "fields": [
        fld("plot", "Plot number", "int", True, 1, 99),
        fld("date_applied", "Date applied", "date", True),
        fld("fertilizer", "Fertilizer", "text", True, size=30, options=FERTILIZERS),
        fld("amount_kg", "Amount (kg)", "decimal", False, 0, 100, 2, "0.5"),
        fld("notes", "Notes", "text", False, size=80)]},
]

lock = threading.Lock()
started = time.time()
state = {"weight_dg": 0, "flag": "I", "done_count": 0, "last": None}
# Hopper stock in grams, with load-cell noise; "sent" is what the Mega last sent in HW
# (only a move over HW_DEADBAND_G goes out, as in AgriLink.h).
HW_DEADBAND_G = 5.0

# [10 Nov 2026] The stock check, copied from stock_check.h: a hopper is short when it
# weighs less than its share of the dose plus STOCK_SPARE_G (config.h).
RECIPE_PCT = [(25, 70, 5), (30, 65, 5), (30, 60, 10)]   # AGRI_RECIPES in AgriLink.h: hull, compost, ash
STOCK_SPARE_G = (20, 20, 20)
DOSE_MIN_G, DOSE_MAX_G = 300, 900


def stock_short_mask(pct, grams, have_dg):
    total = grams * 10
    hull = (total * pct[0] + 50) // 100                    # agriSplitDose
    ash = (total * pct[2] + 50) // 100
    mask = 0
    for i, share in enumerate((hull, total - hull - ash, ash)):
        if share > 0 and have_dg[i] < share + STOCK_SPARE_G[i] * 10:
            mask |= 1 << i
    return mask


def stock_max_dose(pct, have_dg):
    """(biggest dose in grams, 0 if not even the smallest; the hoppers that limit it)"""
    for g in range(DOSE_MAX_G, DOSE_MIN_G - 1, -1):
        if stock_short_mask(pct, g, have_dg) == 0:
            return g, (stock_short_mask(pct, g + 1, have_dg) if g < DOSE_MAX_G else 0)
    return 0, stock_short_mask(pct, DOSE_MIN_G, have_dg)
stock = {"g": [4200.0, 3600.0, 900.0], "sent_dg": [42000, 36000, 9000]}
doses = [{"no": n, "boot": 1, "uptime_s": 60 * n, "seq": n, "recipe": "ABC"[n % 3],
          "target_dg": 6000, "actual_dg": 6000 + random.randint(-25, 25),
          # dose 1 was made before any phone gave the ESP32 the time (as after a restart)
          "time": "" if n == 1 else f"2026-10-30 08:{n:02d}"} for n in range(1, 8)]

# The ESP32's clock (wall_clock.h / clock_rules.h): not set until a phone sends its time.
CLOCK_MIN_EPOCH, CLOCK_MAX_EPOCH, CLOCK_SLACK_S = 1767225600, 4102444800, 120
clock = {"set": False, "epoch": 0, "at": 0.0, "tz_min": 0}


def clock_now():
    return int(clock["epoch"] + time.time() - clock["at"]) if clock["set"] else 0


def clock_text():
    """'2026-11-01 14:05' in the time zone of the phone that set it, '' if not set."""
    if not clock["set"]:
        return ""
    return time.strftime("%Y-%m-%d %H:%M", time.gmtime(clock_now() + clock["tz_min"] * 60))
# Juan and Rosa as in agridose-game data/sample-users.json (oldest first, like the CSV files).
users = [{"id": 1, "name": "Juan"}, {"id": 2, "name": "Rosa"}]


def row(kind, **values):
    return {f["name"]: "" for f in next(t for t in RECORD_TYPES if t["type"] == kind)["fields"]} | values


def csv_line(kind, r):
    names = [f["name"] for f in next(t for t in RECORD_TYPES if t["type"] == kind)["fields"]]
    return ",".join([r["user_id"]] + [r[n] for n in names])


# The data rules of crop_records.cpp (checkRules, nameFree), in short.
KEYS = {"soil": ["plot", "date_tested"], "planting": ["plot", "date_planted"],
        "growth": ["plot", "date_observed"], "fertilizer": ["plot", "date_applied", "fertilizer"]}
ONE = {"soil": "soil test", "planting": "planting", "growth": "growth record", "fertilizer": "fertilizer application"}


def name_error(name, except_id=0):
    key = " ".join(name.lower().split())
    other = next((u for u in users if u["id"] != except_id and " ".join(u["name"].lower().split()) == key), None)
    return f'Farmer ID {other["id"]} is already called {name.strip()}. Add an initial or a family name to tell them apart.' \
        if other else None


def record_error(kind, spec, rec, today, skip=-1):
    for f in spec["fields"]:
        if f["required"] and not rec[f["name"]]:
            return f'{f["label"]} is required'
        if f["kind"] == "date" and rec[f["name"]] and today and rec[f["name"]] > today:
            return f'{f["label"]} can\'t be after today ({today})'
    uid = rec["user_id"]
    if kind == "growth":
        if rec["date_planted"] and rec["date_observed"] < rec["date_planted"]:
            return f'The date observed can\'t be before the date planted ({rec["date_planted"]})'
        if not any(p["user_id"] == uid and p["plot"] == rec["plot"] for p in records["planting"]):
            return f'Plot {rec["plot"]} has no planting. Add the planting first.'
    if kind == "planting":
        for n in rec["dose_numbers"].replace(",", " ").split():
            if not n.isdigit() or not 1 <= int(n) <= len(doses):
                return f"Dose {n} is not in the Batch Log (it has doses 1 to {len(doses)})"
    keys = KEYS[kind]
    for j, r in enumerate(records[kind]):
        if j != skip and r["user_id"] == uid and all(r[k].lower() == rec[k].lower() for k in keys):
            label = {f["name"]: f["label"] for f in spec["fields"]}
            what = " and ".join(f"{label[k]} {rec[k]}" for k in keys)
            return f"This farmer already has a {ONE[kind]} with {what}. Open it and tap Edit to change it."
    return None


def line_rev(line):
    """FNV-1a, the same "rev" as lineRev() in crop_records.cpp."""
    h = 0x811C9DC5
    for b in line.encode():
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return f"{h:08x}"


records = {
    "soil": [row("soil", user_id="1", plot="1", date_tested="2026-09-08", soil_ph="5.0", soil_moisture_pct="30"),
             row("soil", user_id="2", plot="1", date_tested="2026-09-01", soil_ph="6.1", soil_moisture_pct="34"),
             row("soil", user_id="2", plot="2", date_tested="2026-09-05", soil_ph="6.0", soil_moisture_pct="33"),
             row("soil", user_id="2", plot="1", date_tested="2026-09-10", soil_ph="5.8", soil_moisture_pct="34")],
    "planting": [row("planting", user_id="1", plot="1", crop="Pechay", variety="Native", date_planted="2026-09-08", used_agridose="no"),
                 row("planting", user_id="1", plot="2", crop="Pechay", variety="Native", date_planted="2026-09-08", used_agridose="no"),
                 row("planting", user_id="2", plot="1", crop="Tomato", variety="Native", date_planted="2026-09-03", used_agridose="yes",
                     dose_numbers="3 4"),
                 row("planting", user_id="2", plot="2", crop="Pechay", variety="Native", date_planted="2026-09-05", used_agridose="no")],
    "growth": [row("growth", user_id="2", plot="1", stage="Seedling", date_observed="2026-09-10", date_planted="2026-09-03",
                   days_after_planting="7", height_cm="4.5", leaf_count="4")],
    # Rosa: urea on plot 1 before its 2026-09-10 soil test (not counted), complete on plot 2 after its test
    "fertilizer": [row("fertilizer", user_id="2", plot="1", date_applied="2026-09-05", fertilizer="Urea (46-0-0)", amount_kg="0.5"),
                   row("fertilizer", user_id="2", plot="2", date_applied="2026-09-12", fertilizer="Complete (14-14-14)")],
}
photos = {}          # id -> {"full": (bytes, type), "compressed": (bytes, type), "user_id": "2"}


# Logins as in accounts.cpp: a PBKDF2-SHA256 hash with its own salt, never the password.
# The admin starts with the first password from config.h and must change it; Juan and
# Rosa have logins (juan1234, rosa1234) for working on the page.
def make_hash(password):
    salt = os.urandom(16)
    return {"salt": salt, "hash": hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 10000)}


def password_ok(account, password):
    return hmac.compare_digest(hashlib.pbkdf2_hmac("sha256", password.encode(), account["salt"], 10000), account["hash"])


accounts = {"admin": {"role": "admin", "user_id": 0, "must_change": True} | make_hash("agridose-admin"),
            "juan": {"role": "farmer", "user_id": 1, "must_change": False} | make_hash("juan1234"),
            "rosa": {"role": "farmer", "user_id": 2, "must_change": False} | make_hash("rosa1234")}
sessions = {}        # sid -> username


def login_error(username, password, user_id):
    """accounts.cpp checkNewLogin, in short. None = fine."""
    if not 3 <= len(username) <= 20 or not all(c.isalnum() or c in "._-" for c in username):
        return "The username must be 3 to 20 letters, digits, '.', '_' or '-'"
    if not 6 <= len(password) <= 64:
        return "The password must have 6 to 64 characters"
    a = accounts.get(username)
    if a and not (a["role"] == "farmer" and a["user_id"] == user_id):
        return f"The username {username} is taken"
    return None


def set_farmer_login(user_id, username, password):
    for name in [n for n, a in accounts.items() if a["role"] == "farmer" and a["user_id"] == user_id]:
        del accounts[name]
        for sid in [s for s, n in sessions.items() if n == name]:
            del sessions[sid]                      # that farmer's phones log in again
    accounts[username] = {"role": "farmer", "user_id": user_id, "must_change": False} | make_hash(password)


def simulate():
    """A dose every 20 s: about 12 s of dosing, then DONE."""
    while True:
        time.sleep(0.2)
        t = (time.time() - started) % 20
        with lock:
            # noise of a few grams, plus the dose leaving the hoppers while dosing (25 / 70 / 5 %)
            for i, share in enumerate((0.25, 0.70, 0.05)):
                stock["g"][i] += random.uniform(-3, 3) - (share * 600 / 60 if t < 12 else 0)
                stock["g"][i] = max(0.0, stock["g"][i])
            now_dg = [round(g * 10) for g in stock["g"]]
            if any(abs(n - s) > HW_DEADBAND_G * 10 for n, s in zip(now_dg, stock["sent_dg"])):
                stock["sent_dg"] = now_dg
            if t < 12:
                state["flag"] = "D"
                state["weight_dg"] = int(t / 12 * 6000)
            elif state["flag"] == "D":
                actual = 6000 + random.randint(-30, 30)
                n = len(doses) + 1
                doses.append({"no": n, "boot": 1, "uptime_s": int(time.time() - started), "seq": n % 255 or 1,
                              "recipe": "ABC"[n % 3], "target_dg": 6000, "actual_dg": actual, "time": clock_text()})
                state.update(flag="I", weight_dg=actual, done_count=state["done_count"] + 1,
                             last={"seq": n, "recipe": "ABC"[n % 3], "target_dg": 6000, "actual_dg": actual})


def status():
    with lock:
        s = {"online": True, "weight_dg": state["weight_dg"], "flag": state["flag"], "last_frame_ms": 120,
             "done_count": state["done_count"], "dose_count": len(doses), "sd_ok": True, "fake_mega": True,
             "boot": 1, "uptime_s": int(time.time() - started), "heap_free": 180000, "heap_min": 150000, "clients": 1,
             "hoppers_dg": list(stock["sent_dg"]), "clock_set": clock["set"], "epoch": clock_now()}
        if state["last"]:
            s["last"] = state["last"]
        stock_rows = [stock_max_dose(pct, s["hoppers_dg"]) for pct in RECIPE_PCT]
        s["stock_max_g"] = [m for m, _ in stock_rows]
        s["stock_short"] = [k for _, k in stock_rows]
    return s


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def send_body(self, code, body, ctype="application/json", headers=()):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    # ---- logins, as web_server.cpp: the "sid" cookie, or ?sid= ----
    def session(self):
        sid = parse_qs(urlparse(self.path).query).get("sid", [""])[0]
        for part in self.headers.get("Cookie", "").split(";"):
            k, _, v = part.strip().partition("=")
            if k == "sid" and not sid:
                sid = v
        name = sessions.get(sid)
        return {"username": name, "sid": sid} | accounts[name] if name in accounts else None

    def need(self, admin=False):
        s = self.session()
        if s is None:
            self.send_json({"ok": False, "error": "Log in first"}, 401)
        elif s["must_change"]:
            self.send_json({"ok": False, "error": "Choose a new admin password first"}, 403)
        elif admin and s["role"] != "admin":
            self.send_json({"ok": False, "error": "Only the admin can do this"}, 403)
        else:
            return s
        return None

    def may_use(self, s, user_id):
        if s["role"] == "admin" or str(s["user_id"]) == str(user_id):
            return True
        self.send_json({"ok": False, "error": "Those are another farmer's records"}, 403)
        return False

    def send_me(self, s, cookie=None):
        me = {"logged_in": False} if s is None else {
            "logged_in": True, "username": s["username"], "role": s["role"], "user_id": s["user_id"],
            "must_change": s["must_change"], "sid": s["sid"]}
        self.send_body(200, json.dumps(me), headers=[("Set-Cookie", cookie)] if cookie else ())

    def send_csv(self, lines, name):
        data = ("\n".join(lines) + "\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/csv")
        self.send_header("Content-Disposition", f'attachment; filename="{name}"')
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_json(self, obj, code=200):
        self.send_body(code, json.dumps(obj))

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        if url.path == "/ws":
            return self.websocket()
        if url.path in TYPES:
            name, ctype = TYPES[url.path]
            return self.send_body(200, (WWW / name).read_bytes(), ctype)
        if url.path == "/api/status":
            return self.send_json(status())
        if url.path == "/api/doses":
            limit = max(1, min(200, int(q.get("limit", 20))))
            with lock:
                return self.send_json({"total": len(doses), "items": list(reversed(doses))[:limit]})
        if url.path == "/api/doses.csv":
            lines = ["dose_no,boot,uptime_s,seq,recipe,target_g,actual_g,error_g,error_pct,date_time"]
            with lock:
                for d in doses:
                    e = d["actual_dg"] - d["target_dg"]
                    lines.append(f'{d["no"]},{d["boot"]},{d["uptime_s"]},{d["seq"]},{d["recipe"]},'
                                 f'{d["target_dg"] / 10:.1f},{d["actual_dg"] / 10:.1f},{e / 10:.1f},{e * 100 / d["target_dg"]:.2f},{d["time"]}')
            return self.send_body(200, "\n".join(lines) + "\n", "text/csv")
        if url.path == "/api/record-types":
            return self.send_json(RECORD_TYPES)
        if url.path == "/api/me":
            return self.send_me(self.session())
        # everything below needs a login
        s = self.need(admin=url.path in ("/api/users.csv", "/api/logins"))
        if s is None:
            return None
        if url.path == "/api/users":
            return self.send_json([u for u in users if s["role"] == "admin" or u["id"] == s["user_id"]])
        if url.path == "/api/users.csv":
            return self.send_csv(["user_id,name"] + [f'{u["id"]},{u["name"]}' for u in users], "agridose_farmers.csv")
        if url.path == "/api/logins":
            return self.send_json([{"user_id": a["user_id"], "username": n} for n, a in accounts.items() if a["role"] == "farmer"])
        if url.path.startswith("/api/records/") and url.path.endswith(".csv"):
            kind = url.path.rsplit("/", 1)[1][:-4]
            spec = next((t for t in RECORD_TYPES if t["type"] == kind), None)
            if spec is None:
                return self.send_json({"ok": False, "error": "Not found"}, 404)
            names = [f["name"] for f in spec["fields"]]
            rows = [r for r in records[kind] if s["role"] == "admin" or r["user_id"] == str(s["user_id"])]
            lines = [",".join(["user_id"] + names)] + [",".join([r["user_id"]] + [r[n] for n in names]) for r in rows]
            return self.send_csv(lines, f"agridose_{kind}.csv")
        if url.path == "/api/photo":
            p = photos.get(int(q.get("id", 0) or 0))
            if not p:
                return self.send_json({"ok": False, "error": "Photo not found"}, 404)
            if not self.may_use(s, p["user_id"]):
                return None
            data, ctype = p["full" if q.get("size") == "full" else "compressed"]
            return self.send_body(200, data, ctype)
        if url.path.startswith("/api/records/"):
            if not self.may_use(s, q.get("user_id")):
                return None
            kind = url.path.rsplit("/", 1)[1]
            # row = line number in the file (the header is line 0), rev = checksum of the line, as on the ESP32
            mine = [{"row": i + 1, "rev": line_rev(csv_line(kind, r))} | r
                    for i, r in enumerate(records.get(kind, [])) if r["user_id"] == q.get("user_id")]
            limit = int(q.get("limit", 3)) or 100
            items = [{k: v for k, v in r.items() if k != "user_id"} for r in reversed(mine)][:limit]
            return self.send_json({"type": kind, "user_id": int(q.get("user_id", 0)), "total": len(mine), "items": items})
        self.send_json({"ok": False, "error": "Not found"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)
        path = urlparse(self.path).path
        form = {} if path == "/api/photos" else {k: v[0] for k, v in parse_qs(raw.decode()).items()}
        # ---- the clock (web_server.cpp handleClock, rules as clock_rules.h) ----
        if path == "/api/clock":
            try:
                epoch, tz = int(form["epoch"]), int(form["tz_min"])
            except (KeyError, ValueError):
                return self.send_json({"ok": False, "error": "Send epoch and tz_min"}, 400)
            if not (CLOCK_MIN_EPOCH <= epoch <= CLOCK_MAX_EPOCH) or not (-720 <= tz <= 840):
                return self.send_json({"ok": False, "error": "That time or time zone can't be right"}, 400)
            s = self.session()
            logged_in = s is not None and not s["must_change"]
            with lock:
                take = not clock["set"] or (logged_in and abs(epoch - clock_now()) > CLOCK_SLACK_S)
                if take:
                    clock.update(set=True, epoch=epoch, at=time.time(), tz_min=tz)
                return self.send_json({"ok": True, "set": take, "epoch": clock_now()})
        # ---- logins ----
        if path == "/api/login":
            name = form.get("username", "").strip().lower()
            if name not in accounts or not password_ok(accounts[name], form.get("password", "")):
                return self.send_json({"ok": False, "error": "Wrong username or password"}, 401)
            sid = os.urandom(16).hex()
            sessions[sid] = name
            return self.send_me({"username": name, "sid": sid} | accounts[name],
                                f"sid={sid}; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200")
        if path == "/api/logout":
            s = self.session()
            sessions.pop(s["sid"] if s else "", None)
            return self.send_me(None, "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0")
        if path == "/api/account/password":
            s = self.session()                     # also with the first admin password
            if s is None:
                return self.send_json({"ok": False, "error": "Log in first"}, 401)
            new = form.get("new", "")
            if not password_ok(accounts[s["username"]], form.get("old", "")):
                return self.send_json({"ok": False, "error": "The current password is wrong"}, 400)
            if not 6 <= len(new) <= 64 or new == form.get("old"):
                return self.send_json({"ok": False, "error": "The new password must have 6 to 64 characters and be new"}, 400)
            accounts[s["username"]] |= make_hash(new) | {"must_change": False}
            return self.send_me(self.session())
        # everything below needs a login; farmers and their logins: the admin only
        s = self.need(admin=path in ("/api/users", "/api/edit/user", "/api/farmer-login", "/api/delete/user"))
        if s is None:
            return None
        if path == "/api/photos":
            if not self.may_use(s, parse_qs(urlparse(self.path).query).get("user_id", [""])[0]):
                return None
            return self.photo_upload(raw)
        if path == "/api/delete/user":
            uid = int(form.get("user_id", 0) or 0)
            if not any(u["id"] == uid for u in users):
                return self.send_json({"ok": False, "error": f"User ID {uid} is not registered"}, 400)
            for name in [n for n, a in accounts.items() if a["role"] == "farmer" and a["user_id"] == uid]:
                del accounts[name]
            for kind in records:
                records[kind] = [r for r in records[kind] if r["user_id"] != str(uid)]
            for pid in [p for p, v in photos.items() if v["user_id"] == str(uid)]:
                del photos[pid]
            users[:] = [u for u in users if u["id"] != uid]
            return self.send_json({"ok": True})
        if path == "/api/farmer-login":
            uid = int(form.get("user_id", 0) or 0)
            name = form.get("username", "").strip().lower()
            err = login_error(name, form.get("password", ""), uid)
            if err:
                return self.send_json({"ok": False, "error": err}, 400)
            set_farmer_login(uid, name, form["password"])
            return self.send_json({"ok": True})
        if path == "/api/users":
            if not form.get("user_id") or not form.get("name"):
                return self.send_json({"ok": False, "error": "User ID and name are required"}, 400)
            uid = int(form["user_id"])
            name = form.get("username", "").strip().lower()
            err = (name and login_error(name, form.get("password", ""), uid)) or name_error(form["name"])
            if err:
                return self.send_json({"ok": False, "error": err}, 400)
            users.append({"id": uid, "name": form["name"].strip()})
            if name:
                set_farmer_login(uid, name, form["password"])
            return self.send_json({"ok": True})
        if path == "/api/edit/user":
            u = next((u for u in users if str(u["id"]) == form.get("user_id")), None)
            if u is None or not form.get("name"):
                return self.send_json({"ok": False, "error": "User ID is not registered, or the name is empty"}, 400)
            err = name_error(form["name"], u["id"])
            if err:
                return self.send_json({"ok": False, "error": err}, 400)
            u["name"] = form["name"].strip()
            return self.send_json({"ok": True})
        if (path.startswith("/api/edit/") or path.startswith("/api/records/") or path.startswith("/api/delete/")) and \
                not self.may_use(s, form.get("user_id")):
            return None
        if path.startswith("/api/delete/"):
            # one record and its photos (crop_records.cpp cropDeleteRecord, 1 Nov 2026)
            kind = path.rsplit("/", 1)[1]
            if kind not in records:
                return self.send_json({"ok": False, "error": "Not found"}, 404)
            i = int(form.get("row", 0) or 0) - 1
            rows = records[kind]
            if not (0 <= i < len(rows)) or rows[i]["user_id"] != form.get("user_id") or \
                    line_rev(csv_line(kind, rows[i])) != form.get("rev"):
                return self.send_json({"ok": False, "error": "This record was changed since the list was loaded. "
                                                             "Close it and open it again."}, 400)
            rec = rows[i]
            if kind == "planting":
                same_plot = lambda r: r["user_id"] == rec["user_id"] and r["plot"].lower() == rec["plot"].lower()
                growth = sum(1 for r in records["growth"] if same_plot(r))
                others = sum(1 for j, r in enumerate(rows) if j != i and same_plot(r))
                if growth and not others:
                    return self.send_json({"ok": False, "error": f'Plot {rec["plot"]} has {growth} growth record(s) '
                                                                 "from this planting. Delete them first."}, 400)
            for pid in (rec.get("photos") or "").split():
                if pid.isdigit() and int(pid) in photos and photos[int(pid)]["user_id"] == rec["user_id"]:
                    del photos[int(pid)]
            del rows[i]
            return self.send_json({"ok": True})
        if path.startswith("/api/edit/"):
            kind = path.rsplit("/", 1)[1]
            spec = next((t for t in RECORD_TYPES if t["type"] == kind), None)
            if spec is None:
                return self.send_json({"ok": False, "error": "Not found"}, 404)
            i = int(form.get("row", 0) or 0) - 1
            rows = records[kind]
            if not (0 <= i < len(rows)) or rows[i]["user_id"] != form.get("user_id") or \
                    line_rev(csv_line(kind, rows[i])) != form.get("rev"):
                return self.send_json({"ok": False, "error": "This record was changed since the list was loaded. "
                                                             "Close it and open it again."}, 400)
            rec = {"user_id": form["user_id"]} | {f["name"]: form.get(f["name"], "") for f in spec["fields"]}
            err = record_error(kind, spec, rec, form.get("today", ""), skip=i)
            if err:
                return self.send_json({"ok": False, "error": err}, 400)
            rows[i] = rec
            return self.send_json({"ok": True})
        if path.startswith("/api/records/"):
            kind = path.rsplit("/", 1)[1]
            spec = next((t for t in RECORD_TYPES if t["type"] == kind), None)
            if spec is None:
                return self.send_json({"ok": False, "error": "Not found"}, 404)
            rec = {"user_id": form.get("user_id", "")} | {f["name"]: form.get(f["name"], "") for f in spec["fields"]}
            err = record_error(kind, spec, rec, form.get("today", ""))
            if err:
                return self.send_json({"ok": False, "error": err}, 400)
            records[kind].append(rec)
            return self.send_json({"ok": True})
        self.send_json({"ok": False, "error": "Not found"}, 404)

    def photo_upload(self, raw):
        """Two files, compressed.jpg then full.jpg/png/webp, like crop_photos.cpp expects."""
        msg = BytesParser(policy=email_policy).parsebytes(
            b"Content-Type: " + self.headers.get("Content-Type", "").encode() + b"\r\n\r\n" + raw)
        names = [part.get_filename() for part in msg.iter_parts()]
        if len(names) != 2 or names[0] != "compressed.jpg" or not (names[1] or "").startswith("full."):
            return self.send_json({"ok": False, "error": f"Unexpected files in the upload: {names}"}, 400)
        parts = list(msg.iter_parts())
        new_id = len(photos) + 1
        photos[new_id] = {"compressed": (parts[0].get_payload(decode=True), "image/jpeg"),
                          "full": (parts[1].get_payload(decode=True), parts[1].get_content_type()),
                          "user_id": parse_qs(urlparse(self.path).query).get("user_id", [""])[0]}
        print(f"photo {new_id}: full {len(photos[new_id]['full'][0])} bytes, compressed {len(photos[new_id]['compressed'][0])} bytes")
        return self.send_json({"ok": True, "id": new_id})

    def websocket(self):
        """Minimal WebSocket: handshake, then a status message twice a second."""
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        try:
            while True:
                data = json.dumps(status()).encode()
                header = bytes([0x81, len(data)]) if len(data) < 126 else bytes([0x81, 126]) + len(data).to_bytes(2, "big")
                self.wfile.write(header + data)
                self.wfile.flush()
                time.sleep(0.5)
        except OSError:
            pass                                   # the browser closed the page


if __name__ == "__main__":
    threading.Thread(target=simulate, daemon=True).start()
    print(f"Mock ESP32 on http://127.0.0.1:{PORT}/  (Ctrl+C to stop)")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
