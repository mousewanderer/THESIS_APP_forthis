// Laptop test for the dashboard's recommendation (AgriDose_ESP32_Link/www/agri.js)
// against the 9 worked examples in agridose-game docs/03-recommendation-logic.md,
// plus the dose split against agriSplitDose() in AgriLink.h.
//
// Run from the project folder (Node 18 or newer):
//   node tests/test_recommend.mjs
import fs from "node:fs";
import vm from "node:vm";

const src = fs.readFileSync(new URL("../AgriDose_ESP32_Link/www/agri.js", import.meta.url), "utf8");
const ctx = vm.createContext({});
vm.runInContext(src + "\n;globalThis.api = { rankRecipes, cropFor, splitDoseDg, rainEstimateMm, daysBetween, fertKey };", ctx);
const { rankRecipes, cropFor, splitDoseDg, rainEstimateMm, daysBetween, fertKey } = ctx.api;

let failures = 0;
function check(ok, what) {
  console.log(`${ok ? "PASS" : "FAIL"}  ${what}`);
  if (!ok) failures++;
}

const EXAMPLES = [
  { n: 1, crop: "Pechay", soil: 5.0, fert: [], rain: 0, month: 9, est: 5.0, season: "wet", rule: 2, rec: "C", others: "B,A" },
  { n: 2, crop: "Tomato", soil: 6.1, fert: ["urea", "ammosul"], rain: 300, month: 10, est: 5.83, season: "wet", rule: 3, rec: "B", others: "C,A" },
  { n: 3, crop: "Pechay", soil: 6.0, fert: [], rain: 100, month: 4, est: 5.99, season: "dry", rule: 4, rec: "B", others: "A,C" },
  { n: 4, crop: "Pechay", soil: 6.0, fert: [], rain: 100, month: 9, est: 5.99, season: "wet", rule: 5, rec: "A", others: "B,C" },
  { n: 5, crop: "Sweet potato", soil: 7.0, fert: [], rain: 0, month: 9, est: 7.0, season: "wet", rule: 1, rec: "A", others: "B,C", notAdvised: "C" },
  { n: 6, crop: "Sweet potato", soil: 6.8, fert: ["urea", "urea"], rain: 400, month: 9, est: 6.56, season: "wet", rule: 1, rec: "A", others: "B,C", notAdvised: "C" },
  { n: 7, crop: "Pechay", soil: null, fert: [], rain: 200, month: 9, est: 5.48, season: "wet", rule: 3, rec: "B", others: "C,A", noTest: true },
  { n: 8, crop: "Cabbage", soil: 6.6, fert: ["complete", "complete", "complete"], rain: 1000, month: 11, est: 6.38, season: "wet", rule: 3, rec: "B", others: "C,A" },
  { n: 9, crop: "Pechay", soil: 6.0, fert: [], rain: 100, month: 12, est: 5.99, season: "wet", rule: 5, rec: "A", others: "B,C" },
];

for (const ex of EXAMPLES) {
  const r = rankRecipes({ crop: cropFor(ex.crop), soilTestPH: ex.soil, soilTestDate: null,
    fertSinceTest: ex.fert, rainMmSinceTest: ex.rain, month: ex.month });
  const others = r.others.map((o) => o.recipe).join(",");
  const notAdvised = r.others.filter((o) => o.notAdvised).map((o) => o.recipe).join(",");
  const ok = r.rule === ex.rule && r.recommended.recipe === ex.rec && others === ex.others && r.season === ex.season &&
    r.estimatedPH === ex.est && notAdvised === (ex.notAdvised || "") && r.noTest === !!ex.noTest;
  check(ok, `example ${ex.n}: pH ${r.estimatedPH}, ${r.season}, rule ${r.rule}, recommended ${r.recommended.recipe}, others ${others}`);
}

// AgriLink.h: hull and ash rounded to the nearest dg, compost gets the rest.
for (const [recipe, dg, hull, ash, compost] of [["A", 2500, 625, 125, 1750], ["B", 1000, 300, 50, 650], ["C", 2503, 751, 250, 1502]]) {
  const s = splitDoseDg(recipe, dg);
  check(s.hull === hull && s.ash === ash && s.compost === compost && s.hull + s.ash + s.compost === dg,
    `split ${recipe} ${dg} dg -> hull ${s.hull}, ash ${s.ash}, compost ${s.compost}`);
}

check(cropFor("kamatis")?.name === "Tomato" && cropFor("Tomato (Kamatis)")?.name === "Tomato" && cropFor("Mango") === null,
  "crop names: local name and label find the crop, unknown crops do not");
check(fertKey("Urea (46-0-0)") === "urea" && fertKey("Ammonium sulfate (21-0-0)") === "ammosul" &&
  fertKey("Complete (14-14-14)") === "complete" && fertKey("Vermicast") === null,
  "fertilizer names from crop_records.cpp map to the docs/03 factors; others are not counted");
check(daysBetween("2026-09-03", "2026-09-10") === 7 && daysBetween("2026-12-31", "2027-01-01") === 1, "days between dates");
check(rainEstimateMm("2026-09-01", "2026-10-01") === 237 && rainEstimateMm("2026-09-10", "2026-09-10") === 0,
  "rain estimate: all of September is its normal (236.6 mm)");

console.log(`\n${failures ? "FAILED" : "ALL PASSED"}: ${failures} failure(s)`);
process.exit(failures ? 1 : 0);
