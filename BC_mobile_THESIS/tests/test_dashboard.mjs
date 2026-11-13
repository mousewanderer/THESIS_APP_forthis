// Browser test for the dashboard page (AgriDose_ESP32_Link/www) at phone size.
// Starts tools/mock_esp32.py and headless Edge, clicks through every tab, adds
// a soil test with a photo and a growth record, edits both and the farmer's
// name, and checks the Recommend tab.
//
// Run from the project folder (Node 18 or newer, Python, Microsoft Edge):
//   node tests/test_dashboard.mjs
// Another Chromium browser: set EDGE_PATH. Screenshots: %TEMP%\agridose-dashboard-shots
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const OUT = path.join(os.tmpdir(), "agridose-dashboard-shots");
const EDGE = process.env.EDGE_PATH || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
const PORT = 9333, MOCK_PORT = 8766;
fs.rmSync(OUT, { recursive: true, force: true });
fs.mkdirSync(OUT + "/shots", { recursive: true });
const mock = spawn("python", [fileURLToPath(new URL("../AgriDose_ESP32_Link/tools/mock_esp32.py", import.meta.url)), String(MOCK_PORT)], { stdio: "ignore" });
const edge = spawn(EDGE, ["--headless=new", `--remote-debugging-port=${PORT}`, `--user-data-dir=${OUT}/edge-profile`,
  "--no-first-run", "--window-size=390,844", "about:blank"], { stdio: "ignore" });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let targets = null;
for (let i = 0; i < 40 && !targets; i++) {
  await sleep(250);
  try { targets = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json(); } catch (_) {}
}
const page = targets.find((t) => t.type === "page");
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener("open", r, { once: true }));
let id = 0;
const pending = new Map();
const logs = [];
ws.addEventListener("message", (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); return; }
  if (msg.method === "Runtime.consoleAPICalled") logs.push(msg.params.type + ": " + msg.params.args.map((a) => a.value ?? a.description).join(" "));
  if (msg.method === "Runtime.exceptionThrown") logs.push("EXCEPTION: " + (msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text));
  if (msg.method === "Log.entryAdded" && msg.params.entry.level === "error") logs.push("LOG: " + msg.params.entry.text + " " + (msg.params.entry.url || ""));
});
const send = (method, params = {}) => new Promise((resolve, reject) => {
  const i = ++id;
  pending.set(i, (m) => (m.error ? reject(new Error(method + ": " + m.error.message)) : resolve(m.result)));
  ws.send(JSON.stringify({ id: i, method, params }));
});
async function js(expr) {
  const r = await send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error("eval failed: " + expr.slice(0, 100) + " -> " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
  return r.result.value;
}
async function shot(name) {
  const r = await send("Page.captureScreenshot", { format: "png" });
  fs.writeFileSync(`${OUT}/shots/${name}.png`, Buffer.from(r.data, "base64"));
}
let failures = 0;
function check(ok, what) { console.log((ok ? "PASS  " : "FAIL  ") + what); if (!ok) failures++; }
const noSideScroll = () => js("document.documentElement.scrollWidth <= window.innerWidth && document.querySelector('.ph-body').scrollWidth <= document.querySelector('.ph-body').clientWidth");
const tab = async (name) => { await js(`document.querySelector('#tabs [data-tab=${name}]').click()`); await sleep(500); };

try {
  await send("Runtime.enable");
  await send("Log.enable");
  await send("Page.enable");
  await send("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await send("Emulation.setTouchEmulationEnabled", { enabled: true });
  await send("Page.navigate", { url: `http://127.0.0.1:${MOCK_PORT}/` });
  // wait until the page has drawn its tabs (a cold Edge or mock can take several seconds)
  for (let i = 0; i < 40 && !(await js("!!document.querySelector('#tabs .on')").catch(() => false)); i++) await sleep(250);
  await sleep(1000);

  check(await js("document.querySelector('#tabs .on').dataset.tab") === "machine", "opens on the Machine tab");
  check(/Dosing|Idle/.test(await js("document.querySelector('.status').textContent")), "live status shows the mock's state");
  check(await js("document.getElementById('dot').classList.contains('on')"), "online dot is lime");
  // Flag R (power cut during a dose): set by hand, because the mock never has one. It is put back at once.
  const resume = await js("(() => { const was = S.live.flag; S.live.flag = 'R'; updateMachine(); const r = { status: document.querySelector('.status').textContent, cls: document.querySelector('.status').className, alert: alerts().join(' ') }; S.live.flag = was; updateMachine(); return r; })()");
  check(/Waiting/.test(resume.status) && /Unfinished dose/.test(resume.status) && /fault/.test(resume.cls) && /power went off/.test(resume.alert), "flag R: the status and the bell tell the operator to answer at the keypad");
  const paused = await js("(() => { const was = S.live.flag; S.live.flag = 'F'; updateMachine(); const r = { status: document.querySelector('.status').textContent, alert: alerts().join(' ') }; S.live.flag = was; updateMachine(); return r; })()");
  check(/Fault/.test(paused.status) && /# or \*/.test(paused.status) && /paused/.test(paused.alert) && /press # on the keypad to continue/.test(paused.alert), "flag F: the status and the bell say the dose is paused and how to continue");
  // [10 Nov] Flag E: a hopper is being drained from the keypad's D menu. Set by hand, put back at once.
  const draining = await js("(() => { const was = S.live.flag; S.live.flag = 'E'; updateMachine(); const r = { status: document.querySelector('.status').textContent, cls: document.querySelector('.status').className, alert: alerts().length }; S.live.flag = was; updateMachine(); return r; })()");
  check(/Draining/.test(draining.status) && /keypad D/.test(draining.status) && /busy/.test(draining.cls), "flag E: the status says a hopper is being drained: " + JSON.stringify(draining));
  const hg = await js("({ vals: [...document.querySelectorAll('.grid3 .gval')].map(t => t.textContent + t.nextSibling.textContent), total: document.querySelector('.total').textContent })");
  check(hg.vals.length === 3 && hg.vals.every(v => /^\d+(\.\d\d)?(g|kg)$/.test(v)) && /^\d/.test(hg.total), "hopper A/B/C gauges and total weight: " + JSON.stringify(hg));
  // [10 Nov] Enough stock for: the mock's hoppers (4.2 / 3.6 / 0.9 kg) cover 900 g of every recipe.
  const stock = await js("[...document.querySelectorAll('.stock dd')].map(d => d.textContent)");
  check(stock.length === 3 && stock.every(t => t === "up to 900 g"), "enough stock: every recipe up to 900 g with full hoppers: " + JSON.stringify(stock));
  // Low compost and too little ash: set by hand, because the mock's hoppers stay full. Put back at once.
  const low = await js("(() => { const was = [S.live.stock_max_g, S.live.stock_short]; S.live.stock_max_g = [557, 900, 0]; S.live.stock_short = [2, 0, 4]; updateMachine(); const r = [...document.querySelectorAll('.stock dd')].map(d => ({ t: d.textContent, short: d.classList.contains('short') })); [S.live.stock_max_g, S.live.stock_short] = was; updateMachine(); return r; })()");
  check(low[0].t === "up to 557 g: compost (Hopper B) is low" && !low[0].short && low[1].t === "up to 900 g" &&
        low[2].t === "not even 300 g: refill wood ash (Hopper C)" && low[2].short, "low stock: says how much and which hopper to refill: " + JSON.stringify(low));
  // [10 Nov] The bell: a hopper under 200 g is a low alert. Full hoppers give none.
  check(await js("!alerts().some(a => / is low: /.test(a))"), "full hoppers: no low-hopper alert under the bell");
  // Compost at 150 g, ash below zero (a jug lifted off), hull at exactly 200 g: set by hand, put back at once.
  const lowBell = await js("(() => { const was = S.live.hoppers_dg; S.live.hoppers_dg = [2000, 1509, -30]; updateHeader(); const r = { list: alerts().filter(a => / is low: /.test(a)), badge: !document.getElementById('badge').hidden }; document.getElementById('bell').click(); r.sheet = [...document.querySelectorAll('#sheet .note')].map(n => n.textContent); S.live.hoppers_dg = was; updateHeader(); return r; })()");
  check(lowBell.list.length === 2 && lowBell.list[0] === "Hopper B (compost) is low: 150 g left, under 200 g. Refill it before the next dose." &&
        lowBell.list[1].startsWith("Hopper C (wood ash) is low: 0 g left") && lowBell.badge &&
        lowBell.sheet.some(t => t.startsWith("Hopper B (compost) is low")), "low hoppers: the bell gets its red dot and names each hopper under 200 g: " + JSON.stringify(lowBell));
  await js("document.getElementById('sheet-close').click()");
  await sleep(300);
  await shot("1-machine");
  check(await noSideScroll(), "machine: no sideways scroll");

  // logins: the machine is public (above, logged out); records are not
  const MOCK = `http://127.0.0.1:${MOCK_PORT}`;
  const api = async (path, fields, sid) => {
    const res = await fetch(MOCK + path, { method: fields ? "POST" : "GET", body: fields ? new URLSearchParams(fields) : undefined,
      headers: sid ? { Cookie: "sid=" + sid } : {} });
    return { status: res.status, body: await res.json() };
  };
  check(await js("document.getElementById('who').textContent") === "Log in", "logged out: the header says Log in");
  check((await api("/api/records/soil?user_id=2")).status === 401, "records without a login: 401");
  check((await api("/api/status")).status === 200, "machine status without a login: 200");

  // the clock: the ESP32 has none, so the page gave it this phone's time when it opened
  const st = (await api("/api/status")).body;
  check(st.clock_set === true && Math.abs(st.epoch - Date.now() / 1000) < 5, "the page set the ESP32's clock from the phone");
  check(/clock \d\d:\d\d/.test(await js("document.querySelector('.ph-body').textContent")), "the Machine tab shows the ESP32's clock");
  const stray = await api("/api/clock", { epoch: String(Math.round(Date.now() / 1000) + 3600), tz_min: "480" });
  check(stray.status === 200 && stray.body.set === false && Math.abs((await api("/api/status")).body.epoch - Date.now() / 1000) < 5,
    "a phone that isn't logged in can't move a clock that is already set");
  check((await api("/api/clock", { epoch: "315532800", tz_min: "480" })).status === 400, "a 1980 time (dead phone clock) is refused");
  await tab("records");
  check(await js("!!document.querySelector('#body input[type=password]')") && await js("document.querySelectorAll('.rec').length") === 0,
    "Records tab shows a login form and no records");
  await shot("1b-records-logged-out");
  const wrong = await api("/api/login", { username: "admin", password: "nope-nope" });
  check(wrong.status === 401 && wrong.body.error === "Wrong username or password", "a wrong password is refused");

  // the admin's first login must choose a new password before anything else
  await js("document.getElementById('who').click()");
  await sleep(300);
  await js("{ const i = document.querySelectorAll('#sheet input'); i[0].value = 'Admin '; i[1].value = 'agridose-admin'; document.querySelector('#sheet .savebtn').click(); }");
  await sleep(1500);
  check(await js("document.getElementById('sheet-title').textContent").then((t) => t.startsWith("Choose a new password")), "first admin login asks for a new password");
  const adminSid = await js("S.me.sid");
  check((await api("/api/users", null, adminSid)).status === 403, "with the first password nothing else is allowed (403)");
  await shot("1c-new-password");
  await js("{ const i = document.querySelectorAll('#sheet input[type=password]'); i[0].value = 'agridose-admin'; i[1].value = 'admin-pass-2026'; i[2].value = 'admin-pass-2026'; document.querySelector('#sheet .savebtn').click(); }");
  await sleep(1500);
  check(await js("document.getElementById('who').textContent") === "Choose a farmer", "new admin password saved: choose a farmer");
  await tab("machine");

  // choose Rosa
  await js("document.getElementById('who').click()");
  await sleep(300);
  await shot("2-farmer-sheet");
  await js("[...document.querySelectorAll('.choice')].find(b => b.textContent.includes('Rosa')).click()");
  await sleep(1200);
  check(await js("document.getElementById('who').textContent") === "Rosa · ID 2", "header shows Rosa · ID 2");

  await tab("records");
  await sleep(500);
  const titles = await js("[...document.querySelectorAll('.rec b')].map(b => b.textContent)");
  check(titles[0] === "pH 5.8 · moisture 34%", "newest soil test first: " + titles[0]);
  check(titles.includes("Tomato (Kamatis) · NativeAgriDose"), "planting row with local name and AgriDose pill");
  check(titles.includes("Seedling · day 7"), "growth row: stage and day");
  await shot("3-records");
  check(await noSideScroll(), "records: no sideways scroll");
  check(titles.includes("Urea (46-0-0) · 0.5 kg"), "fertilizer applications are listed");

  // Bisaya lines (on by default), the CSV downloads, and sun mode
  check(await js("document.querySelector('#tabs [data-tab=recommend] .bis')?.textContent") === "Tambag", "Bisaya line under the tab names");
  await js("document.getElementById('lang').click()");
  await sleep(200);
  const left = await js("[...document.querySelectorAll('.bis')].map(b => b.parentElement.id || b.parentElement.className + ':' + b.textContent)");
  check(left.length === 0, "Bisaya button hides every Bisaya line" + (left.length ? ": " + left.join(", ") : ""));
  await js("document.getElementById('lang').click()");
  await sleep(200);
  check(await js("document.querySelectorAll('.dlrow a').length") === 5, "CSV download for each record type and the farmers");
  check((await js("fetch('/api/records/soil.csv').then(r => r.text())")).startsWith("user_id,plot,date_tested"), "soil CSV downloads");
  await js("document.getElementById('sun').click()");
  await sleep(200);
  check(await js("document.documentElement.dataset.theme === 'light' && getComputedStyle(document.querySelector('.app')).backgroundColor === 'rgb(255, 255, 255)'"), "sun mode: white screen");
  await shot("3b-records-sun");
  await js("document.getElementById('sun').click()");
  await sleep(200);
  check(await js("!document.documentElement.dataset.theme"), "sun mode off again");

  // add a soil test with a photo
  await js("document.querySelectorAll('.addbtn')[0].click()");
  await sleep(300);
  await js("document.querySelectorAll('.step')[1].click()");   // plot +
  await js("document.querySelectorAll('.step')[1].click()");
  await js("document.querySelectorAll('.step')[1].click()");   // starts at 1 (newest soil test's plot), then 2, 3
  check(await js("document.getElementById('f-soil_ph').placeholder") === "5.8", "pH placeholder shows the last reading");
  await js("document.querySelectorAll('.step')[3].click()");   // pH: first tap fills in 5.8
  await js("document.querySelectorAll('.step')[3].click()");   // then 5.9
  const formVals = await js("({ plot: document.getElementById('f-plot').value, ph: document.getElementById('f-soil_ph').value, date: document.getElementById('f-date_tested').value })");
  check(formVals.plot === "3" && formVals.ph === "5.9", "steppers: plot " + formVals.plot + ", pH " + formVals.ph);
  // a 2400 x 1800 test photo made on a canvas
  await js(`(async () => {
    const c = document.createElement('canvas'); c.width = 2400; c.height = 1800;
    const g = c.getContext('2d'); g.fillStyle = '#3a7d2c'; g.fillRect(0, 0, 2400, 1800);
    g.fillStyle = '#d2f53c'; g.beginPath(); g.arc(1200, 900, 600, 0, 7); g.fill();
    const blob = await new Promise(r => c.toBlob(r, 'image/jpeg', 0.95));
    const dt = new DataTransfer(); dt.items.add(new File([blob], 'plant.jpg', { type: 'image/jpeg' }));
    const input = document.querySelectorAll('.photobtn input')[1];
    input.files = dt.files; input.dispatchEvent(new Event('change'));
  })()`);
  await sleep(1500);
  const ready = await js("document.querySelector('.fld.wide .formhint').textContent");
  check(/^Ready: original .* compressed copy/.test(ready), "photo prepared: " + ready);
  check(await js("document.querySelectorAll('.thumbs .thumb').length") === 1, "one thumbnail in the form");
  await shot("4-soil-form");
  await js("document.querySelector('#sheet .savebtn').click()");
  await sleep(2500);
  check(await js("document.getElementById('sheet').hidden"), "sheet closed after saving");
  const first = await js("document.querySelector('.rec b').textContent");
  check(first.startsWith("pH 5.9"), "new soil test listed first: " + first);
  check(await js("!!document.querySelector('.rec .rthumb') && document.querySelector('.rec .rthumb').naturalWidth === 1024"), "row thumbnail is the 1024 px compressed copy");
  await shot("5-records-after-save");

  await js("document.querySelector('.rec .vd').click()");
  await sleep(600);
  check(await js("[...document.querySelectorAll('#sheet-body .fields dd')].some(d => d.textContent === '5.9')"), "details sheet shows the fields");
  await shot("6-details");
  await js("document.querySelector('#sheet-body .thumb').click()");
  await sleep(600);
  check(await js("!document.getElementById('viewer').hidden && document.getElementById('viewer-full').href.includes('size=full')"), "viewer open with save links");
  await shot("7-viewer");
  await js("document.getElementById('viewer-close').click()");
  await sleep(200);
  await js("document.getElementById('sheet-close').click()");
  await sleep(300);
  check(await js("document.getElementById('sheet').hidden && document.getElementById('viewer').hidden"), "closed sheet and viewer");

  // edit the soil test just saved: the form opens filled in, the photo stays, no record is added
  const soilCount = await js("document.querySelector('.rgroup .gright span').textContent");
  await js("document.querySelector('.rec .vd').click()");
  await sleep(400);
  await js("[...document.querySelectorAll('#sheet-body button')].find(b => b.textContent.startsWith('Edit')).click()");
  await sleep(300);
  const ef = await js("({ title: document.getElementById('sheet-title').textContent, plot: document.getElementById('f-plot').value, ph: document.getElementById('f-soil_ph').value, thumbs: document.querySelectorAll('#sheet .thumbs .thumb').length, button: document.querySelector('#sheet .savebtn').textContent })");
  check(ef.title.startsWith("Edit soil test") && ef.plot === "3" && ef.ph === "5.9" && ef.thumbs === 1 && ef.button.startsWith("Save changes"),
    "edit form filled in: " + JSON.stringify(ef));
  await js("document.getElementById('f-soil_ph').value = '6.2'");
  await shot("6b-edit-form");
  await js("document.querySelector('#sheet .savebtn').click()");
  await sleep(1500);
  check(await js("document.getElementById('sheet').hidden"), "sheet closed after the edit");
  const edited = await js("document.querySelector('.rec b').textContent");
  check(edited.startsWith("pH 6.2"), "edited soil test shows the new pH: " + edited);
  check(await js("document.querySelector('.rgroup .gright span').textContent") === soilCount, "editing did not add a record (" + soilCount + ")");
  check(await js("!!document.querySelector('.rec .rthumb')"), "the photo stayed with the edited record");
  // sent from here, not the page: the browser would log the expected 400 as a console error
  const stale = (await api("/api/edit/soil", { user_id: "2", row: "2", rev: "00000000", plot: "1", date_tested: "2026-09-01", soil_ph: "7" }, adminSid)).body;
  check(stale.ok === false && /changed since/.test(stale.error), "an edit with an old rev is refused: " + stale.error);

  // growth form: planting picks the plot, date planted and day count
  await js("document.querySelectorAll('.addbtn')[2].click()");
  await sleep(300);
  const g = await js("({ planting: document.getElementById('f-plot').selectedOptions[0].textContent, planted: document.getElementById('f-date_planted').value, days: document.getElementById('f-days_after_planting').value, hint: document.querySelector('#sheet .formhint').textContent })");
  check(g.planted === "2026-09-03" && g.days !== "", "growth form: " + JSON.stringify(g));
  await js("{ const s = document.querySelector('#sheet select:not(#f-plot)'); s.value = 'Vegetative'; s.dispatchEvent(new Event('change')); }");
  await shot("8-growth-form");
  await js("document.querySelector('#sheet .savebtn').click()");
  await sleep(1500);
  check(await js("[...document.querySelectorAll('.rec b')].some(b => b.textContent.startsWith('Vegetative · day'))"), "growth record saved with stage and day count");

  // edit that growth record: its planting and stage are picked, the stage can change
  await js("[...document.querySelectorAll('.rec')].find(r => r.querySelector('b').textContent.startsWith('Vegetative')).querySelector('.vd').click()");
  await sleep(400);
  await js("[...document.querySelectorAll('#sheet-body button')].find(b => b.textContent.startsWith('Edit')).click()");
  await sleep(300);
  const ge = await js("({ plot: document.getElementById('f-plot').value, stage: document.querySelector('#sheet select:not(#f-plot)').value, planted: document.getElementById('f-date_planted').value })");
  check(ge.plot === "1" && ge.stage === "Vegetative" && ge.planted === "2026-09-03", "growth edit form filled in: " + JSON.stringify(ge));
  await js("{ const s = document.querySelector('#sheet select:not(#f-plot)'); s.value = 'Flowering'; s.dispatchEvent(new Event('change')); }");
  await js("document.querySelector('#sheet .savebtn').click()");
  await sleep(1500);
  check(await js("[...document.querySelectorAll('.rec b')].some(b => b.textContent.startsWith('Flowering · day')) && ![...document.querySelectorAll('.rec b')].some(b => b.textContent.startsWith('Vegetative'))"),
    "growth record changed to Flowering, not added");

  // delete (1 Nov): that growth record, from its details, after a second tap
  const farmer = await js("S.user");
  const countOf = async (type) => (await api(`/api/records/${type}?user_id=${farmer}&limit=100`, null, adminSid)).body.total;
  const growthBefore = await countOf("growth");
  await js("[...document.querySelectorAll('.rec')].find(r => r.querySelector('b').textContent.startsWith('Flowering')).querySelector('.vd').click()");
  await sleep(400);
  check(await js("!!document.querySelector('#sheet .delrec .linkbtn') && document.querySelector('#sheet .delrec .dangerbtn').hidden"),
    "record details have a Delete button that asks first");
  await js("document.querySelector('#sheet .delrec .linkbtn').click()");
  await sleep(200);
  await shot("8c-delete-ask");
  await js("document.querySelector('#sheet .delrec .dangerbtn').click()");
  await sleep(1500);
  check(await countOf("growth") === growthBefore - 1 && !(await js("[...document.querySelectorAll('.rec b')].some(b => b.textContent.startsWith('Flowering'))")),
    "the growth record is deleted and gone from the list");
  // a planting whose plot still has growth records can't go; an old rev is refused
  const plantingList = (await api(`/api/records/planting?user_id=${farmer}&limit=100`, null, adminSid)).body.items;
  const grown = plantingList.find((p) => p.plot === "1");
  const kept = await api("/api/delete/planting", { user_id: farmer, row: String(grown.row), rev: grown.rev }, adminSid);
  check(kept.status === 400 && /growth record\(s\) from this planting\. Delete them first/.test(kept.body.error),
    "a planting with growth records is not deleted: " + kept.body.error);
  const staleDel = await api("/api/delete/planting", { user_id: farmer, row: String(grown.row), rev: "00000000" }, adminSid);
  check(staleDel.status === 400 && /changed since the list was loaded/.test(staleDel.body.error), "a delete with an old rev is refused");

  // data rules: a second soil test on the same plot and day is refused in the form, with the reason
  await js("document.querySelectorAll('.addbtn')[0].click()");
  await sleep(300);
  check(await js("document.getElementById('f-date_tested').max") === await js("today()"), "date boxes stop at today");
  await js("document.getElementById('f-plot').value = '3'; document.getElementById('f-soil_ph').value = '6.0'");
  await js("document.querySelector('#sheet .savebtn').click()");
  await sleep(1200);
  const dupMsg = await js("document.querySelector('#sheet .formmsg').textContent");
  check(/already has a soil test with Plot number 3 and Soil test date/.test(dupMsg), "duplicate soil test refused: " + dupMsg);
  await shot("8b-duplicate-refused");
  await js("document.getElementById('sheet-close').click()");
  await sleep(300);
  // the other rules, straight to the server
  const post = async (path, fields) => (await api(path, fields, adminSid)).body;
  const future = await post("/api/records/soil", { user_id: "2", plot: "5", date_tested: "2099-01-01", soil_ph: "6", today: "2026-10-30" });
  check(/can't be after today/.test(future.error), "a date after today is refused: " + future.error);
  const orphan = await post("/api/records/growth", { user_id: "2", plot: "9", stage: "Seedling", date_observed: "2026-10-01", today: "2026-10-30" });
  check(/Plot 9 has no planting/.test(orphan.error), "growth on a plot with no planting is refused: " + orphan.error);
  const early = await post("/api/records/growth", { user_id: "2", plot: "1", stage: "Seedling", date_observed: "2026-09-01", date_planted: "2026-09-03", today: "2026-10-30" });
  check(/can't be before the date planted/.test(early.error), "growth observed before planting is refused: " + early.error);
  const dose = await post("/api/records/planting", { user_id: "2", plot: "4", crop: "Corn", date_planted: "2026-10-01", used_agridose: "yes", dose_numbers: "2 999", today: "2026-10-30" });
  check(/Dose 999 is not in the Batch Log/.test(dose.error), "a dose number not in the Batch Log is refused: " + dose.error);
  const twin = await post("/api/users", { user_id: "3", name: "  JUAN " });
  check(/already called/.test(twin.error), "a second farmer called Juan is refused: " + twin.error);

  // fix the farmer's name: the ID stays
  await js("document.getElementById('who').click()");
  await sleep(300);
  check(await js("document.getElementById('f-rename').value") === "Rosa", "rename box starts with the current name");
  await js("document.getElementById('f-rename').value = 'Rosa Cruz'");
  await js("[...document.querySelectorAll('#sheet .savebtn')].find(b => b.textContent.startsWith('Save name')).click()");
  await sleep(1200);
  check(await js("document.getElementById('who').textContent") === "Rosa Cruz · ID 2", "header shows the corrected name");

  await tab("recommend");
  const rec = await js("document.querySelector('.reccard.main .r')?.textContent");
  check(rec === "Recipe B", "Rosa plot 1 (Tomato, pH 5.8): " + rec);
  check(await js("[...document.querySelectorAll('.why li')].length") === 8, "Why? lists 8 steps");
  check(await js("[...document.querySelectorAll('.why li')][1].textContent.startsWith('Fertilizer since test: none')"), "urea from before the soil test is not counted");
  check(await js("document.querySelector('.keys').textContent") === "#B#600##", "keypad steps for Recipe B, 600 g");
  await shot("9-recommend");
  check(await noSideScroll(), "recommend: no sideways scroll");
  // + Add fertilizer: the form opens on this plot; saving it changes the recommendation
  await js("[...document.querySelectorAll('.addbtn')].find(b => b.textContent.startsWith('+ Add fertilizer')).click()");
  await sleep(300);
  check(await js("document.getElementById('f-plot').value") === "1", "fertilizer form starts on plot 1");
  await js("{ const s = document.querySelector('#sheet select'); s.value = 'Urea (46-0-0)'; s.dispatchEvent(new Event('change')); }")
  await shot("9b-fertilizer-form");
  await js("document.querySelector('#sheet .savebtn').click()");
  await sleep(2000);
  check(await js("[...document.querySelectorAll('.why li')][1].textContent.startsWith('Fertilizer since test: urea (1.0)')"), "saved urea counts in the recommendation");
  const rec2 = await js("document.querySelector('.reccard.main .r')?.textContent");
  check(rec2 === "Recipe C", "after urea the pH estimate drops below the gap: " + rec2);
  check(await js("document.querySelector('.keys').textContent") === "#C#600##", "keypad steps follow the new recipe");
  await js("document.querySelector('.why').open = true");
  await js("document.querySelector('.ph-body').scrollTop = 10000");
  await sleep(200);
  await shot("10-recommend-why");

  // a logged-in phone corrects a wrong clock: the admin moves it an hour off,
  // and this page (logged in as the admin) puts it back on its next live message
  const moved = await api("/api/clock", { epoch: String(Math.round(Date.now() / 1000) + 3600), tz_min: "480" }, adminSid);
  check(moved.body.set === true && moved.body.epoch - Date.now() / 1000 > 3500, "a logged-in phone may correct the clock");
  await sleep(3000);
  check(Math.abs((await api("/api/status")).body.epoch - Date.now() / 1000) < 5, "the logged-in page put the clock right again");

  await tab("crops");
  check(await js("document.querySelectorAll('.ctable tr.has-src').length") === 12, "crop guide lists 12 crops");
  check(await js("document.querySelectorAll('.ctable .pill').length") === 0 && !/verify/i.test(await js("document.querySelector('.ctable').textContent")),
    "no verify tags in the crop guide");
  await shot("11-crops");
  check(await noSideScroll(), "crops: no sideways scroll");

  await tab("batches");
  await sleep(500);
  check(await js("document.querySelectorAll('.rec').length") >= 7, "batch log lists the doses");
  const whens = await js("[...document.querySelectorAll('.rec .when')].map(w => w.textContent)");
  check(whens.some((w) => /^2026-\d\d-\d\d \d\d:\d\d$/.test(w)) && whens.includes("Time not known"),
    "each dose shows its date and time, or 'Time not known': " + JSON.stringify(whens.slice(-3)));
  const doseCsv = await (await fetch(MOCK + "/api/doses.csv")).text();
  check(doseCsv.split("\n")[0].endsWith(",date_time"), "the dose log CSV has a date_time column");
  await shot("12-batches");
  await js("document.querySelector('.rec .vd').click()");
  await sleep(300);
  check(/When(\d{4}-\d\d-\d\d \d\d:\d\d|Time not known) \(ESP32 boot/.test(await js("document.getElementById('sheet').textContent")), "dose details show when it was made");
  await shot("13-dose-details");
  check(await noSideScroll(), "batches: no sideways scroll");

  // the admin gives Juan a new login; Rosa logs in on this phone and sees only her own records
  const juanLogin = await api("/api/farmer-login", { user_id: "1", username: "Juan.D", password: "juan-new-1" }, adminSid);
  check(juanLogin.status === 200 && (await api("/api/logins", null, adminSid)).body.some((l) => l.username === "juan.d"),
    "admin sets a farmer login (username kept in small letters)");
  check((await api("/api/farmer-login", { user_id: "2", username: "juan.d", password: "whatever1" }, adminSid)).body.error === "The username juan.d is taken",
    "a username can't belong to two farmers");

  // the admin deletes a farmer: two taps, then the farmer, their login and records are gone
  check((await api("/api/users", { user_id: "9", name: "Test Farmer", username: "test9", password: "test1234" }, adminSid)).status === 200 &&
    (await api("/api/records/soil", { user_id: "9", plot: "1", date_tested: "2026-10-01", soil_ph: "6", today: "2026-10-30" }, adminSid)).status === 200,
    "a throwaway farmer with a login and a soil test");
  await js("loadUsers('9')");
  await sleep(1200);
  await js("document.getElementById('who').click()");
  await sleep(300);
  await js("[...document.querySelectorAll('#sheet .linkbtn')].find(b => b.textContent.startsWith('Delete farmer')).click()");
  await sleep(200);
  check(await js("[...document.querySelectorAll('#sheet .note.bad')].some(n => !n.hidden && n.textContent.includes(\"can't be undone\"))"),
    "the first tap only warns what will be lost");
  await shot("13b-delete-farmer");
  await js("[...document.querySelectorAll('#sheet .dangerbtn')].find(b => b.textContent.startsWith('Yes, delete Test Farmer')).click()");
  await sleep(1500);
  const gone = { users: (await api("/api/users", null, adminSid)).body, logins: (await api("/api/logins", null, adminSid)).body,
    soil: (await api("/api/records/soil?user_id=9&limit=100", null, adminSid)).body };
  check(!gone.users.some((u) => u.id === 9) && !gone.logins.some((l) => l.username === "test9") && gone.soil.total === 0,
    "farmer 9, their login and their records are deleted");
  check(await js("document.getElementById('who').textContent") === "Choose a farmer", "after the delete no farmer is chosen");
  await js("document.getElementById('sheet-close').click()");
  await js("document.getElementById('who').click()");
  await sleep(300);
  await js("[...document.querySelectorAll('#sheet .linkbtn')].find(b => b.textContent.startsWith('Log out')).click()");
  await sleep(800);
  check(await js("document.getElementById('who').textContent") === "Log in" && await js("!S.users.length"), "logged out: farmers forgotten on this phone");
  await js("document.getElementById('who').click()");
  await sleep(300);
  await js("{ const i = document.querySelectorAll('#sheet input'); i[0].value = 'rosa'; i[1].value = 'rosa1234'; document.querySelector('#sheet .savebtn').click(); }");
  await sleep(1800);
  check(await js("document.getElementById('who').textContent") === "Rosa Cruz · ID 2", "Rosa logged in: her own name in the header");
  await tab("records");
  await sleep(600);
  check(await js("document.querySelectorAll('.rec').length") > 0 && await js("document.querySelectorAll('.dlrow a').length") === 4,
    "Rosa sees her records and 4 downloads (no farmers list)");
  await js("document.getElementById('who').click()");
  await sleep(300);
  check(await js("document.getElementById('sheet-title').textContent.startsWith('My account') && !document.getElementById('sheet-body').textContent.includes('Juan')"),
    "Rosa's account sheet: no other farmers");
  await shot("14-farmer-account");
  await js("document.getElementById('sheet-close').click()");
  const rosaSid = await js("S.me.sid");
  check((await api("/api/records/soil?user_id=1", null, rosaSid)).status === 403, "Rosa can't read Juan's records (403)");
  check((await api("/api/records/soil", { user_id: "1", plot: "7", date_tested: "2026-10-01", soil_ph: "6", today: "2026-10-30" }, rosaSid)).status === 403,
    "Rosa can't add a record for Juan (403)");
  const rosaUsers = (await api("/api/users", null, rosaSid)).body;
  check(rosaUsers.length === 1 && rosaUsers[0].id === 2, "Rosa's farmer list is only herself");
  check((await api("/api/users.csv", null, rosaSid)).status === 403, "the farmers file is admin-only (403)");
  const csv = await (await fetch(MOCK + "/api/records/soil.csv?sid=" + rosaSid)).text();
  check(csv.split("\n").filter(Boolean).slice(1).every((l) => l.startsWith("2,")), "Rosa's soil CSV (sid in the link) has only her rows");

  // the one 400 the page is expected to get: the duplicate soil test refused above
  const expected = (l) => /status of 400 .*\/api\/records\/soil$/.test(l);
  const bad = logs.filter((l) => !l.startsWith("log:") && !l.startsWith("info:") && !expected(l));
  check(bad.length === 0, "no console errors" + (bad.length ? ": " + bad.join(" | ") : ""));
} catch (e) {
  console.log("FAIL  " + e.message);
  failures++;
} finally {
  ws.close();
  edge.kill();
  mock.kill();
}
console.log(failures ? `FAILED: ${failures}` : "ALL PASSED");
process.exit(failures ? 1 : 0);
