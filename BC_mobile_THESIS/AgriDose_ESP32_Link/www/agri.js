'use strict';
// AgriDose agronomy data and the recipe recommendation. This runs on the
// phone, not on the ESP32: the ESP32 only serves this file.
//
// Copied from the team's agridose-game spec (data/crops.json, recipes.json,
// config.json, maasin-rainfall.json and docs/03-recommendation-logic.md).
// Keep both in step. rankRecipes() is pure (no page, no network), so
// tests/test_recommend.mjs checks it against the 9 worked examples.

const AGRI = {
  // Demo values, tunable (docs/07-sources.md says which are CITED, DERIVED or DEMO).
  defaultPH: 5.5,
  fertDropPerApp: 0.10,
  fertTypes: {
    urea:     { label: 'Urea (46-0-0)',             factor: 1.0 },
    ammosul:  { label: 'Ammonium sulfate (21-0-0)', factor: 1.4 },
    complete: { label: 'Complete (14-14-14)',       factor: 0.4 },
  },
  rainDropPer100mm: 0.01,
  mildGap: 0.3,
  driftDropPH: 0.2,
  wetMonthMinMm: 150,
  doseMinG: 300,          // the real machine's range (AgriLink.h AGRILINK_DOSE_MIN_G/MAX_G), 300 to 900 g since 1 Nov 2026 (was 100 to 300)
  doseMaxG: 900,
};

// Crop soil pH ranges from Philippine DA production guides where available.
// verify: seen only in a secondary copy; check the original guide before citing.
// (Kept for the thesis citations; since 1 Nov 2026 the Crop Guide no longer shows a tag for it.)
const CROPS = [
  { name: 'Pechay',         local: 'Pechay',   phMin: 5.5, phMax: 6.5, source: 'DA-ATI VII, Pechay Production Guide (n.d.)', verify: false },
  { name: 'Rice (lowland)', local: 'Palay',    phMin: 5.5, phMax: 6.5, source: 'TNAU Agritech Portal (n.d.); no PhilRice/DA range found', verify: true },
  { name: 'Corn',           local: 'Mais',     phMin: 5.8, phMax: 7.0, source: 'Univ. of Arkansas, Corn Production Handbook MP437 (n.d.)', verify: false },
  { name: 'Tomato',         local: 'Kamatis',  phMin: 6.0, phMax: 6.8, source: 'PCAARRD guide as reported by agriculture.com.ph (2023); DA-RFO 02 (2017) gives 5.5-8.0', verify: true },
  { name: 'Eggplant',       local: 'Talong',   phMin: 5.5, phMax: 6.8, source: 'DA-RFO 02, Eggplant Production Guide (secondary copy)', verify: true },
  { name: 'Okra',           local: 'Okra',     phMin: 5.5, phMax: 7.0, source: 'DA-ATI VII, Okra Production Guide (n.d.)', verify: false },
  { name: 'Bitter gourd',   local: 'Ampalaya', phMin: 6.0, phMax: 6.7, source: 'DA-ATI CAR (2022); DA-RFO 02 Ampalaya guide', verify: false },
  { name: 'String bean',    local: 'Sitaw',    phMin: 5.5, phMax: 6.6, source: 'DA-RFO 02, Sitao Production Guide (2017)', verify: false },
  { name: 'Squash',         local: 'Kalabasa', phMin: 6.0, phMax: 6.7, source: 'Attributed to DA-BPI / DA-RFO II (secondary copies)', verify: true },
  { name: 'Cabbage',        local: 'Repolyo',  phMin: 6.0, phMax: 6.8, source: 'DA-ATI VII, Cabbage Production Guide (n.d.)', verify: false },
  { name: 'Water spinach',  local: 'Kangkong', phMin: 5.5, phMax: 6.5, source: 'DA-ATI VII, Kangkong Production Guide (2020)', verify: false },
  { name: 'Sweet potato',   local: 'Kamote',   phMin: 5.5, phMax: 6.5, source: 'DA-RFO 02, Sweet Potato Production Guide (2012)', verify: false },
];

// PAGASA climatological normals 1991-2020, Maasin, Southern Leyte.
const RAINFALL = [
  { name: 'January',   rainMm: 289.2 }, { name: 'February', rainMm: 198.2 }, { name: 'March',    rainMm: 142.7 },
  { name: 'April',     rainMm: 71.9 },  { name: 'May',      rainMm: 95.3 },  { name: 'June',     rainMm: 160.6 },
  { name: 'July',      rainMm: 209.6 }, { name: 'August',   rainMm: 202.6 }, { name: 'September', rainMm: 236.6 },
  { name: 'October',   rainMm: 232.8 }, { name: 'November', rainMm: 213.7 }, { name: 'December', rainMm: 285.6 },
];

// Fixed recipes, percent by mass. Same numbers as AGRI_RECIPES in AgriLink.h.
const RECIPES = {
  A: { hull: 25, compost: 70, ash: 5,  label: 'Compost-heavy maintenance blend' },
  B: { hull: 30, compost: 65, ash: 5,  label: 'More rice hull for water retention' },
  C: { hull: 30, compost: 60, ash: 10, label: 'Most wood ash, raises pH the most' },
};

function round1(x) { return Math.round(x * 10) / 10; }
function round2(x) { return Math.round(x * 100) / 100; }
function fmt1(x) { return x.toFixed(1); }
function fmt2(x) { return x.toFixed(2); }

// "Tomato", "tomato", "Kamatis" or "Tomato (Kamatis)" -> the Tomato entry.
function cropFor(text) {
  const t = String(text || '').trim().toLowerCase();
  return CROPS.find((c) => t === c.name.toLowerCase() || t === c.local.toLowerCase() ||
    t === (c.name + ' (' + c.local + ')').toLowerCase()) || null;
}
function cropLabel(text) {
  const c = cropFor(text);
  return c ? (c.local === c.name ? c.name : c.name + ' (' + c.local + ')') : String(text || '');
}

function seasonForMonth(month) { return RAINFALL[month - 1].rainMm >= AGRI.wetMonthMinMm ? 'wet' : 'dry'; }

// Days between two YYYY-MM-DD dates (to - from).
function daysBetween(fromIso, toIso) {
  return Math.round((Date.UTC(...isoParts(toIso)) - Date.UTC(...isoParts(fromIso))) / 86400000);
}
function isoParts(iso) { const [y, m, d] = iso.split('-').map(Number); return [y, m - 1, d]; }

// Rain to expect from fromIso up to (not including) toIso, from the Maasin
// normals spread evenly over each month. A starting value the farmer can change.
function rainEstimateMm(fromIso, toIso) {
  let mm = 0;
  const day = new Date(Date.UTC(...isoParts(fromIso)));
  const end = Date.UTC(...isoParts(toIso));
  for (let n = 0; day.getTime() < end && n < 3660; n++) {
    const days = new Date(Date.UTC(day.getUTCFullYear(), day.getUTCMonth() + 1, 0)).getUTCDate();
    mm += RAINFALL[day.getUTCMonth()].rainMm / days;
    day.setUTCDate(day.getUTCDate() + 1);
  }
  return Math.round(mm);
}

// A dose split like agriSplitDose() in AgriLink.h, in decigrams (1 g = 10 dg):
// rice hull and wood ash rounded, compost gets the rest, so they add up exactly.
function splitDoseDg(recipeKey, totalDg) {
  const r = RECIPES[recipeKey];
  const hull = Math.floor((totalDg * r.hull + 50) / 100);
  const ash = Math.floor((totalDg * r.ash + 50) / 100);
  return { hull, ash, compost: totalDg - hull - ash };
}

const SHORT_REASON = {
  A: 'Compost-heavy maintenance blend.',
  B: 'More rice hull for water retention.',
  C: 'Most wood ash. Raises pH the most.',
};
const NOT_ADVISED_C = 'Not advised: soil is already above range and wood ash raises pH further.';

function fertName(type) { return AGRI.fertTypes[type].label.split(' (')[0].toLowerCase(); }   // "ammonium sulfate"

// A fertilizer record's name -> its key in AGRI.fertTypes: "Urea (46-0-0)" -> "urea".
// null for a fertilizer the docs give no pH factor for (it is listed, not counted).
function fertKey(text) {
  const t = String(text || '').trim().toLowerCase();
  if (t.startsWith('urea')) return 'urea';
  if (t.startsWith('ammonium sulfate') || t.startsWith('ammosul')) return 'ammosul';
  if (t.startsWith('complete') || t.startsWith('14-14-14')) return 'complete';
  return null;
}

// docs/03. input: { crop: { name, phMin, phMax }, soilTestPH (or null), soilTestDate,
//   fertSinceTest (list of AGRI.fertTypes keys), rainMmSinceTest, month (1 to 12) }
// Every value is rounded before it is compared, so floating-point error cannot flip a rule.
function rankRecipes(input) {
  const { crop, soilTestPH, soilTestDate, fertSinceTest, rainMmSinceTest, month } = input;
  const noTest = soilTestPH === null || soilTestPH === undefined;
  const basePH = noTest ? AGRI.defaultPH : soilTestPH;
  const fertUnits = round2(fertSinceTest.reduce((sum, t) => sum + AGRI.fertTypes[t].factor, 0));
  const fertDrop = round2(AGRI.fertDropPerApp * fertUnits);
  const rainMm = rainMmSinceTest;
  const rainDrop = round2((AGRI.rainDropPer100mm * rainMm) / 100);
  const drop = round2(fertDrop + rainDrop);
  const estimatedPH = round2(basePH - drop);
  const gap = round2(crop.phMin - estimatedPH);
  const normal = RAINFALL[month - 1];
  const season = seasonForMonth(month);
  const est = fmt2(estimatedPH), min = fmt1(crop.phMin), max = fmt1(crop.phMax);

  let rule, rec, order, reason;
  if (estimatedPH > crop.phMax) {
    rule = 1; rec = 'A'; order = ['B', 'C'];
    reason = `Soil pH ${est} is above ${crop.name}'s maximum of ${max}. All three recipes contain wood ash, so A has the mildest effect. Consider a smaller batch.`;
  } else if (gap > AGRI.mildGap) {
    rule = 2; rec = 'C'; order = ['B', 'A'];
    reason = `Soil pH ${est} is ${fmt2(gap)} below ${crop.name}'s minimum of ${min}. C has the most wood ash for liming.`;
  } else if (gap > 0 || drop >= AGRI.driftDropPH) {
    rule = 3; rec = 'B'; order = ['C', 'A'];
    reason = gap > 0
      ? `Soil pH ${est} is slightly below ${crop.name}'s minimum of ${min}. B gives mild correction with more rice hull.`
      : `Fertilizer and rain since your last soil test may have lowered pH by about ${fmt2(drop)}. B gives mild correction.`;
  } else if (season === 'dry') {
    rule = 4; rec = 'B'; order = ['A', 'C'];
    reason = `Soil pH ${est} is within ${crop.name}'s range (${min} to ${max}). Dry month at Maasin, so B adds rice hull for water retention.`;
  } else {
    rule = 5; rec = 'A'; order = ['B', 'C'];
    reason = `Soil pH ${est} is within ${crop.name}'s range (${min} to ${max}). Wet month at Maasin, so A keeps it balanced with compost.`;
  }

  const others = order.map((r) => (rule === 1 && r === 'C'
    ? { recipe: r, reason: NOT_ADVISED_C, notAdvised: true }
    : { recipe: r, reason: SHORT_REASON[r], notAdvised: false }));

  const fertList = fertSinceTest.length
    ? fertSinceTest.map((t) => `${fertName(t)} (${fmt1(AGRI.fertTypes[t].factor)})`).join(' + ')
    : 'none';
  const steps = [
    noTest ? `No soil test yet. Default pH ${fmt1(basePH)} for Leyte soils` : `Latest soil test: ${fmt1(basePH)}` + (soilTestDate ? ` on ${soilTestDate}` : ''),
    `Fertilizer since test: ${fertList} = ${fmt1(fertUnits)} units x ${fmt2(AGRI.fertDropPerApp)} = -${fmt2(fertDrop)}`,
    `Rain since test: ${round1(rainMm)} mm x ${fmt2(AGRI.rainDropPer100mm)} per 100 mm = -${fmt2(rainDrop)}`,
    `Estimated pH: ${fmt1(basePH)} - ${fmt2(drop)} = ${est}`,
    `${crop.name} range: ${min} to ${max}`,
    `Gap: ${min} - ${est} = ${fmt2(gap)}`,
    `${normal.name} at Maasin: ${fmt1(normal.rainMm)} mm normal, ${season}`,
    `Rule ${rule} fired`,
  ];

  return {
    basePH, noTest, fertUnits, fertDrop, rainMm, rainDrop, drop, estimatedPH, gap, season, rule,
    recommended: { recipe: rec, reason }, others, steps,
  };
}
