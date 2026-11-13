#!/usr/bin/env python
"""Cuts the power on the REAL AgriDose_Mega_Link sketch, on a laptop.

arduino-cli gives the sketch with its prototypes, g++ builds it against the
pretend Arduino in tests/mega_sim, and every power-on is one process, so the
sketch's RAM is really gone in between. Only the EEPROM bytes and the hoppers
carry over. The hoppers are a model (feed rates from the bench notes), so this
checks the firmware's logic, not the accuracy of the real machine.

Run from the project folder (first run about 10 s to build, then about 1 minute):
    python tests/test_power_cut.py
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKETCH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "AgriDose_Mega_Link")  # another copy can be named, to test a change
SIM_DIR = os.path.join(ROOT, "tests", "mega_sim")
CLI = r"C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
GPP = r"C:\msys64\ucrt64\bin\g++.exe"
SKETCHBOOK = r"C:\Users\melwi\OneDrive\Documents\Arduino"

# Recipe A, 600 g. Order: hull, then ash, then compost.
TARGET = [150.0, 420.0, 30.0]
FILL = [500.0, 900.0, 300.0]
DOSE = "A,600,7"
TOLERANCE_G = 7.0     # each hopper, against the model: see the no-cut run printed below

failures = 0


def check(ok, what):
    global failures
    if not ok:
        failures += 1
    print(("PASS  " if ok else "FAIL  ") + what)


def build(tmp):
    env = dict(os.environ, ARDUINO_DIRECTORIES_USER=SKETCHBOOK)
    src = subprocess.run([CLI, "compile", "--preprocess", "-b", "arduino:avr:mega", SKETCH],
                         capture_output=True, text=True, env=env, check=True).stdout
    with open(os.path.join(tmp, "sim_sketch.cpp"), "w", encoding="utf-8") as f:
        f.write(src)
    exe = os.path.join(tmp, "sim.exe")
    env = dict(os.environ, PATH=r"C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + os.environ["PATH"])
    # -w: the sketch's own warnings are for the real compiler, not this one.
    subprocess.run([GPP, "-std=c++11", "-w", "-O1", "-DARDUINO=10819", "-I", SIM_DIR, "-I", SKETCH, "-I", tmp,
                    os.path.join(SIM_DIR, "sim_main.cpp"), "-o", exe, "-static"], env=env, check=True)
    return exe


class Machine:
    """One simulated machine: its EEPROM and hoppers live in a file."""

    def __init__(self, exe, path):
        self.exe, self.path = exe, path

    def power_on(self, *args):
        r = subprocess.run([self.exe, self.path] + [str(a) for a in args], capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, r.stderr
        return json.loads(r.stdout)

    def fresh(self):
        """A new board: first power-on with empty hoppers, then filled."""
        if os.path.exists(self.path):
            os.remove(self.path)
        self.power_on("--run", 1000)
        self.set_stock(FILL)

    def world(self):
        with open(self.path, "rb") as f:
            return bytearray(f.read())

    def set_stock(self, grams, hopper=None):
        data = self.world()
        off = 4096  # World: ee[4096], then stock[3] of doubles
        if hopper is None:
            for i in range(3):
                struct.pack_into("<d", data, off + 8 * i, grams[i])
        else:
            old = struct.unpack_from("<d", data, off + 8 * hopper)[0]
            struct.pack_into("<d", data, off + 8 * hopper, old + grams)
        with open(self.path, "wb") as f:
            f.write(data)


def worst_error(out):
    return max(abs(out[i] - TARGET[i]) for i in range(3))


def main():
    tmp = tempfile.mkdtemp(prefix="agridose_sim_")
    try:
        exe = build(tmp)
        m = Machine(exe, os.path.join(tmp, "world.bin"))

        # ---- first power-on, and the saved zero ----
        if os.path.exists(m.path):
            os.remove(m.path)
        first = m.power_on("--run", 1000)
        check(not first["haveSaved"] and first["zeroed"] == [1, 1, 1], "first power-on: nothing saved, every hopper is zeroed")
        m.set_stock(FILL)
        second = m.power_on("--run", 3000)
        check(second["haveSaved"] and second["zeroed"] == [1, 1, 1], "second power-on: the saved zero is used")
        check(second["haveHw"] and all(abs(second["hw"][i] / 10 - FILL[i]) < 3 for i in range(3)),
              "loaded hoppers read their true weight after a power-off: %s g" % [h / 10 for h in second["hw"]])
        check(second["flag"] == "I" and not second["motorBeforeAnswer"], "no dose saved: idle, and no auger moves")

        # ---- a dose with no cut, to see how close the model gets ----
        m.fresh()
        base = m.power_on("--dose", DOSE, "--end-idle")
        print("      no cut: out %s g, DONE %s" % (base["out"], base["dones"]))
        check(worst_error(base["out"]) <= TOLERANCE_G, "a dose with no cut lands on target")
        check(len(base["dones"]) == 1, "it reports DONE once")
        check(all(base["learned"]), "it learns each hopper's stop-early: %s" % base["stopEarly"])
        again = m.power_on("--run", 1000)
        check(all(again["learned"]) and all(abs(again["stopEarly"][i] - base["stopEarly"][i]) < 0.01 for i in range(3)),
              "the learned stop-early grams survive a power-off")
        check(again["flag"] == "I" and again["dones"] == [], "after a finished dose: idle, and no DONE sent again")

        # ---- the sweep: a cut at each moment of a dose, then # on the keypad ----
        # A run of the dose takes about 60 s. Steps of 193 ms land all over the
        # save, the weighing, the settling and the top-ups.
        cuts = list(range(100, 66000, 193))
        moved, wrong, doneBad, noPrompt, worst, finishedAtCut, reportedBad = [], [], [], [], 0.0, 0, []
        for n, cut in enumerate(cuts):
            m.fresh()
            a = m.power_on("--dose", DOSE, "--cut", cut, "--seed", n + 1)
            assert a["cut"], "the dose ended before the cut at %d ms" % cut
            b = m.power_on("--answer", "resume", "--end-idle", "--seed", n + 1000)
            if b["motorBeforeAnswer"]:
                moved.append(cut)
            if "R" not in b["flags"]:
                # Only right if the dose had already ended at the cut, or had not begun.
                if a["dones"] or sum(a["out"]) == 0:
                    finishedAtCut += 1
                else:
                    noPrompt.append(cut)
            if sum(a["out"]) > 0 or "R" in b["flags"]:
                e = worst_error(b["out"])
                worst = max(worst, e)
                if e > TOLERANCE_G:
                    wrong.append((cut, b["out"]))
            alld = a["dones"] + b["dones"]
            seen = {d[0] for d in alld}
            if sum(b["out"]) > 0 and not (len(seen) == 1 and len(b["dones"]) <= 1):
                doneBad.append((cut, alld))
            if alld and abs(alld[-1][3] / 10 - sum(b["out"])) > 8:
                reportedBad.append((cut, alld[-1][3] / 10, sum(b["out"])))
        print("      %d cuts; worst hopper error after resuming %.1f g; %d cut before the first auger or after the end" %
              (len(cuts), worst, finishedAtCut))
        check(not moved, "no auger moves at power-on before the operator answers, at any cut: %s" % moved[:5])
        check(not noPrompt, "the LCD question appears whenever a dose was cut off: %s" % noPrompt[:5])
        check(not wrong, "after # every hopper still lands on target, at every cut: %s" % wrong[:3])
        check(not doneBad, "one dose, one DONE, at every cut: %s" % doneBad[:3])
        check(not reportedBad, "the mass reported matches what really came out, at every cut: %s" % reportedBad[:3])

        # ---- the sweep again, answering * ----
        wrong, moved = [], []
        for n, cut in enumerate(range(300, 64000, 2111)):
            m.fresh()
            a = m.power_on("--dose", DOSE, "--cut", cut, "--seed", n + 1)
            b = m.power_on("--answer", "stop", "--end-idle", "--seed", n + 500)
            if b["motorBeforeAnswer"]:
                moved.append(cut)
            if abs(sum(a["out"]) - sum(b["out"])) > 0.01:
                moved.append(cut)
            if "R" in b["flags"]:
                got = b["dones"][-1][3] / 10 if b["dones"] else None
                if got is None or abs(got - sum(b["out"])) > 8:
                    wrong.append((cut, got, sum(b["out"])))
        check(not moved, "* at the question: no auger runs at all")
        check(not wrong, "* at the question: the log gets what really came out: %s" % wrong[:3])

        # ---- cut, resume, cut again ----
        wrong = []
        for n, (c1, c2) in enumerate([(5000, 4000), (21000, 500), (21000, 12000), (30000, 8000), (40000, 1500), (55000, 900), (2000, 2000), (25000, 25000)]):
            m.fresh()
            m.power_on("--dose", DOSE, "--cut", c1, "--seed", n + 1)
            m.power_on("--answer", "resume", "--cut", c2, "--seed", n + 100)
            c = m.power_on("--answer", "resume", "--end-idle", "--seed", n + 200)
            if worst_error(c["out"]) > TOLERANCE_G or len(c["dones"]) != 1:
                wrong.append((c1, c2, c["out"], c["dones"]))
        check(not wrong, "cut twice in one dose: still on target, one DONE: %s" % wrong[:3])

        # ---- the power goes on and off with nobody answering ----
        m.fresh()
        a = m.power_on("--dose", DOSE, "--cut", 30000)
        r1 = m.power_on("--run", 4000)
        r2 = m.power_on("--run", 4000)
        check(r1["flag"] == "R" and r2["flag"] == "R" and abs(sum(r1["out"]) - sum(a["out"])) < 0.01 and abs(sum(r2["out"]) - sum(a["out"])) < 0.01,
              "unanswered, it keeps asking through more power-offs, and nothing moves")
        check(r1["lcdR"] == r2["lcdR"] and r1["lcdR"][0].startswith("RESUME A 600g?"), "the same question both times: %s" % r1["lcdR"])
        print("      the LCD asks: |%s|%s|" % tuple(r1["lcdR"]))
        check(abs(int(r1["lcdR"][1].split("g")[0]) - sum(a["out"])) < 6, "it shows the grams that really came out (%.1f g)" % sum(a["out"]))
        c = m.power_on("--answer", "resume", "--end-idle")
        check(worst_error(c["out"]) <= TOLERANCE_G, "and # still finishes the dose")

        # ---- the zero moved a little across the power cut ----
        wrong = []
        for drift in (-3, 3):
            for cut in (9000, 25000, 45000):
                m.fresh()
                m.power_on("--dose", DOSE, "--cut", cut)
                b = m.power_on("--answer", "resume", "--end-idle", "--drift", drift)
                if worst_error(b["out"]) > TOLERANCE_G + abs(drift):
                    wrong.append((drift, cut, b["out"]))
        check(not wrong, "a zero 3 g off after the cut costs at most those 3 g: %s" % wrong[:3])

        # ---- material added or taken out while the power was off ----
        m.fresh()
        m.power_on("--dose", DOSE, "--cut", 8000)      # in hopper A
        m.set_stock(200.0, hopper=0)
        r = m.power_on("--run", 4000)
        check(r["flag"] == "R" and r["lcdR"][0].startswith("WEIGHT CHANGED!"), "200 g added while off: the LCD warns: %s" % r["lcdR"])
        check(not r["motorBeforeAnswer"], "and no auger moves")
        c = m.power_on("--answer", "resume", "--end-idle")
        print("      refilled, then resumed anyway: out %s g (saved grams at most 5 s old)" % c["out"])
        check(c["out"][0] <= TARGET[0] + 5 * 8 + TOLERANCE_G, "resuming anyway overshoots by no more than 5 s of feed")

        m.fresh()
        m.power_on("--dose", DOSE, "--cut", 8000)
        m.set_stock(-300.0, hopper=0)
        r = m.power_on("--run", 4000)
        check(r["lcdR"][0].startswith("WEIGHT CHANGED!"), "300 g taken out while off: the LCD warns")

        # ---- a finished hopper stays finished ----
        # Cuts just after hopper A ends, before the next auger starts. Hopper A is
        # refilled while off: since it is finished, that must not matter.
        hits, wrong = 0, []
        for cut in range(19000, 24000, 97):
            m.fresh()
            a = m.power_on("--dose", DOSE, "--cut", cut)
            if a["out"][2] > 0:
                break
            m.set_stock(200.0, hopper=0)
            r = m.power_on("--answer", "resume", "--end-idle")
            if r["lcdR"][0].startswith("RESUME A"):
                hits += 1
                if abs(r["out"][0] - a["out"][0]) > 0.01 or abs(r["out"][1] - TARGET[1]) > TOLERANCE_G:
                    wrong.append((cut, a["out"], r["out"]))
        check(hits > 0 and not wrong, "a cut after a hopper finished: it does not run again (%d cuts landed there): %s" % (hits, wrong[:2]))

        # ---- a fault pauses the dose, the LCD says what to check, # continues ----
        def on_target(out):
            return worst_error(out) <= TOLERANCE_G

        def low_stock(hopper, grams):
            m.fresh()
            s = list(FILL)
            s[hopper] = grams
            m.set_stock(s)

        # Hopper A runs out at 80 g of 150 g, the operator refills it and presses #.
        low_stock(0, 80.0)
        r = m.power_on("--dose", DOSE, "--answer", "resume", "--fix-at-fault", 1, "--refill", 300, "--end-idle")
        print("      ran out, refilled, #: out %s g, DONE %s" % (r["out"], r["dones"]))
        check("F" in r["flags"], "a hopper that runs out gives flag F")
        check(r["donesAtFirstAnswer"] == 0, "the dose is only paused: no DONE until the operator answers")
        check(not r["motorInFault"], "no auger is driven during the pause")
        for text in ("EMPTY: HULL", "REFILL HOPPER?", "STUCK? TAP JUG", "MOTOR POWER ON?", "HANDS CLEAR?", "#=GO ON *=END"):
            check(any(text in sc for sc in r["screens"]), "the LCD shows: %s" % text)
        check(all(len(p) <= 16 for sc in r["screens"] for p in sc.split("|")), "every screen fits 16 columns: %s" % r["screens"])
        check(on_target(r["out"]), "after the refill and # every hopper lands on target")
        check(len(r["dones"]) == 1 and abs(r["dones"][0][3] / 10 - sum(r["out"])) <= 8,
              "one DONE, and it says what really came out: the refill is not counted")

        # The same, in the later hoppers: the finished ones don't run again.
        low_stock(2, 12.0)
        r = m.power_on("--dose", DOSE, "--answer", "resume", "--fix-at-fault", 1, "--refill", 100, "--end-idle")
        check(any("EMPTY: ASH" in sc for sc in r["screens"]) and on_target(r["out"]), "ash runs out, is refilled, #: on target, hull not run again: %s" % r["out"])
        low_stock(1, 100.0)
        r = m.power_on("--dose", DOSE, "--answer", "resume", "--fix-at-fault", 1, "--refill", 500, "--end-idle")
        check(any("EMPTY: COMPOST" in sc for sc in r["screens"]) and on_target(r["out"]), "compost runs out, is refilled, #: on target: %s" % r["out"])

        # One bad reading just before the fault must not carry over into the rest of the dose.
        low_stock(0, 80.0)
        r = m.power_on("--dose", DOSE, "--spike-before-fault", "--answer", "resume", "--fix-at-fault", 1, "--refill", 300, "--end-idle")
        check("F" in r["flags"] and on_target(r["out"]), "a bad reading just before the fault: the re-weigh keeps the grams right: %s" % r["out"])

        # [10 Nov] A hand on the jug (clearing a clog) while the fault re-weighs it: the
        # grams already dosed must be kept, not counted from zero again after #.
        for early in (False, True):
            low_stock(0, 80.0)
            args = ["--dose", DOSE, "--press", 120, "--answer", "resume", "--fix-at-fault", 1, "--refill", 300, "--end-idle"]
            if early:
                args.append("--press-early")
            r = m.power_on(*args)
            check("F" in r["flags"] and on_target(r["out"]) and len(r["dones"]) == 1 and abs(r["dones"][0][3] / 10 - sum(r["out"])) <= 8,
                  "a hand pushing on the jug %s: # carries on from the grams already out: out %s, DONE %s" %
                  ("while it is stuck and at the fault" if early else "at the fault", r["out"], r["dones"]))

        # * ends it: logged with what came out, and nothing else runs.
        low_stock(0, 80.0)
        r = m.power_on("--dose", DOSE, "--answer", "stop", "--end-idle")
        check(len(r["dones"]) == 1 and r["donesAtFirstAnswer"] == 0 and abs(r["dones"][0][3] / 10 - sum(r["out"])) <= 3 and r["out"][1] == 0 and r["out"][2] == 0,
              "* at a fault: one DONE with what came out, and the other hoppers never run: %s" % r["dones"])

        # Motor power off: the auger moves nothing, EMPTY after 15 s; power back on and #.
        m.fresh()
        r = m.power_on("--dose", DOSE, "--feed-scale", 0, "--answer", "resume", "--fix-at-fault", 1, "--end-idle")
        check("F" in r["flags"] and on_target(r["out"]), "motor power off, then on, then #: the dose finishes on target")

        # Nothing fixed: # gives the same fault again, as often as it takes, and * still works.
        m.fresh()
        r = m.power_on("--dose", DOSE, "--feed-scale", 0, "--answer", "resume,resume,stop", "--end-idle")
        check(r["flags"].count("F") == 3 and len(r["dones"]) == 1 and sum(r["out"]) == 0, "not fixed: the fault comes back on each #, and * ends it: %s" % r["flags"])
        check(not r["motorInFault"], "and no auger is driven during any pause")

        # SENSOR: the load cell stops answering, the wire is fixed, #.
        m.fresh()
        r = m.power_on("--dose", DOSE, "--sensor-dead", 6000, "--answer", "resume", "--fix-at-fault", 1, "--end-idle")
        check(any("SENSOR: HULL" in sc for sc in r["screens"]) and any("CHECK LOAD WIRES" in sc for sc in r["screens"]), "a dead load cell: the LCD says to check the wires")
        check(on_target(r["out"]) and len(r["dones"]) == 1, "wire fixed, #: on target (no reading for a moment costs little): %s" % r["out"])
        m.fresh()
        r = m.power_on("--dose", DOSE, "--sensor-dead", 6000, "--answer", "resume,stop", "--end-idle")
        check(r["flags"].count("F") == 2 and len(r["dones"]) == 1 and not r["motorInFault"], "wire not fixed: # can't start the auger blind, the fault comes back: %s" % r["flags"])

        # TIMEOUT: feeding far too slowly for 4 minutes.
        m.fresh()
        r = m.power_on("--dose", DOSE, "--feed-scale", 0.05, "--answer", "resume", "--fix-at-fault", 1, "--end-idle")
        check(any("TIMEOUT: HULL" in sc for sc in r["screens"]) and any("FEED TOO SLOW" in sc for sc in r["screens"]), "too slow for 4 minutes: TIMEOUT, and the LCD says so")
        check(on_target(r["out"]), "cause fixed, #: on target: %s" % r["out"])

        # The power goes during the pause, and the hopper is refilled while it is off.
        low_stock(0, 80.0)
        a = m.power_on("--dose", DOSE, "--cut-in-fault", 6000)
        check(a["cut"] and a["dones"] == [], "power cut during a pause: no DONE yet")
        m.set_stock(300.0, hopper=0)
        r = m.power_on("--answer", "resume", "--end-idle")
        print("      the LCD asks: |%s|%s|" % tuple(r["lcdR"]))
        check("R" in r["flags"] and r["lcdR"][0].startswith("RESUME A 600g?"), "at power-on it asks, with no false weight warning: %s" % r["lcdR"])
        check(not r["motorBeforeAnswer"], "and no auger moves until #")
        check(on_target(r["out"]) and len(r["dones"]) == 1, "then # finishes on target: the refill is not counted: %s" % r["out"])
        # And when stock is taken out during the pause, which a scale could not tell from dosing.
        low_stock(0, 80.0)
        a = m.power_on("--dose", DOSE, "--cut-in-fault", 6000)
        m.set_stock(300.0, hopper=0)
        m.power_on("--run", 3000)                       # still unanswered through another power-off
        r = m.power_on("--answer", "resume", "--end-idle")
        check(on_target(r["out"]), "asked twice, then #: still on target: %s" % r["out"])

        # A cut at many moments after a fault was answered. The fault comes at about
        # 27 s (15 s EMPTY window since 10 Nov) and the operator answers about 11 s later, so the cuts start at 40 s.
        wrong = []
        for n, cut in enumerate(range(300, 45000, 2903)):
            low_stock(0, 80.0)
            a = m.power_on("--dose", DOSE, "--answer", "resume", "--fix-at-fault", 1, "--refill", 300, "--cut", 40000 + cut, "--seed", n + 1)
            assert a["cut"] and "F" in a["flags"] and a["donesAtFirstAnswer"] == 0, "the cut came before the fault was answered: %s" % a
            b = m.power_on("--answer", "resume", "--end-idle", "--seed", n + 50)
            if not on_target(b["out"]) or b["motorBeforeAnswer"]:
                wrong.append((cut, b["out"]))
        check(not wrong, "a power cut after a fault was answered: still on target: %s" % wrong[:3])

        # ---- the ESP32 did not confirm the result before the power went ----
        m.fresh()
        a = m.power_on("--dose", DOSE, "--no-got", "--run", 90000)
        check(len(a["dones"]) == 1 and a["flag"] == "I", "the dose ends, the ESP32 is silent, DONE keeps repeating")
        b = m.power_on("--end-idle")
        check(b["dones"] == a["dones"], "at the next power-on the same DONE goes out again: %s" % b["dones"])
        check(not b["motorBeforeAnswer"] and "R" not in b["flags"], "it doesn't ask about a finished dose, and no auger moves")
        c = m.power_on("--run", 3000)
        check(c["dones"] == [], "once confirmed, it is not sent again")

        # ---- [10 Nov] draining a hopper (D on the keypad, then A, B or C, then #) ----
        def drain_screen(r, start):
            return next((sc for sc in r["drainScreens"] if sc.startswith(start)), "")

        def shown_g(screen):                            # "DRAIN STOPPED   |HULL 41g OUT    " -> 41
            return int(screen.split("|")[1].split()[1].rstrip("g"))

        m.fresh()                                       # FILL: hull 500, compost 900, ash 300 g
        r = m.power_on("--drain", "B,7", "--end-idle")
        print("      drain B: out %s g, screens %s" % (r["out"], r["drainScreens"]))
        check(r["replies"][:1] == ["ACK,7"] and "E" in r["flags"] and r["flag"] == "I", "DRAIN B: accepted, flag E while it runs, then idle: %s" % r["flags"])
        check(r["out"][1] > 899 and r["out"][0] == 0 and r["out"][2] == 0 and r["ran"] == [0, 1, 0],
              "it empties the compost hopper and runs no other auger: %s" % r["out"])
        check(r["dones"] == [] and r["wt"] == 0, "a drain is not a dose: no DONE (no log row), the WT mass is unchanged")
        check(not any(r["learned"]) and all(abs(x - 3.0) < 1e-6 for x in r["stopEarly"]), "a drain doesn't change the learned stop-early grams")
        check("|OUT " in drain_screen(r, "DRAINING COMP") and "*=STOP" in drain_screen(r, "DRAINING COMP"), "the LCD shows DRAINING COMP / OUT ...g *=STOP")
        sc = drain_screen(r, "NO FLOW: EMPTY?")
        check(sc and abs(shown_g(sc) - r["out"][1]) <= 3, "it stops by itself when nothing comes out, and shows the grams: |%s|" % sc)
        check(all(len(p) <= 16 for sc in r["drainScreens"] for p in sc.split("|")), "every drain screen fits 16 columns")

        m.fresh()
        r = m.power_on("--drain", "A,7", "--send", "6000:STOP,8", "--end-idle")
        sc = drain_screen(r, "DRAIN STOPPED")
        check(r["replies"][:2] == ["ACK,7", "ACK,8"] and 25 < r["out"][0] < 55 and sc and abs(shown_g(sc) - r["out"][0]) <= 3,
              "* stops a drain at once and shows what came out: %.1f g, |%s|" % (r["out"][0], sc))

        m.fresh()
        r = m.power_on("--drain", "C,7", "--send", "3000:DOSE,8,A,600", "--send", "3500:TARE,9", "--send", "4000:RESUME,10",
                       "--send", "5000:STOP,11", "--end-idle")
        check(r["replies"][1:4] == ["NAK,8,BUSY", "NAK,9,BUSY", "NAK,10,BUSY"] and r["dones"] == [] and r["ran"] == [0, 0, 1],
              "while draining, DOSE, TARE and RESUME are refused (BUSY): %s" % r["replies"])

        m.fresh()
        r = m.power_on("--dose", DOSE, "--send", "5000:DRAIN,8,B", "--end-idle")
        check("NAK,8,BUSY" in r["replies"] and on_target(r["out"]) and len(r["dones"]) == 1, "DRAIN during a dose is refused, and the dose lands on target")
        r = m.power_on("--drain", "X,9", "--run", 3000)
        check(r["replies"] == ["NAK,9,ARGS"] and r["ran"] == [0, 0, 0], "DRAIN of a hopper that isn't A, B or C is refused (ARGS)")

        m.fresh()
        r = m.power_on("--drain", "A,7", "--feed-scale", 0, "--end-idle")
        check(drain_screen(r, "NO FLOW: EMPTY?") != "" and r["out"][0] == 0, "a jammed auger: the drain stops after 15 s of no flow")
        m.fresh()
        r = m.power_on("--drain", "A,7", "--sensor-dead", 5000, "--end-idle")
        check(drain_screen(r, "DRAIN: NO SENSOR") != "" and r["out"][0] < 45, "a dead load cell stops the drain at once: %.1f g out" % r["out"][0])
        m.fresh()
        m.set_stock(10000.0 - FILL[0], hopper=0)        # 10 kg of hull: 8 g/s would take 21 min
        r = m.power_on("--drain", "A,7", "--end-idle")
        check(drain_screen(r, "DRAIN TIME UP") != "" and 4500 < r["out"][0] < 5000, "after 10 minutes it stops: DRAIN TIME UP, %.0f g out" % r["out"][0])

        m.fresh()
        a = m.power_on("--drain", "B,7", "--cut", 8000)
        b = m.power_on("--run", 5000)
        check(a["cut"] and 50 < a["out"][1] < 120 and b["flag"] == "I" and not b["motorBeforeAnswer"] and b["dones"] == [] and "R" not in b["flags"],
              "power cut while draining: at power-on it is idle, no auger moves, nothing to resume")

        m.fresh()
        r = m.power_on("--drain", "C,7", "--send", "3000:STOP,8", "--send", "6000:DOSE,9,A,600", "--end-idle")
        drained = r["drainScreens"] and shown_g(drain_screen(r, "DRAIN STOPPED"))
        dose_out = [r["out"][0], r["out"][1], r["out"][2] - drained]
        check(r["replies"][:3] == ["ACK,7", "ACK,8", "ACK,9"] and len(r["dones"]) == 1 and r["dones"][0][0] == 9 and worst_error(dose_out) <= TOLERANCE_G + 3,
              "a dose straight after a drain still lands on target: %s" % [round(x, 1) for x in dose_out])

        # ---- zeroing is remembered ----
        m.fresh()
        m.set_stock([0.0, 0.0, 0.0])
        d = m.power_on("--run", 2000)
        check(all(abs(h) <= 10 for h in d["hw"]), "empty hoppers read 0 g with the saved zero: %s" % d["hw"])
        # Only printed when every step above has run: a crash must never read as a pass.
        print("\n%s: %d failure(s)" % ("FAILED" if failures else "ALL PASSED", failures))
        return 0 if failures == 0 else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
