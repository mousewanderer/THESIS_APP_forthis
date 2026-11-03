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

        # ---- the ESP32 did not confirm the result before the power went ----
        m.fresh()
        a = m.power_on("--dose", DOSE, "--no-got", "--run", 90000)
        check(len(a["dones"]) == 1 and a["flag"] == "I", "the dose ends, the ESP32 is silent, DONE keeps repeating")
        b = m.power_on("--end-idle")
        check(b["dones"] == a["dones"], "at the next power-on the same DONE goes out again: %s" % b["dones"])
        check(not b["motorBeforeAnswer"] and "R" not in b["flags"], "it doesn't ask about a finished dose, and no auger moves")
        c = m.power_on("--run", 3000)
        check(c["dones"] == [], "once confirmed, it is not sent again")

        # ---- zeroing is remembered ----
        m.fresh()
        m.set_stock([0.0, 0.0, 0.0])
        d = m.power_on("--run", 2000)
        check(all(abs(h) <= 10 for h in d["hw"]), "empty hoppers read 0 g with the saved zero: %s" % d["hw"])
        return 0 if failures == 0 else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        print("\n%s: %d failure(s)" % ("FAILED" if failures else "ALL PASSED", failures))


if __name__ == "__main__":
    sys.exit(main())
