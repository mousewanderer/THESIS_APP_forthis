'use strict';
// AgriDose dashboard, in the look of the team's Blynk-style phone app
// (agridose-game docs/05). LOOK-ONLY for the machine: this page reads the
// live feed (/ws) and the history (/api/...). It can save farmers, crop
// records and photos on the SD card, nothing else.
//
// The heavy work happens here, on the phone: the recommendation (agri.js),
// sorting and counting records, day counts, and shrinking photos before
// they are sent. The ESP32 only stores and serves. No frameworks, no
// internet: every file comes from the ESP32.

const $ = (sel, root = document) => root.querySelector(sel);

// Small DOM builder. Text always goes in as text, never as HTML.
function el(tag, attrs, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
    else n.setAttribute(k, v === true ? '' : v);
  }
  return addKids(n, ...kids);
}
// Appends children at any list depth and skips empty ones (null, undefined, false).
function addKids(n, ...kids) {
  for (const kid of kids.flat(Infinity)) if (kid !== null && kid !== undefined && kid !== false) n.append(kid);
  return n;
}

// Browser storage may be blocked (private tab); the page must work without it.
const store = {
  get(k) { try { return localStorage.getItem(k); } catch (_) { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch (_) {} },
};

// ---- Bisaya (Cebuano) under the English ----
// A first draft by the developers: the team must check every line before
// farmers see it, as they did for the sticker. The Bisaya button hides them.
const BIS = {
  'Machine': 'Makina', 'Records': 'Mga rekord', 'Recommend': 'Tambag', 'Crop Guide': 'Giya sa tanom', 'Batch Log': 'Mga dosis',
  'Weight dispensed': 'Timbang nga nagawas', 'Machine Status': 'Kahimtang sa makina', 'Last dose': 'Katapusang dosis',
  'Idle': 'Andam', 'Dosing': 'Nagdosis', 'Fault': 'Naay sayop', 'Offline': 'Walay koneksyon', 'Waiting': 'Naghulat', 'No link': 'Walay koneksyon',
  'Crop Records': 'Mga rekord sa tanom', 'Soil tests': 'Pagsusi sa yuta', 'Seed type and planting': 'Binhi ug pagtanom',
  'Growth records': 'Pagtubo sa tanom', 'Fertilizer applications': 'Pag-abono',
  'Soil test': 'Pagsusi sa yuta', 'Growth record': 'Pagtubo sa tanom', 'Fertilizer application': 'Pag-abono',
  'New soil test': 'Bag-ong pagsusi sa yuta', 'New seed type and planting': 'Bag-ong pagtanom',
  'New growth record': 'Bag-ong rekord sa pagtubo', 'New fertilizer application': 'Bag-ong pag-abono',
  '+ Add': 'Dugang', 'View details': 'Tan-awa', 'Save record': 'I-save', 'Close': 'Sirado', 'Photos': 'Mga litrato',
  'Take photo': 'Kuhaa og litrato', 'Choose photo': 'Pili og litrato', 'Save original': 'I-save ang orihinal', 'Save compressed': 'I-save ang gamay',
  'Plot number': 'Numero sa plot', 'Soil test date': 'Petsa sa pagsusi', 'Soil pH': 'pH sa yuta', 'Soil moisture (%)': 'Kahumod sa yuta (%)',
  'Seed type': 'Klase sa binhi', 'Variety': 'Barayti', 'Date planted': 'Petsa sa pagtanom', 'AgriDose dose numbers': 'Numero sa dosis sa AgriDose',
  'Soil was treated with AgriDose amendment': 'Gibutangan og AgriDose ang yuta', 'Growth stage': 'Ang-ang sa pagtubo',
  'Growth date observed': 'Petsa nga gitan-aw', 'Growth day count': 'Pila ka adlaw na', 'Plant height (cm)': 'Gitas-on sa tanom (cm)',
  'Leaf count': 'Ihap sa dahon', 'Notes': 'Mga sulat', 'Planting': 'Tanom', 'Date applied': 'Petsa sa pag-abono',
  'Fertilizer': 'Abono', 'Amount (kg)': 'Gidaghanon (kg)',
  'Recommendation for': 'Tambag para sa', 'Recommended': 'Gitambag', 'Also possible': 'Pwede usab', 'Not advised': 'Dili angay',
  'Why?': 'Ngano?', 'Rain (mm)': 'Ulan (mm)', '+ Add fertilizer': 'Dugang abono', 'Dose split': 'Bahin sa dosis',
  'At the machine\'s keypad': 'Sa keypad sa makina', 'Download CSV': 'I-download', 'Download records (CSV)': 'I-download ang mga rekord',
  'Farmer': 'Mag-uuma', 'Farmers': 'Mga mag-uuma', 'Add a farmer': 'Dugang mag-uuma', 'Name': 'Ngalan', 'Save farmer': 'I-save',
  'Alerts': 'Mga pahibalo', 'Crop': 'Tanom', 'Local name': 'Lokal nga ngalan',
  'Hopper A Weight': 'Timbang sa hopper A', 'Hopper B Weight': 'Timbang sa hopper B', 'Hopper C Weight': 'Timbang sa hopper C',
  'Total Weight': 'Kinatibuk-ang timbang',
  'Log in': 'Mag-log in', 'Log out': 'Mag-log out', 'Username': 'Username', 'Password': 'Password',
  'Change password': 'Usba ang password', 'New password': 'Bag-ong password', 'My account': 'Akong account',
  'Farmer login': 'Login sa mag-uuma', 'Save login': 'I-save ang login', 'Delete farmer': 'Tangtanga ang mag-uuma', 'Delete': 'Tangtanga',
  'Edit': 'Usba', 'Save changes': 'I-save ang kausaban', 'Change the name': 'Usba ang ngalan', 'Save name': 'I-save ang ngalan',
  'Edit soil test': 'Usba ang pagsusi sa yuta', 'Edit seed type and planting': 'Usba ang pagtanom',
  'Edit growth record': 'Usba ang rekord sa pagtubo', 'Edit fertilizer application': 'Usba ang pag-abono',
  'No soil test yet': 'Wala pay pagsusi sa yuta. Gigamit ang pH 5.5. Maayo nga ipasusi ang yuta.',
};
let bisaya = store.get('bisaya') !== 'off';
// The English text plus its Bisaya line (when on), for use as el() children.
function tr(en) {
  const b = bisaya && BIS[en];
  return b ? [en, el('span', { class: 'bis', lang: 'ceb', text: b })] : en;
}
function setText(node, en) { node.replaceChildren(...[tr(en)].flat()); }
// Only the Bisaya line (or null), for English that has more after it: "Last dose (no. 7)".
function bisOf(en) { return bisaya && BIS[en] ? el('span', { class: 'bis', lang: 'ceb', text: BIS[en] }) : null; }

// ---- number formatting (masses arrive in decigrams, 1 g = 10 dg) ----
function g1(dg) {                       // 2486 -> "248.6", exact, no rounding
  const sign = dg < 0 ? '-' : '';
  dg = Math.abs(dg);
  return sign + Math.floor(dg / 10) + '.' + (dg % 10);
}
function signedG(dg) { return (dg > 0 ? '+' : '') + g1(dg); }
function pctNumber(errDg, targetDg) {   // "+0.60"
  if (!targetDg) return '-';
  const p = (errDg * 100) / targetDg;
  return (p > 0 ? '+' : '') + p.toFixed(2);
}
function today() {                      // the phone's date, YYYY-MM-DD
  const d = new Date();
  return d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0');
}
function kb(bytes) { return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(bytes / 1024)) + ' KB'; }

// ---- talking to the ESP32 ----
async function getJson(url, opts) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 10000);
  try {
    const res = await fetch(url, Object.assign({ cache: 'no-store', signal: ctl.signal }, opts));
    let data = null;
    try { data = await res.json(); } catch (_) {}
    if (res.status === 401) loggedOut('Logged out: log in again');   // the session ended (an ESP32 restart, or it expired)
    if (!res.ok) throw new Error((data && data.error) || 'HTTP ' + res.status);
    return data;
  } catch (e) {
    if (e.name === 'AbortError') throw new Error('no answer from the ESP32');
    throw e;
  } finally {
    clearTimeout(timer);
  }
}

// ================================================================ state ==
const TYPE_KEYS = ['soil', 'planting', 'growth', 'fertilizer'];
const DATE_KEY = { soil: 'date_tested', planting: 'date_planted', growth: 'date_observed', fertilizer: 'date_applied' };
const ONE_TITLE = { soil: 'Soil test', planting: 'Seed type and planting', growth: 'Growth record', fertilizer: 'Fertilizer application' };
const S = {
  tab: 'machine',
  live: null,                           // the latest message from /ws
  me: null,                             // who is logged in, from /api/me (null = not asked yet)
  logins: [],                           // admin: which farmers have a login
  users: [],
  user: '',                             // the chosen farmer's user ID, as text
  types: null,                          // record types and their fields, from the ESP32
  rec: { soil: null, planting: null, growth: null, fertilizer: null },   // { items, total } for the chosen farmer
  recError: '',
  showAll: {},
  doses: null, doseLimit: 20, doseError: '',
};

function currentUser() { return S.users.find((u) => String(u.id) === S.user) || null; }
function typeSpec(type) { return S.types.find((t) => t.type === type); }
function photoIds(item) { return String(item.photos || '').split(' ').filter(Boolean); }
function photoUrl(id) { return withSid('/api/photo?id=' + encodeURIComponent(id)); }

// A record list, newest date first (same date: the one saved last first).
function sorted(type) {
  const k = DATE_KEY[type];
  const list = S.rec[type] ? S.rec[type].items : [];
  return list.map((x, i) => ({ x, i }))
    .sort((a, b) => (a.x[k] < b.x[k] ? 1 : a.x[k] > b.x[k] ? -1 : a.i - b.i))
    .map((o) => o.x);
}
// The newest record of a type for each plot: Map plot -> record.
function newestByPlot(type) {
  const m = new Map();
  for (const r of sorted(type)) if (!m.has(r.plot)) m.set(r.plot, r);
  return m;
}

// ============================================================ overlays ==
let overlayPushed = false;              // a history entry, so Android's Back button closes the sheet
function showSheet(title, ...kids) {
  setText($('#sheet-title'), title);
  addKids($('#sheet-body'), ...kids);
  $('#sheet').hidden = false;
  if (!overlayPushed) { history.pushState({ overlay: 1 }, ''); overlayPushed = true; }
}
function closeOverlays() {
  $('#sheet').hidden = true;
  $('#sheet-title').replaceChildren();
  $('#sheet-body').replaceChildren();
  $('#viewer').hidden = true;
  $('#viewer-img').removeAttribute('src');
}
function closeSheet() {
  if (overlayPushed) history.back();    // popstate closes it
  else closeOverlays();
}
window.addEventListener('popstate', () => { overlayPushed = false; closeOverlays(); });
$('#sheet-close').addEventListener('click', closeSheet);
$('#sheet').addEventListener('click', (e) => { if (e.target === $('#sheet')) closeSheet(); });

let toastTimer = 0;
function toast(text) {
  const t = $('#toast');
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 2600);
}

function openViewer(id) {
  $('#viewer-img').src = photoUrl(id);
  $('#viewer-full').href = photoUrl(id) + '&size=full&download=1';
  $('#viewer-small').href = photoUrl(id) + '&download=1';
  $('#viewer').hidden = false;
  if (!overlayPushed) { history.pushState({ overlay: 1 }, ''); overlayPushed = true; }
}
$('#viewer-close').addEventListener('click', () => {
  if ($('#sheet').hidden) closeSheet();
  else { $('#viewer').hidden = true; $('#viewer-img').removeAttribute('src'); }
});

// ================================================================ tabs ==
const TABS = { machine: renderMachine, records: renderRecords, recommend: renderRecommend, crops: renderCrops, batches: renderBatches };
const TAB_NAMES = { machine: 'Machine', records: 'Records', recommend: 'Recommend', crops: 'Crop Guide', batches: 'Batch Log' };

function render() {
  const u = currentUser();
  $('#who').textContent = !S.me ? '...' : !S.me.logged_in ? 'Log in' : S.me.must_change ? 'Choose a new password'
    : u ? u.name + ' · ID ' + u.id : 'Choose a farmer';
  for (const b of document.querySelectorAll('#tabs button')) {
    const on = b.dataset.tab === S.tab;
    setText(b, TAB_NAMES[b.dataset.tab]);
    b.classList.toggle('on', on);
    b.setAttribute('aria-current', on ? 'page' : 'false');
  }
  const body = $('#body');
  const keep = body.scrollTop;
  body.replaceChildren();
  M = null;
  TABS[S.tab](body);
  body.scrollTop = keep;
}

function showTab(name) {
  if (S.tab !== name) $('#body').scrollTop = 0;
  S.tab = name;
  store.set('tab', name);
  render();
  if (name === 'batches' || (name === 'machine' && !S.doses)) loadDoses();
}
document.querySelectorAll('#tabs button').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));

function widget(label, body) {
  return el('div', { class: 'widget' }, el('div', { class: 'wlabel' }, tr(label)), body);
}
// Records and Recommend before a farmer is chosen: log in, or (admin) choose one.
function farmerPrompt() {
  if (!S.me) return el('div', { class: 'empty', text: 'Loading...' });
  if (!S.me.logged_in) {
    return el('div', { class: 'rgroup' }, el('div', { class: 'rtitle' }, tr('Log in')),
      el('p', { class: 'formhint', text: 'Records, photos and recommendations are private: log in to see them. Watching the machine needs no login.' }),
      loginForm());
  }
  if (S.me.must_change) {
    return el('div', { class: 'empty' }, 'Choose a new admin password first. ',
      el('button', { type: 'button', class: 'linkbtn', onclick: openAccount }, 'Choose it now'));
  }
  return el('div', { class: 'empty' }, 'Choose a farmer first. ',
    el('button', { type: 'button', class: 'linkbtn', onclick: openAccount }, 'Choose or add a farmer'));
}

// ===================================================== Machine (live) ==
// ---- arc gauge (from the team's dashboard reference) ----
const SVG_NS = 'http://www.w3.org/2000/svg';
const GAUGE_START = -225, GAUGE_SWEEP = 270;   // degrees, the arc opens at the bottom
function polar(cx, cy, r, deg) {
  const a = (deg * Math.PI) / 180;
  return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
}
function arcPath(cx, cy, r, a0, a1) {
  const p0 = polar(cx, cy, r, a0), p1 = polar(cx, cy, r, a1);
  const large = a1 - a0 > 180 ? 1 : 0;
  return `M${p0[0].toFixed(2)} ${p0[1].toFixed(2)} A${r} ${r} 0 ${large} 1 ${p1[0].toFixed(2)} ${p1[1].toFixed(2)}`;
}
function svg(tag, attrs) {
  const n = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs || {})) n.setAttribute(k, v);
  return n;
}
function makeGauge(box, min, max) {
  const root = svg('svg', { viewBox: '0 0 120 116', role: 'img' });
  const cx = 60, cy = 58, r = 46;
  const track = svg('path', { d: arcPath(cx, cy, r, GAUGE_START, GAUGE_START + GAUGE_SWEEP), fill: 'none', stroke: 'var(--track)', 'stroke-width': 9, 'stroke-linecap': 'round' });
  const bar = svg('path', { fill: 'none', stroke: 'var(--acc)', 'stroke-width': 11, 'stroke-linecap': 'round' });
  const val = svg('text', { x: cx, y: cy + 6, 'text-anchor': 'middle' });
  const num = svg('tspan', { class: 'gval' });
  const unit = svg('tspan', { class: 'gunit', dy: -8 });
  val.append(num, unit);
  const tick = (p, t) => { const tx = svg('text', { x: p[0].toFixed(1), y: (p[1] + 20).toFixed(1), 'text-anchor': 'middle', class: 'gtick' }); tx.textContent = t; return tx; };
  root.append(track, bar, val, tick(polar(cx, cy, r, GAUGE_START), String(min)), tick(polar(cx, cy, r, GAUGE_START + GAUGE_SWEEP), String(max)));
  box.append(root);
  return { root, bar, num, unit, min, max, cx, cy, r };
}
function setGauge(g, value, text, unit) {
  const f = Math.max(0, Math.min(1, (value - g.min) / (g.max - g.min)));
  g.bar.setAttribute('d', arcPath(g.cx, g.cy, g.r, GAUGE_START, GAUGE_START + Math.max(f * GAUGE_SWEEP, 0.6)));
  g.num.textContent = text;
  g.unit.textContent = unit;
  g.root.setAttribute('aria-label', text + ' ' + unit);
}

// The hopper gauges' scale, as on the team's Blynk screen (0 to 10 kg).
const HOPPER_MAX_KG = 10;
const HOPPERS = [['Hopper A', 'Rice hull'], ['Hopper B', 'Compost'], ['Hopper C', 'Wood ash']];
// 50 g, 140 g, 1.25 kg: grams under 1 kg, kilograms above.
function massText(dg) {
  const g = dg / 10;
  return Math.abs(g) >= 1000 ? [(g / 1000).toFixed(2), 'kg'] : [String(Math.round(g)), 'g'];
}

let M = null;                           // the Machine tab's live parts, while it is shown
function renderMachine(body) {
  const box = el('div', { class: 'gauge' });
  const hopperBoxes = HOPPERS.map(() => el('div', { class: 'gauge' }));
  M = {
    gauge: makeGauge(box, 0, AGRI.doseMaxG),
    hoppers: hopperBoxes.map((b) => makeGauge(b, 0, HOPPER_MAX_KG)),
    total: el('div', { class: 'total' }),
    status: el('div', { class: 'status' }),
    notes: el('div', { class: 'rgroup' }),
    last: el('div', { class: 'reccard' }),
    sys: el('p', { class: 'fine' }),
  };
  addKids(body,
    el('div', { class: 'grid2' }, widget('Weight dispensed', box), widget('Machine Status', M.status)),
    el('div', { class: 'grid3' }, HOPPERS.map(([name, what], i) =>
      widget(name + ' Weight', [hopperBoxes[i], el('div', { class: 'hmat', text: what })]))),
    widget('Total Weight', M.total),
    M.notes, M.last,
    el('p', { class: 'fine', text: 'This app only watches the machine. Doses start at the keypad, and * stops them.' }),
    M.sys);
  updateMachine();
}

const STATES = {
  I: ['Idle', 'Ready', ''], D: ['Dosing', 'Weighing', 'busy'],
  F: ['Fault', 'Press * on the keypad', 'fault'], '?': ['Waiting', 'For the Mega', 'off'],
  R: ['Waiting', 'Unfinished dose: # or * on the keypad', 'fault'],
};

function updateMachine() {
  if (!M) return;
  const s = S.live;
  const linkDown = !s || wifiDown();
  if (linkDown || s.flag === '?') setGauge(M.gauge, 0, '--', 'g');
  else setGauge(M.gauge, s.weight_dg / 10, g1(s.weight_dg), 'g');
  // The Mega only sends a hopper weight when it moved more than 5 g (noise is not sent).
  const hw = !linkDown && s.online && s.hoppers_dg;
  M.hoppers.forEach((g, i) => {
    if (!hw) { setGauge(g, 0, '--', ''); return; }
    const [t, u] = massText(hw[i]);
    setGauge(g, hw[i] / 10000, t, u);
  });
  const [tt, tu] = hw ? massText(hw[0] + hw[1] + hw[2]) : ['--', ''];
  M.total.replaceChildren(tt, el('small', { text: tu }));

  const [word, small, cls] = linkDown ? ['No link', 'Not on the AgriDose Wi-Fi', 'off']
    : !s.online ? ['Offline', 'No data from the Mega', 'off'] : STATES[s.flag] || [s.flag, '', ''];
  M.status.className = 'status ' + cls;
  M.status.replaceChildren(...[tr(word)].flat(), el('small', { text: small }));

  M.notes.replaceChildren(...(s && s.fake_mega ? [el('div', { class: 'note', text: 'TEST MODE: the Mega is simulated (FAKE_MEGA).' })] : []),
    ...(s && !s.sd_ok ? [el('div', { class: 'note bad', text: 'SD card problem: doses, records and photos are not being saved.' })] : []));

  const d = (s && s.last) || (S.doses && S.doses.items[0]) || null;
  if (d) {
    const err = d.actual_dg - d.target_dg;
    const no = d.no || (S.doses && S.doses.items[0] && S.doses.items[0].seq === d.seq ? S.doses.items[0].no : 0);
    M.last.replaceChildren(
      el('span', { class: 'k' }, 'Last dose' + (no ? ' (no. ' + no + ')' : ''), bisOf('Last dose')),
      el('span', { class: 'r', text: 'Recipe ' + d.recipe }),
      el('dl', { class: 'dl' },
        el('dt', { text: 'Target' }), el('dd', { text: g1(d.target_dg) + ' g' }),
        el('dt', { text: 'Actual' }), el('dd', { text: g1(d.actual_dg) + ' g' }),
        el('dt', { text: 'Error' }), el('dd', { text: signedG(err) + ' g (' + pctNumber(err, d.target_dg) + ' %)' })));
  } else {
    M.last.replaceChildren(el('span', { class: 'k' }, tr('Last dose')), el('p', { class: 'fine', text: 'No dose yet.' }));
  }
  M.sys.textContent = s ? 'ESP32 boot ' + s.boot + ', up ' + Math.floor(s.uptime_s / 60) + ' min, ' +
    (s.clock_set ? 'clock ' + clockText(s.epoch) + ', ' : s.clock_set === false ? 'clock not set yet, ' : '') +
    Math.round(s.heap_free / 1024) + ' KB free (lowest ' + Math.round(s.heap_min / 1024) + ' KB), ' +
    s.clients + ' phone(s) on the live feed' : '';
}

// ---- header: online dot and the bell ----
function alerts() {
  const s = S.live;
  if (wifiDown()) return ['Not connected to the AgriDose Wi-Fi.'];
  if (!s) return [];
  const out = [];
  if (!s.online) out.push('Mega offline: no data from the machine for 2 seconds.');
  if (s.flag === 'F') out.push('Fault at the machine (hopper empty, sensor or time-out). Press * on the keypad to clear it.');
  if (s.flag === 'R') out.push('The power went off during a dose. Press # on the keypad to continue it, or * to end it.');
  if (!s.sd_ok) out.push('SD card problem: doses, records and photos are not being saved.');
  return out;
}
function updateHeader() {
  const on = !wifiDown() && !!S.live && S.live.online;
  $('#dot').classList.toggle('on', on);
  $('#dot').title = on ? 'Machine online' : 'Machine offline';
  $('#badge').hidden = alerts().length === 0;
}
$('#bell').addEventListener('click', () => {
  const list = alerts();
  if (!list.length) { toast('No alerts'); return; }
  showSheet('Alerts', list.map((a) => el('div', { class: 'note' + (a.startsWith('SD') || a.startsWith('Fault') ? ' bad' : ''), text: a })));
});

// ---- WebSocket with automatic reconnect ----
// The ESP32 sends at least once a second. If nothing arrives for 5 s (the
// phone slept, the Wi-Fi dropped), the socket is thrown away and a new one
// is opened. Waking the phone or coming back online reconnects at once.
let ws = null;
let lastMsgAt = 0;
let retryMs = 1000;
let retryTimer = null;
let downSince = Date.now();
let lastDoseCount = null;

function wifiDown() { return !!downSince && Date.now() - downSince > 2000; }
function setConnected(ok) {
  if (ok) downSince = 0;
  else if (!downSince) downSince = Date.now();
}

// ---- the ESP32's clock (1 Nov 2026) ----
// The ESP32 has no clock of its own and no internet, so this page gives it
// the phone's time: any phone while the ESP32 has none (after every
// restart), and a logged-in phone when the ESP32 is more than CLOCK_SLACK_S
// off. The ESP32 applies the same rules (clock_rules.h) and may say no.
const CLOCK_SLACK_S = 120;
let clockSentAt = 0;
function syncClock(s) {
  if (typeof s.clock_set !== 'boolean') return;          // an older firmware without the clock
  const now = Math.round(Date.now() / 1000);
  const loggedIn = S.me && S.me.logged_in && !S.me.must_change;
  const off = !s.clock_set || (loggedIn && Math.abs(s.epoch - now) > CLOCK_SLACK_S);
  if (!off || Date.now() - clockSentAt < 10000) return;  // at most one try every 10 s
  clockSentAt = Date.now();
  getJson('/api/clock', { method: 'POST', body: new URLSearchParams({ epoch: now, tz_min: -new Date().getTimezoneOffset() }) })
    .catch(() => {});
}
function clockText(epoch) {                              // UTC seconds -> "14:05" on this phone
  const d = new Date(epoch * 1000);
  return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
}

function onLive(s) {
  S.live = s;
  syncClock(s);
  if (lastDoseCount !== null && s.dose_count !== lastDoseCount) loadDoses();   // a new row was logged
  lastDoseCount = s.dose_count;
  updateHeader();
  updateMachine();
}

function connect() {
  clearTimeout(retryTimer);
  if (ws) return;
  const sock = new WebSocket('ws://' + location.host + '/ws');
  ws = sock;
  sock.onopen = () => { retryMs = 1000; lastMsgAt = Date.now(); setConnected(true); };
  sock.onmessage = (e) => {
    lastMsgAt = Date.now();
    try { onLive(JSON.parse(e.data)); } catch (_) {}
  };
  sock.onclose = () => {
    if (ws !== sock) return;           // an old socket we already replaced
    ws = null;
    setConnected(false);
    retryTimer = setTimeout(connect, retryMs);
    retryMs = Math.min(retryMs * 2, 10000);
  };
  sock.onerror = () => { try { sock.close(); } catch (_) {} };
}

function reconnectNow() {
  const old = ws;
  ws = null;
  if (old) {
    old.onclose = old.onmessage = old.onerror = null;
    try { old.close(); } catch (_) {}
  }
  setConnected(false);
  retryMs = 1000;
  connect();
}

setInterval(() => {
  if (ws && ws.readyState === WebSocket.OPEN && Date.now() - lastMsgAt > 5000) reconnectNow();   // silent socket
  const down = wifiDown();
  if ($('#banner').hidden === down) {       // the link just went down or came back
    $('#banner').hidden = !down;
    updateHeader();
    updateMachine();
  }
}, 1000);
document.addEventListener('visibilitychange', () => {
  if (!document.hidden && (!ws || Date.now() - lastMsgAt > 3000)) reconnectNow();
});
window.addEventListener('online', reconnectNow);
window.addEventListener('pageshow', (e) => { if (e.persisted) reconnectNow(); });

// ============================================================= logins ==
// Watching the machine needs no login. A farmer's records, photos and
// recommendation need that farmer's login, or the admin's (accounts.h on
// the ESP32 decides; the page only follows).
function loggedIn() { return !!(S.me && S.me.logged_in && !S.me.must_change); }
function isAdmin() { return loggedIn() && S.me.role === 'admin'; }
// ?sid= for links the Android app downloads itself (it has no cookie).
function withSid(url) { return loggedIn() ? url + (url.includes('?') ? '&' : '?') + 'sid=' + S.me.sid : url; }

async function loadMe() {
  try { S.me = await getJson('/api/me'); } catch (_) { S.me = { logged_in: false }; }
  afterLogin();
}

// After /api/me, a login, a logout or a new password.
function afterLogin() {
  S.users = [];
  S.logins = [];
  S.rec = { soil: null, planting: null, growth: null, fertilizer: null };
  S.recError = '';
  if (!loggedIn()) { S.user = ''; render(); return; }
  loadUsers(isAdmin() ? null : String(S.me.user_id));
}

// The ESP32 says this phone is not logged in (any more): back to the login.
function loggedOut(text) {
  const was = loggedIn();
  S.me = { logged_in: false };
  if (was) { toast(text); afterLogin(); }
}

// A small form: labelled inputs, a line for the ESP32's reason, a button.
// send() does the work and may throw; its message is shown.
let fieldNo = 0;
function inputField(label, attrs, wide = true) {
  const id = attrs.id || 'f-' + attrs.name + '-' + (++fieldNo);
  const input = el('input', Object.assign({ autocomplete: 'off' }, attrs, { id }));
  return { input, wrap: el('div', { class: 'fld' + (wide ? ' wide' : '') }, el('label', { for: id }, tr(label)), input) };
}
function smallForm(fields, buttonText, send) {
  const msg = el('p', { class: 'formmsg', role: 'alert', hidden: true });
  const button = el('button', { type: 'submit', class: 'savebtn wide' }, tr(buttonText));
  const form = el('form', { class: 'phform', novalidate: true }, fields.map((f) => f.wrap), el('div', { class: 'wide' }, msg), button);
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    msg.hidden = true;
    button.disabled = true;
    try {
      await send();
    } catch (err) {
      msg.textContent = err.message;          // the ESP32's own reason
      msg.hidden = false;
    } finally {
      button.disabled = false;
    }
  });
  return form;
}
function group(title, ...kids) { return [el('div', { class: 'glabel' }, el('span', {}, tr(title))), ...kids]; }

function loginForm() {
  const user = inputField('Username', { name: 'username', maxlength: 20, autocomplete: 'username', autocapitalize: 'none', spellcheck: 'false' });
  const pass = inputField('Password', { name: 'password', type: 'password', maxlength: 64, autocomplete: 'current-password' });
  return smallForm([user, pass], 'Log in', async () => {
    S.me = await getJson('/api/login', { method: 'POST', body: new URLSearchParams({ username: user.input.value, password: pass.input.value }) });
    pass.input.value = '';
    toast(S.me.must_change ? 'Choose a new password' : 'Logged in as ' + S.me.username);
    afterLogin();
    if (!S.me.must_change) { closeSheet(); return; }
    // straight on to the new password: swap the sheet's content (closing it first would let
    // the late history "back" close the new one too)
    $('#sheet-body').replaceChildren();
    openAccount();
  });
}

function passwordForm() {
  const old = inputField(S.me.must_change ? 'First password' : 'Current password', { name: 'old', type: 'password', maxlength: 64, autocomplete: 'current-password' });
  const fresh = inputField('New password', { name: 'new', type: 'password', maxlength: 64, autocomplete: 'new-password' });
  const again = inputField('New password again', { name: 'again', type: 'password', maxlength: 64, autocomplete: 'new-password' });
  return smallForm([old, fresh, again], 'Change password', async () => {
    if (fresh.input.value !== again.input.value) throw new Error('The two new passwords are not the same.');
    S.me = await getJson('/api/account/password', { method: 'POST', body: new URLSearchParams({ old: old.input.value, new: fresh.input.value }) });
    closeSheet();
    toast('Password changed');
    afterLogin();
  });
}

function logoutButton() {
  return el('button', { type: 'button', class: 'linkbtn', onclick: async () => {
    try { await getJson('/api/logout', { method: 'POST' }); } catch (_) {}
    S.me = { logged_in: false };
    closeSheet();
    toast('Logged out');
    afterLogin();
  } }, tr('Log out'));
}

// Tap the name in the header: log in, or the account (and, for the admin, the farmers).
function openAccount() {
  if (!S.me || !S.me.logged_in) {
    showSheet('Log in', el('p', { class: 'formhint', text: 'Records, photos and recommendations are private: log in to see them. Watching the machine needs no login.' }),
      loginForm());
    return;
  }
  if (S.me.must_change) {
    showSheet('Choose a new password', el('div', { class: 'note', text: 'This is the first admin password from the setup. Choose your own before anything else.' }),
      passwordForm(), logoutButton());
    return;
  }
  const who = el('p', { class: 'formhint', text: 'Logged in as ' + S.me.username + (isAdmin() ? ' (admin)' : '') + '.' });
  if (!isAdmin()) {
    const u = currentUser();
    showSheet('My account', who, u ? el('div', { class: 'choice on', text: u.name + ' · ID ' + u.id }) : null,
      group('Change password', passwordForm()), logoutButton());
    return;
  }
  const list = S.users.map((u) => el('button', {
    type: 'button', class: 'choice' + (String(u.id) === S.user ? ' on' : ''),
    onclick: () => { closeSheet(); selectUser(String(u.id)); },
  }, u.name + ' · ID ' + u.id + (loginOf(u.id) ? ' · ' + loginOf(u.id) : ' · no login')));
  showSheet('Farmers', who,
    list.length ? list : el('div', { class: 'empty', text: 'No farmers yet. Add one below.' }),
    renameForm(), farmerLoginForm(), deleteFarmerBox(), group('Add a farmer', addFarmerForm()),
    group('Change password', passwordForm()), logoutButton());
}
$('#who').addEventListener('click', openAccount);

// ============================================================ farmers ==
function loginOf(id) { const l = S.logins.find((x) => String(x.user_id) === String(id)); return l ? l.username : ''; }

async function loadUsers(selectId) {
  try {
    S.users = await getJson('/api/users');
    if (isAdmin()) S.logins = await getJson('/api/logins');
  } catch (err) {
    S.recError = 'Could not load the farmers: ' + err.message;
    render();
    return;
  }
  const want = String(selectId || S.user || store.get('user') || '');
  if (S.users.some((u) => String(u.id) === want)) selectUser(want);
  else if (S.users.length === 1) selectUser(String(S.users[0].id));   // only one farmer: pick them
  else render();
}

function selectUser(id) {
  if (S.user !== id) {
    S.rec = { soil: null, planting: null, growth: null, fertilizer: null };
    S.showAll = {};
  }
  S.user = id;
  store.set('user', id);
  render();
  loadRecords();
}

// Admin: a new farmer and their login, in one form.
function addFarmerForm() {
  const id = inputField('User ID (1 to 9999)', { id: 'f-uid', name: 'user_id', type: 'number', inputmode: 'numeric', min: 1, max: 9999, step: 1 }, false);
  const name = inputField('Name', { id: 'f-uname', name: 'name', maxlength: 30 }, false);
  const user = inputField('Username', { id: 'f-ulogin', name: 'username', maxlength: 20, autocapitalize: 'none', spellcheck: 'false' }, false);
  const pass = inputField('Password', { id: 'f-upass', name: 'password', type: 'password', maxlength: 64, autocomplete: 'new-password' }, false);
  return smallForm([id, name, user, pass], 'Save farmer', async () => {
    await getJson('/api/users', { method: 'POST', body: new URLSearchParams({
      user_id: id.input.value, name: name.input.value, username: user.input.value, password: pass.input.value }) });
    closeSheet();
    toast('Farmer saved');
    await loadUsers(String(Number(id.input.value)));
  });
}

// Admin: fixes the chosen farmer's name if it was typed wrong. The user ID
// stays: the records point to it.
function renameForm() {
  const me = currentUser();
  if (!me) return null;
  const name = inputField('Name', { id: 'f-rename', name: 'name', maxlength: 30, value: me.name });
  name.wrap.firstChild.append(el('span', { text: ' of ID ' + me.id }));
  return group('Change the name', smallForm([name], 'Save name', async () => {
    await getJson('/api/edit/user', { method: 'POST', body: new URLSearchParams({ user_id: me.id, name: name.input.value }) });
    closeSheet();
    toast('Name saved');
    await loadUsers(String(me.id));
  }));
}

// Admin: deletes the chosen farmer with their login, records and photos.
// Two taps: the first only shows what will be lost.
function deleteFarmerBox() {
  const me = currentUser();
  if (!me) return null;
  const msg = el('p', { class: 'formmsg', role: 'alert', hidden: true });
  const warn = el('p', { class: 'note bad', hidden: true, text: me.name + ' (ID ' + me.id + '), their login, soil tests, plantings, growth and ' +
    'fertilizer records and photos are deleted from the SD card. This can\'t be undone. The Batch Log stays.' });
  const sure = el('button', { type: 'button', class: 'savebtn dangerbtn', hidden: true }, 'Yes, delete ' + me.name + ' and all their records');
  const ask = el('button', { type: 'button', class: 'linkbtn danger', onclick: () => { ask.hidden = true; warn.hidden = false; sure.hidden = false; } },
    tr('Delete farmer'));
  sure.addEventListener('click', async () => {
    sure.disabled = true;
    msg.hidden = true;
    try {
      await getJson('/api/delete/user', { method: 'POST', body: new URLSearchParams({ user_id: me.id }) });
      closeSheet();
      toast(me.name + ' deleted');
      S.user = '';
      store.set('user', '');
      await loadUsers();
    } catch (err) {
      msg.textContent = err.message;
      msg.hidden = false;
    } finally {
      sure.disabled = false;
    }
  });
  return group('Delete farmer', ask, warn, sure, msg);
}

// Admin: the chosen farmer's login (new, or a new username / password if
// they forgot it; their phones are logged out).
function farmerLoginForm() {
  const me = currentUser();
  if (!me) return null;
  const has = loginOf(me.id);
  const user = inputField('Username', { id: 'f-flogin', name: 'username', maxlength: 20, autocapitalize: 'none', spellcheck: 'false', value: has }, false);
  const pass = inputField('New password', { id: 'f-fpass', name: 'password', type: 'password', maxlength: 64, autocomplete: 'new-password' }, false);
  return group('Farmer login', el('p', { class: 'formhint', text: has ? me.name + ' logs in as ' + has + '. Set a new password here if they forgot it.' : me.name + ' has no login yet.' }),
    smallForm([user, pass], 'Save login', async () => {
      await getJson('/api/farmer-login', { method: 'POST', body: new URLSearchParams({ user_id: me.id, username: user.input.value, password: pass.input.value }) });
      closeSheet();
      toast('Login saved');
      await loadUsers(String(me.id));
    }));
}

// ============================================================ records ==
async function loadRecords() {
  const user = S.user;
  if (!user) return;
  try {
    if (!S.types) S.types = await getJson('/api/record-types');
    for (const t of TYPE_KEYS) {                // one at a time: the ESP32 answers one request at a time anyway
      const data = await getJson('/api/records/' + t + '?user_id=' + encodeURIComponent(user) + '&limit=100');
      if (user !== S.user) return;              // the farmer was changed meanwhile
      S.rec[t] = data;
    }
    S.recError = '';
  } catch (err) {
    S.recError = 'Could not load the records: ' + err.message;
  }
  render();
}
function recordsReady() { return S.types && TYPE_KEYS.every((t) => S.rec[t]); }

function countText(shown, total) {
  if (total === 0) return 'none yet';
  return total > shown ? 'Showing ' + shown + ' of ' + total : total + (total === 1 ? ' record' : ' records');
}

// The title and second line of one record row.
function rowText(type, r) {
  if (type === 'soil') {
    return { title: 'pH ' + r.soil_ph + (r.soil_moisture_pct !== '' ? ' · moisture ' + r.soil_moisture_pct + '%' : ''),
      sub: 'Plot ' + r.plot + ' · tested ' + r.date_tested };
  }
  if (type === 'planting') {
    return { title: cropLabel(r.crop) + (r.variety ? ' · ' + r.variety : ''), pill: r.used_agridose === 'yes' ? 'AgriDose' : '',
      sub: 'Plot ' + r.plot + ' · planted ' + r.date_planted + (r.dose_numbers ? ' · doses ' + r.dose_numbers : '') };
  }
  if (type === 'fertilizer') {
    return { title: r.fertilizer + (r.amount_kg !== '' ? ' · ' + r.amount_kg + ' kg' : ''),
      sub: 'Plot ' + r.plot + ' · applied ' + r.date_applied };
  }
  const p = newestByPlot('planting').get(r.plot);
  return { title: r.stage + (r.days_after_planting !== '' ? ' · day ' + r.days_after_planting : ''),
    sub: (p ? cropLabel(p.crop) + ' · ' : '') + 'Plot ' + r.plot + ' · observed ' + r.date_observed };
}

const EMPTY_TEXT = {
  soil: 'No soil tests yet.', planting: 'No plantings yet.',
  growth: 'No growth records yet. Add a planting first, then its growth.',
  fertilizer: 'No fertilizer applications yet. Recommend counts the ones after each soil test.',
};

function recordGroup(type) {
  const spec = typeSpec(type);
  const all = sorted(type);
  const total = S.rec[type].total;
  const shown = S.showAll[type] ? all : all.slice(0, 3);
  const group = el('div', { class: 'rgroup' },
    el('div', { class: 'glabel' },
      el('span', {}, tr(spec.title)),
      el('span', { class: 'gright' },
        el('span', { text: countText(shown.length, total) }),
        el('button', { type: 'button', class: 'addbtn', onclick: () => openForm(type), 'aria-label': 'Add to ' + spec.title.toLowerCase() }, tr('+ Add')))));
  if (!shown.length) group.append(el('div', { class: 'empty', text: EMPTY_TEXT[type] }));
  for (const item of shown) {
    const r = rowText(type, item);
    const ids = photoIds(item);
    group.append(el('div', { class: 'rec' },
      ids.length ? el('img', { class: 'rthumb', src: photoUrl(ids[0]), alt: '', loading: 'lazy' }) : null,
      el('div', { class: 'rmain' },
        el('b', {}, r.title, r.pill ? el('span', { class: 'pill', text: r.pill }) : null,
          ids.length > 1 ? el('span', { class: 'pill warn', text: ids.length + ' photos' }) : null),
        el('span', { text: r.sub })),
      el('button', { type: 'button', class: 'vd', onclick: () => openDetails(type, item) }, tr('View details'))));
  }
  if (all.length > 3) {
    group.append(el('button', { type: 'button', class: 'linkbtn', onclick: () => { S.showAll[type] = !S.showAll[type]; render(); } },
      S.showAll[type] ? 'Show the newest 3' : 'Show all ' + all.length));
  }
  if (S.showAll[type] && total > all.length) group.append(el('p', { class: 'fine', text: 'The newest ' + all.length + ' of ' + total + ' are shown.' }));
  return group;
}

function renderRecords(body) {
  if (!S.user) { body.append(farmerPrompt()); return; }
  addKids(body,
    el('div', {}, el('div', { class: 'rtitle' }, tr('Crop Records')), el('div', { class: 'rsub', text: 'Saved on the machine\'s microSD card.' })),
    S.recError ? el('div', { class: 'note bad', text: S.recError }) : null);
  if (!recordsReady()) { if (!S.recError) body.append(el('div', { class: 'empty', text: 'Loading...' })); return; }
  for (const t of TYPE_KEYS) body.append(recordGroup(t));
  body.append(downloads());
}

// The CSV files from the SD card, to open in a spreadsheet app: the admin
// gets the whole files (every farmer), a farmer only their own rows.
function downloads() {
  const link = (href, file, label) => el('a', { class: 'button', href: withSid(href), download: file }, tr(label));
  return el('div', { class: 'rgroup' },
    el('div', { class: 'glabel' }, el('span', {}, tr('Download records (CSV)'))),
    el('div', { class: 'dlrow' },
      S.types.map((t) => link('/api/records/' + t.type + '.csv', 'agridose_' + t.type + '.csv', t.title)),
      isAdmin() ? link('/api/users.csv', 'agridose_farmers.csv', 'Farmers') : null),
    el('p', { class: 'fine', text: (isAdmin() ? 'Whole files from the SD card, every farmer.' : 'Your own records from the SD card.') +
      ' They open in a spreadsheet app. Dates are the ones typed on the phones.' }));
}

function showValue(f, v) {
  if (f.kind === 'yesno') return v === 'yes' ? 'Yes' : 'No';
  if (f.name === 'crop') return cropLabel(v);
  return v;
}

function photoGrid(ids) {
  return el('div', { class: 'thumbs' }, ids.map((id, i) => el('button', {
    type: 'button', class: 'thumb', 'aria-label': 'Open photo ' + (i + 1), onclick: () => openViewer(id),
  }, el('img', { src: photoUrl(id), alt: '' }))));
}

function openDetails(type, item) {
  const spec = typeSpec(type);
  const dl = el('dl', { class: 'fields' });
  for (const f of spec.fields) {
    if (f.kind === 'photos' || item[f.name] === '' || item[f.name] === undefined) continue;
    dl.append(el('dt', {}, tr(f.label)), el('dd', { text: showValue(f, item[f.name]) }));
  }
  const ids = photoIds(item);
  // Edit swaps this sheet's content for the form (same history entry, so Back still closes it).
  const edit = el('button', { type: 'button', class: 'savebtn editbtn', onclick: () => { $('#sheet-body').replaceChildren(); openForm(type, null, item); } }, tr('Edit'));
  showSheet(ONE_TITLE[type], dl,
    ids.length ? [el('div', { class: 'glabel' }, el('span', {}, tr('Photos'))), photoGrid(ids),
      el('p', { class: 'fine', text: 'Tap a photo to see it bigger and to save the original or the compressed copy.' })] : null,
    edit, deleteRecordBox(type, item, ids.length));
}

// Delete (1 Nov 2026): one record and its photos, after a second tap. The
// ESP32 checks the record is unchanged (row + rev, as for an edit) and won't
// delete a planting whose plot still has growth records.
function deleteRecordBox(type, item, photoCount) {
  const msg = el('p', { class: 'formmsg', role: 'alert', hidden: true });
  const warn = el('p', { class: 'note bad', hidden: true, text: 'Deleting removes this record' + (photoCount ? ' and its ' + photoCount + ' photo(s)' : '') +
    ' from the SD card. This can\'t be undone.' });
  const sure = el('button', { type: 'button', class: 'savebtn dangerbtn', hidden: true }, 'Yes, delete this record');
  const ask = el('button', { type: 'button', class: 'linkbtn danger', onclick: () => { ask.hidden = true; warn.hidden = false; sure.hidden = false; } },
    tr('Delete'));
  sure.addEventListener('click', async () => {
    sure.disabled = true;
    msg.hidden = true;
    try {
      await getJson('/api/delete/' + type, { method: 'POST', body: new URLSearchParams({ user_id: S.user, row: item.row, rev: item.rev }) });
      closeSheet();
      toast('Record deleted');
      await loadRecords();
    } catch (err) {
      msg.textContent = err.message;   // e.g. "Plot 3 has 2 growth record(s) from this planting. Delete them first."
      msg.hidden = false;
    } finally {
      sure.disabled = false;
    }
  });
  return el('div', { class: 'delrec' }, ask, warn, sure, msg);
}

// ---- "+ Add" forms, built from the fields the ESP32 sends ----
const OTHER = '\u0000other';
const START = { soil_ph: 7, soil_moisture_pct: 50 };   // where - and + start on an empty pH or moisture box

function decimals(step) { const s = String(step); return s.includes('.') ? s.length - s.indexOf('.') - 1 : 0; }

// A number box with - and + buttons, like the Blynk step widget. On an
// empty box the first tap fills in start() (shown greyed as the placeholder).
// o: { id, name, min, max, step, decimals, start(), required }
function stepper(o) {
  const input = el('input', {
    id: o.id, name: o.name, type: 'number', min: o.min, max: o.max,
    step: o.decimals ? String(Math.pow(10, -o.decimals)) : '1', inputmode: o.decimals ? 'decimal' : 'numeric',
  });
  input.required = !!o.required;
  input.placeholder = String(o.start());
  const bump = (dir) => {
    let v = parseFloat(input.value);
    v = isNaN(v) ? o.start() : v + dir * o.step;
    v = Math.min(o.max, Math.max(o.min, v));
    input.value = String(Math.round(v * Math.pow(10, o.decimals)) / Math.pow(10, o.decimals));
    input.dispatchEvent(new Event('input', { bubbles: true }));
  };
  const wrap = el('div', { class: 'stepper' },
    el('button', { type: 'button', class: 'step', 'aria-label': 'Less', onclick: () => bump(-1) }, '−'),
    input,
    el('button', { type: 'button', class: 'step', 'aria-label': 'More', onclick: () => bump(1) }, '+'));
  return { wrap, input };
}

function fieldBox(f, kids, wide) {
  return el('div', { class: 'fld' + (wide ? ' wide' : '') },
    el('label', { for: 'f-' + f.name }, f.label, f.required ? null : el('span', { text: ' optional' }),
      bisaya && BIS[f.label] ? el('span', { class: 'bis', lang: 'ceb', text: BIS[f.label] }) : null), kids);
}

// One control per field: { wrap, input, get(), set(value) }.
function control(type, f) {
  const id = 'f-' + f.name;
  if (f.kind === 'int' || f.kind === 'decimal') {
    const prev = sorted(type)[0];
    const s = stepper({
      id, name: f.name, min: f.min, max: f.max, required: f.required,
      step: Number(f.step || 1), decimals: f.kind === 'int' ? 0 : Math.min(f.size, Math.max(1, decimals(f.step || 1))),
      start: () => {
        const p = prev ? parseFloat(prev[f.name]) : NaN;
        return !isNaN(p) ? p : f.name in START ? START[f.name] : f.min;
      },
    });
    return { wrap: fieldBox(f, s.wrap, false), input: s.input, get: () => s.input.value, set: (v) => { s.input.value = v; } };
  }
  if (f.kind === 'date') {
    const input = el('input', { id, name: f.name, type: 'date', max: today() });   // no dates after today (data rules)
    input.required = f.required;
    if (f.required) input.value = today();
    return { wrap: fieldBox(f, input, false), input, get: () => input.value, set: (v) => { input.value = v; } };
  }
  if (f.kind === 'yesno') {
    const box = el('input', { id, type: 'checkbox' });
    const label = f.name === 'used_agridose' ? 'Soil was treated with AgriDose amendment' : f.label;
    return { wrap: el('div', { class: 'fld check wide' }, box, el('label', { for: id }, tr(label))), input: box,
      get: () => (box.checked ? 'yes' : 'no'), set: (v) => { box.checked = v === 'yes'; } };
  }
  if (f.kind === 'text' && f.options) {         // drop-down, plus "Other" to type a new one
    const text = el('input', { name: f.name, maxlength: f.size, placeholder: 'Type the ' + f.label.toLowerCase(), autocomplete: 'off', hidden: true });
    const select = el('select', { id },
      el('option', { value: '', text: 'Choose...' }),
      f.options.map((o) => el('option', { value: o, text: f.name === 'crop' ? cropLabel(o) : o })),
      el('option', { value: OTHER, text: 'Other (type it)' }));
    select.required = f.required;
    select.addEventListener('change', () => {
      const other = select.value === OTHER;
      text.hidden = !other;
      text.value = other ? '' : select.value;
      if (other) text.focus();
    });
    const set = (v) => {                       // a value not in the list was typed under "Other"
      const listed = v === '' || f.options.includes(v);
      select.value = listed ? v : OTHER;
      text.hidden = listed;
      text.value = v;
    };
    return { wrap: fieldBox(f, [select, text], true), input: select, get: () => text.value, set };
  }
  const input = el('input', { id, name: f.name, maxlength: f.size, autocomplete: 'off' });
  if (f.kind === 'numlist') { input.inputMode = 'numeric'; input.placeholder = 'Dose numbers from the Batch Log, e.g. 12 13'; }
  if (f.name === 'notes') input.placeholder = 'What did you notice?';
  input.required = f.required;
  return { wrap: fieldBox(f, input, true), input, get: () => input.value, set: (v) => { input.value = v; } };
}

// preset: values to start with, e.g. { plot: '2' } from the Recommend tab.
// editing: a record from the list (with its row and rev): the form opens
// filled in with it, and saving replaces that record instead of adding one.
function openForm(type, preset, editing) {
  const spec = typeSpec(type);
  const plantings = [...newestByPlot('planting').values()].sort((a, b) => a.plot - b.plot);
  if (type === 'growth' && !plantings.length && !editing) {
    showSheet('New growth record', el('p', { class: 'formmsg', text: 'Add a planting first. A growth record belongs to a planting.' }));
    return;
  }
  const ctl = {};
  let picker = null;
  const wraps = [];
  for (const f of spec.fields) {
    if (f.kind === 'photos') { picker = photoPicker(f.size, editing ? photoIds(editing) : []); continue; }
    if (type === 'growth' && f.name === 'plot') {   // pick the planting; it gives the plot and the date planted
      const select = el('select', { id: 'f-plot' }, plantings.map((p) => el('option', {
        value: p.plot, text: 'Plot ' + p.plot + ': ' + cropLabel(p.crop) + (p.variety ? ', ' + p.variety : '') + ', planted ' + p.date_planted,
      })));
      const set = (v) => {                       // a plot with no planting now (it was corrected) keeps its own entry
        v = String(v);
        if (![...select.options].some((o) => o.value === v)) select.append(el('option', { value: v, text: 'Plot ' + v }));
        select.value = v;
      };
      ctl.plot = { wrap: el('div', { class: 'fld wide' }, el('label', { for: 'f-plot' }, tr('Planting')), select), input: select, get: () => select.value, set };
    } else {
      ctl[f.name] = control(type, f);
    }
    wraps.push(ctl[f.name].wrap);
  }
  for (const [name, v] of Object.entries(editing || preset || {})) if (ctl[name]) ctl[name].set(v);

  const saveText = editing ? 'Save changes' : 'Save record';
  const hint = type === 'growth' ? el('p', { class: 'formhint wide' }) : null;
  const msg = el('p', { class: 'formmsg wide', role: 'alert', hidden: true });
  const button = el('button', { type: 'submit', class: 'savebtn wide' }, tr(saveText));
  const form = el('form', { class: 'phform', novalidate: true }, wraps, hint, picker ? picker.wrap : null, msg, button);

  if (type === 'growth') {
    const updateDays = () => {
      const a = ctl.date_planted.input.value, b = ctl.date_observed.input.value;
      if (!a || !b) { hint.textContent = ''; return; }
      const d = daysBetween(a, b);
      hint.textContent = d >= 0 ? 'Growth day count: ' + d + ' (from the date planted)' : 'The date observed is before the date planted.';
      if (d >= 0 && d <= 365) ctl.days_after_planting.input.value = d;
    };
    const fillPlanted = () => {
      const p = plantings.find((x) => x.plot === ctl.plot.input.value);
      if (p) ctl.date_planted.input.value = p.date_planted;
      updateDays();
    };
    ctl.plot.input.addEventListener('change', fillPlanted);
    ctl.date_observed.input.addEventListener('input', updateDays);
    ctl.date_planted.input.addEventListener('input', updateDays);
    if (editing) updateDays();                   // keep the saved date planted
    else fillPlanted();
  }

  const fail = (text) => { msg.textContent = text; msg.hidden = false; return false; };
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    msg.hidden = true;
    if (type === 'growth' && ctl.date_planted.input.value && ctl.date_observed.input.value &&
        daysBetween(ctl.date_planted.input.value, ctl.date_observed.input.value) < 0) {
      fail('The date observed can\'t be before the date planted.');
      return;
    }
    // today: the ESP32 has no clock, so it checks "no dates after today" against the phone's date
    const body = new URLSearchParams({ user_id: S.user, today: today() });
    if (editing) { body.set('row', editing.row); body.set('rev', editing.rev); }
    for (const [name, c] of Object.entries(ctl)) body.set(name, c.get());
    button.disabled = true;
    try {
      if (picker) body.set('photos', (await picker.upload((t) => { button.textContent = t; })).join(' '));
      button.textContent = 'Saving...';
      await getJson((editing ? '/api/edit/' : '/api/records/') + type, { method: 'POST', body });
      if (picker) picker.clear();
      closeSheet();
      toast(editing ? 'Record updated' : 'Record saved');
      await loadRecords();
    } catch (err) {
      fail(err.message);                // the ESP32's own reason, e.g. "Soil pH must be a number from 0 to 14"
    } finally {
      button.disabled = false;
      setText(button, saveText);
    }
  });
  showSheet((editing ? 'Edit ' : 'New ') + ONE_TITLE[type].toLowerCase(), form);
}

// ---- photos: the phone shrinks them, the ESP32 stores both files ----
const COMPRESSED_SIDE = 1024;           // px, longest side of the compressed copy
const COMPRESSED_QUALITY = 0.7;
const FULL_MAX_BYTES = 10 * 1024 * 1024;   // PHOTO_FULL_MAX in config.h; bigger photos are re-saved smaller
const FULL_TYPES = { 'image/jpeg': 'jpg', 'image/png': 'png', 'image/webp': 'webp' };

function loadImage(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => resolve({ img, done: () => URL.revokeObjectURL(url) });
    img.onerror = () => {
      URL.revokeObjectURL(url);
      reject(new Error('This phone cannot open that photo. Use a JPEG or PNG photo.'));
    };
    img.src = url;
  });
}

function toJpeg(img, maxSide, quality) {
  const scale = Math.min(1, maxSide / Math.max(img.naturalWidth, img.naturalHeight));
  const c = document.createElement('canvas');
  c.width = Math.max(1, Math.round(img.naturalWidth * scale));
  c.height = Math.max(1, Math.round(img.naturalHeight * scale));
  const g = c.getContext('2d');
  g.fillStyle = '#fff';                 // a PNG's see-through parts become white
  g.fillRect(0, 0, c.width, c.height);
  g.imageSmoothingQuality = 'high';
  g.drawImage(img, 0, 0, c.width, c.height);
  return new Promise((resolve, reject) => c.toBlob((b) => (b ? resolve(b) : reject(new Error('Could not compress the photo'))), 'image/jpeg', quality));
}

// The two files for one photo: the original (the save file) and a small copy.
async function preparePhoto(file) {
  const { img, done } = await loadImage(file);
  try {
    const compressed = await toJpeg(img, COMPRESSED_SIDE, COMPRESSED_QUALITY);
    let full = file, ext = FULL_TYPES[file.type];
    if (!ext || file.size > FULL_MAX_BYTES) { full = await toJpeg(img, 4096, 0.9); ext = 'jpg'; }
    return { compressed, full, ext, shrunk: full !== file, url: URL.createObjectURL(compressed), id: 0 };
  } finally {
    done();
  }
}

// POST /api/photos with a progress callback (fetch cannot report upload progress).
function uploadPhoto(p, onProgress) {
  return new Promise((resolve, reject) => {
    const fd = new FormData();
    fd.append('compressed', p.compressed, 'compressed.jpg');   // must come first (crop_photos.cpp)
    fd.append('full', p.full, 'full.' + p.ext);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/photos?user_id=' + encodeURIComponent(S.user));
    xhr.timeout = 180000;
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) onProgress(e.loaded / e.total); };
    xhr.onload = () => {
      let d = null;
      try { d = JSON.parse(xhr.responseText); } catch (_) {}
      if (xhr.status === 200 && d && d.ok) resolve(d.id);
      else reject(new Error((d && d.error) || 'Photo upload failed (HTTP ' + xhr.status + ')'));
    };
    xhr.onerror = () => reject(new Error('The photo upload was cut off. Check the Wi-Fi and save again.'));
    xhr.ontimeout = () => reject(new Error('The photo upload took too long. Save again.'));
    xhr.send(fd);
  });
}

// ids: photos the record already has (when editing). They stay on the card;
// removing one here only takes it off the record.
function photoPicker(max, ids = []) {
  const items = ids.map((id) => ({ id, url: photoUrl(id) }));
  const thumbs = el('div', { class: 'thumbs' });
  const status = el('p', { class: 'formhint' });
  const fileButton = (text, camera) => {
    const input = el('input', { type: 'file', accept: 'image/*', hidden: true });
    if (camera) input.setAttribute('capture', 'environment');
    input.addEventListener('change', async () => {
      const file = input.files[0];
      input.value = '';
      if (!file) return;
      status.textContent = 'Preparing the photo...';
      try {
        const p = await preparePhoto(file);
        items.push(p);
        status.textContent = 'Ready: original ' + kb(p.full.size) + (p.shrunk ? ' (made smaller to fit)' : '') +
          ', compressed copy ' + kb(p.compressed.size) + '. Both go to the SD card when you save.';
      } catch (err) {
        status.textContent = err.message;
      }
      draw();
    });
    return el('label', { class: 'photobtn' }, input, tr(text));
  };
  const buttons = el('div', { class: 'photobtns' }, fileButton('Take photo', true), fileButton('Choose photo', false));
  function draw() {
    thumbs.replaceChildren(...items.map((p, i) => el('div', { class: 'thumb' },
      el('img', { src: p.url, alt: 'Photo ' + (i + 1) }),
      el('button', { type: 'button', class: 'x', 'aria-label': 'Remove photo ' + (i + 1), onclick: () => {
        URL.revokeObjectURL(p.url);
        items.splice(i, 1);
        status.textContent = '';
        draw();
      } }, '×'))));
    buttons.hidden = items.length >= max;
  }
  draw();
  return {
    wrap: el('div', { class: 'fld wide' }, el('span', { class: 'flabel' }, 'Photos', el('span', { text: ' optional, up to ' + max }),
      bisaya ? el('span', { class: 'bis', lang: 'ceb', text: BIS.Photos }) : null), buttons, thumbs, status),
    // Uploads the photos not sent yet (a failed save keeps the ones already on the card).
    async upload(onText) {
      const ids = [];
      for (let i = 0; i < items.length; i++) {
        const p = items[i];
        if (!p.id) p.id = await uploadPhoto(p, (f) => onText('Sending photo ' + (i + 1) + ' of ' + items.length + ': ' + Math.round(f * 100) + '%'));
        ids.push(p.id);
      }
      return ids;
    },
    clear() { items.forEach((p) => URL.revokeObjectURL(p.url)); items.length = 0; draw(); },
  };
}

// ========================================================== recommend ==
function recipeMix(key) {
  const r = RECIPES[key];
  return 'Rice hull ' + r.hull + '%, compost ' + r.compost + '%, wood ash ' + r.ash + '%';
}

function renderRecommend(body) {
  if (!S.user) { body.append(farmerPrompt()); return; }
  if (S.recError) body.append(el('div', { class: 'note bad', text: S.recError }));
  if (!recordsReady()) { if (!S.recError) body.append(el('div', { class: 'empty', text: 'Loading...' })); return; }
  const plantings = newestByPlot('planting');
  const plots = [...plantings.keys()].sort((a, b) => a - b);
  if (!plots.length) {
    body.append(el('div', { class: 'empty', text: 'No crop is planted yet. Add a planting under Records to get a recommendation.' }));
    return;
  }
  const want = store.get('recPlot:' + S.user);
  const plot = plots.includes(want) ? want : plots[0];
  const planting = plantings.get(plot);
  const crop = cropFor(planting.crop);
  const soil = newestByPlot('soil').get(plot) || null;
  const month = new Date().getMonth() + 1;
  const normal = RAINFALL[month - 1];

  const select = el('select', { class: 'phsel', 'aria-label': 'Plot', onchange: (e) => { store.set('recPlot:' + S.user, e.target.value); render(); } },
    plots.map((p) => el('option', { value: p, text: 'Plot ' + p + ': ' + cropLabel(plantings.get(p).crop) })));
  select.value = plot;
  addKids(body,
    el('div', { class: 'glabel' }, el('span', {}, tr('Recommendation for'))),
    select,
    el('div', { class: 'fine' },
      el('b', { class: 'season-line', text: normal.name + ' at Maasin: ' + fmt1(normal.rainMm) + ' mm normal, ' + seasonForMonth(month) }),
      ' PAGASA 1991-2020'));
  if (!crop) {
    body.append(el('div', { class: 'note', text: '"' + planting.crop + '" is not in the Crop Guide, so there is no pH range to compare with. ' +
      'Add the planting again and pick its crop from the list.' }));
    return;
  }

  // Since the soil test (or since planting, with no test): the plot's
  // fertilizer records from that day on are counted, and the rain is typed
  // here (kept on this phone), starting at the Maasin normal for those days.
  const since = soil ? soil.date_tested : planting.date_planted;
  const apps = sorted('fertilizer').filter((f) => f.plot === plot && f.date_applied >= since).reverse();   // oldest first
  const fert = apps.map((f) => fertKey(f.fertilizer)).filter(Boolean);
  const key = 'rain:' + S.user + ':' + plot + ':' + since;
  const estimate = rainEstimateMm(since, today());
  const saved = parseFloat(store.get(key));
  const rain = stepper({ id: 'r-rain', name: 'rain', min: 0, max: 9990, step: 10, decimals: 0, start: () => estimate });
  rain.input.value = isNaN(saved) ? estimate : saved;
  const appList = apps.length
    ? el('ul', { class: 'applist' }, apps.map((f) => el('li', { text: f.date_applied + ' · ' + f.fertilizer +
        (f.amount_kg !== '' ? ' · ' + f.amount_kg + ' kg' : '') + (fertKey(f.fertilizer) ? '' : ' (not counted: no pH factor in the guide)') })))
    : el('p', { class: 'fine', text: 'No fertilizer recorded on plot ' + plot + ' since ' + since + '.' });

  const mass = stepper({ id: 'r-mass', name: 'mass', min: AGRI.doseMinG, max: AGRI.doseMaxG, step: 10, decimals: 0, start: () => 600 });   // 600 g: the middle of 300..900
  // The phone remembers the last mass. One saved under the old 100..300 g
  // range (before 1 Nov 2026) is out of range now, so it starts at 600 g.
  const savedMass = parseFloat(store.get('doseMass'));
  mass.input.value = savedMass >= AGRI.doseMinG && savedMass <= AGRI.doseMaxG ? savedMass : 600;
  const out = el('div', { class: 'rgroup' });            // the recipe cards
  const splitHead = el('span', { class: 'k' });
  const splitText = el('p');
  const keys = el('div', { class: 'keys', 'aria-label': 'Keys to press' });
  const keysNote = el('p', { class: 'fine' });
  const why = el('div', { class: 'rgroup' });            // Why? and the closing line

  const update = () => {
    const rainMm = Math.max(0, parseFloat(rain.input.value) || 0);
    store.set(key, String(rainMm));
    const r = rankRecipes({ crop, soilTestPH: soil ? parseFloat(soil.soil_ph) : null, soilTestDate: soil ? soil.date_tested : null,
      fertSinceTest: fert, rainMmSinceTest: rainMm, month });
    const rec = r.recommended.recipe;
    const grams = Math.round(Math.max(AGRI.doseMinG, Math.min(AGRI.doseMaxG, parseFloat(mass.input.value) || 600)));
    store.set('doseMass', String(grams));
    const split = splitDoseDg(rec, grams * 10);
    out.replaceChildren(...addKids(document.createDocumentFragment(),
      r.noTest ? el('div', { class: 'note' }, 'No soil test yet. Using the default pH ' + fmt1(AGRI.defaultPH) +
        ' for Leyte soils. A soil test is strongly recommended.', bisOf('No soil test yet')) : null,
      el('div', { class: 'reccard main' },
        el('span', { class: 'k' }, tr('Recommended')),
        el('span', { class: 'r', text: 'Recipe ' + rec }),
        el('p', { text: r.recommended.reason }),
        el('span', { class: 'fine', text: recipeMix(rec) })),
      r.others.map((o) => el('div', { class: 'reccard' + (o.notAdvised ? ' no' : '') },
        el('span', { class: 'k' }, tr(o.notAdvised ? 'Not advised' : 'Also possible')),
        el('span', { class: 'r', text: 'Recipe ' + o.recipe }),
        el('p', { text: o.reason }),
        el('span', { class: 'fine', text: recipeMix(o.recipe) })))).childNodes);
    splitHead.replaceChildren('Dose split: ' + grams + ' g', bisOf('Dose split') || '');
    splitText.textContent = 'Recipe ' + rec + ': rice hull ' + g1(split.hull) + ' g, wood ash ' +
      g1(split.ash) + ' g, compost ' + g1(split.compost) + ' g';
    // The keypad flow in AgriDose_ESP32_Link.ino: HOME # > recipe > # > grams > # > # starts.
    const cap = (t, go) => el('span', { class: 'key' + (go ? ' go' : ''), text: t });   // one keypad key
    keys.replaceChildren(cap('#', true), cap(rec), cap('#', true), ...String(grams).split('').map((d) => cap(d)), cap('#', true), cap('#', true));
    keysNote.textContent = '# new dose, ' + rec + ' the recipe, # OK, ' + grams + ' the grams, # OK, # start. ' +
      'Press * to stop a dose, D to go back to the start.';
    const wasOpen = !!$('details', why) && $('details', why).open;   // keep Why? open while the farmer changes numbers
    why.replaceChildren(...addKids(document.createDocumentFragment(),
      el('details', { class: 'why reccard', open: wasOpen },
        el('summary', {}, tr('Why?')),
        el('ol', {}, r.steps.map((s) => el('li', { text: s }))),
        el('p', { class: 'fine', text: 'Demo values: ' + fmt2(AGRI.fertDropPerApp) + ' pH per fertilizer unit, ' + fmt2(AGRI.rainDropPer100mm) +
          ' pH per 100 mm of rain, rule thresholds ' + fmt1(AGRI.mildGap) + ' and ' + fmt1(AGRI.driftDropPH) + ' pH, wet month at ' +
          AGRI.wetMonthMinMm + ' mm.' })),
      el('p', { class: 'fine', text: 'You choose the recipe at the machine. The app only recommends.' })).childNodes);
  };
  for (const input of [rain.input, mass.input]) input.addEventListener('input', update);

  addKids(body,
    el('div', { class: 'reccard' },
      el('span', { class: 'k', text: soil ? 'Since the soil test (pH ' + soil.soil_ph + ', ' + soil.date_tested + ')' : 'Since planting (no soil test yet)' }),
      el('div', { class: 'glabel' }, el('span', {}, tr('Fertilizer')),
        el('button', { type: 'button', class: 'addbtn', onclick: () => openForm('fertilizer', { plot }) }, tr('+ Add fertilizer'))),
      appList,
      el('div', { class: 'fld' }, el('label', { for: 'r-rain' }, tr('Rain (mm)')), rain.wrap),
      el('p', { class: 'formhint', text: 'Rain starts at ' + estimate + ' mm, the Maasin normal for the ' + Math.max(0, daysBetween(since, today())) +
        ' days since ' + since + '. Change it if you measured the rain.' })),
    out,
    el('div', { class: 'reccard' },
      splitHead,
      el('div', { class: 'fld' }, el('label', { for: 'r-mass', text: 'Grams for one dose (' + AGRI.doseMinG + ' to ' + AGRI.doseMaxG + ')' }), mass.wrap),
      splitText,
      el('span', { class: 'k' }, tr('At the machine\'s keypad')),
      keys, keysNote,
      el('span', { class: 'fine', text: 'The machine doses ' + AGRI.doseMinG + ' to ' + AGRI.doseMaxG + ' g, whole grams.' })),
    why);
  update();
}

// ========================================================= crop guide ==
function renderCrops(body) {
  const rows = CROPS.map((c) => [
    el('tr', { class: 'has-src' }, el('td', { text: c.name }), el('td', { text: c.local }),
      el('td', { class: 'num', text: fmt1(c.phMin) }), el('td', { class: 'num', text: fmt1(c.phMax) })),
    // 1 Nov 2026: no "verify" tag here any more (c.verify stays in agri.js for the citations)
    el('tr', { class: 'src' }, el('td', { colspan: 4 }, 'Source: ' + c.source)),
  ]);
  addKids(body,
    el('div', {}, el('div', { class: 'rtitle' }, tr('Crop Guide')), el('div', { class: 'rsub', text: CROPS.length + ' crops. Soil pH ranges used by Recommend.' })),
    el('table', { class: 'ctable' },
      el('thead', {}, el('tr', {}, el('th', {}, tr('Crop')), el('th', {}, tr('Local name')), el('th', { class: 'num', text: 'pH min' }), el('th', { class: 'num', text: 'pH max' }))),
      el('tbody', {}, rows)),
    el('p', { class: 'fine', text: 'Ranges from Philippine DA production guides where available.' }));
}

// ========================================================== batch log ==
let dosesLoading = false;
async function loadDoses() {
  if (dosesLoading) return;
  dosesLoading = true;
  try {
    S.doses = await getJson('/api/doses?limit=' + S.doseLimit);
    S.doseError = '';
  } catch (err) {
    S.doseError = 'Could not load the dose log: ' + err.message;
  } finally {
    dosesLoading = false;
  }
  if (S.tab === 'batches') render();
  else updateMachine();
}

// d.time is "2026-11-01 14:05", or "" for a dose made while the ESP32 had no
// time yet (no phone had opened this page since it started).
function doseTime(d) {
  const since = 'ESP32 boot ' + d.boot + ', ' + Math.floor(d.uptime_s / 60) + ' min after it started';
  return d.time ? d.time + ' (' + since + ')' : 'Time not known (' + since + ')';
}

function openDose(d) {
  const err = d.actual_dg - d.target_dg;
  const split = splitDoseDg(d.recipe, d.target_dg);
  const names = { hull: 'Rice hull', ash: 'Wood ash', compost: 'Compost' };
  showSheet('Dose ' + d.no,
    el('dl', { class: 'fields' },
      el('dt', { text: 'Dose number' }), el('dd', { text: String(d.no) }),
      el('dt', { text: 'Recipe' }), el('dd', { text: d.recipe + ': ' + recipeMix(d.recipe).toLowerCase() }),
      el('dt', { text: 'Target' }), el('dd', { text: g1(d.target_dg) + ' g' }),
      el('dt', { text: 'Actual' }), el('dd', { text: g1(d.actual_dg) + ' g' }),
      el('dt', { text: 'Error' }), el('dd', { text: signedG(err) + ' g (' + pctNumber(err, d.target_dg) + ' %)' }),
      el('dt', { text: 'When' }), el('dd', { text: doseTime(d) })),
    el('table', { class: 'ctable' },
      el('thead', {}, el('tr', {}, el('th', { text: 'Feedstock' }), el('th', { class: 'num', text: 'Target g' }))),
      el('tbody', {}, ['hull', 'ash', 'compost'].map((k) => el('tr', {}, el('td', { text: names[k] }), el('td', { class: 'num', text: g1(split[k]) }))))),
    el('p', { class: 'fine', text: 'The machine reports the total actual mass only, not each hopper. The ESP32 takes its time from the first phone that opens this page after it starts, so a dose made before that has no time.' }));
}

function renderBatches(body) {
  addKids(body,
    el('div', {}, el('div', { class: 'rtitle' }, tr('Batch Log')), el('div', { class: 'rsub', text: 'From the machine\'s microSD log. Newest first.' })),
    el('a', { class: 'button', href: '/api/doses.csv', download: 'agridose_dose_log.csv' }, tr('Download CSV')));
  if (S.doseError) body.append(el('div', { class: 'note bad', text: S.doseError }));
  if (!S.doses) { if (!S.doseError) body.append(el('div', { class: 'empty', text: 'Loading...' })); return; }
  if (!S.doses.items.length) body.append(el('div', { class: 'empty', text: 'No doses logged yet. Doses are made at the machine\'s keypad.' }));
  for (const d of S.doses.items) {
    const err = d.actual_dg - d.target_dg;
    body.append(el('div', { class: 'rec' },
      el('div', { class: 'rmain' },
        el('b', { text: 'Dose ' + d.no + ' · Recipe ' + d.recipe }),
        el('span', { text: 'Target ' + g1(d.target_dg) + ' g · actual ' + g1(d.actual_dg) + ' g · error ' + pctNumber(err, d.target_dg) + '%' }),
        // 1 Nov 2026: when the dose finished (the ESP32's clock, set by a phone)
        el('span', { class: 'when', text: d.time || 'Time not known' })),
      el('button', { type: 'button', class: 'vd', onclick: () => openDose(d) }, tr('View details'))));
  }
  if (S.doses.items.length) {
    body.append(el('p', { class: 'fine', text: 'Showing ' + S.doses.items.length + ' of ' + S.doses.total + '.' }));
  }
  if (S.doses.items.length < S.doses.total && S.doseLimit < 200) {
    body.append(el('button', { type: 'button', class: 'linkbtn', onclick: () => { S.doseLimit = 200; loadDoses(); } }, 'Show more'));
  }
}

// =============================================================== start ==
// A tab can also be opened by address: http://192.168.4.1/#records
function tabFromHash() { const t = location.hash.slice(1); return TABS[t] ? t : null; }
window.addEventListener('hashchange', () => { if (tabFromHash()) showTab(tabFromHash()); });

// Sun mode (bright, high contrast) and the Bisaya lines, both remembered on this phone.
function applyLook() {
  const light = store.get('theme') === 'light';
  if (light) document.documentElement.dataset.theme = 'light';
  else delete document.documentElement.dataset.theme;
  $('meta[name="theme-color"]').content = light ? '#FFFFFF' : '#26282D';
  $('#sun').setAttribute('aria-pressed', String(light));
  $('#lang').setAttribute('aria-pressed', String(bisaya));
  for (const [id, en] of [['sheet-close', 'Close'], ['viewer-close', 'Close'], ['viewer-full', 'Save original'], ['viewer-small', 'Save compressed']]) {
    setText($('#' + id), en);
  }
}
$('#sun').addEventListener('click', () => {
  store.set('theme', store.get('theme') === 'light' ? 'dark' : 'light');
  applyLook();
  toast(store.get('theme') === 'light' ? 'Sun mode on' : 'Sun mode off');
});
$('#lang').addEventListener('click', () => {
  bisaya = !bisaya;
  store.set('bisaya', bisaya ? 'on' : 'off');
  applyLook();
  render();
});

applyLook();
connect();
S.tab = tabFromHash() || (TABS[store.get('tab')] ? store.get('tab') : 'machine');
showTab(S.tab);
loadMe();
