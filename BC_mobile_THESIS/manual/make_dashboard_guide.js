// To rebuild the dashboard guide after a change (Node 18+):
//   npm install docx@9      (once, in this folder or any other)
//   node make_dashboard_guide.js AgriDose_Dashboard_Guide.docx
// Keep the facts in step with the firmware (AgriDose_ESP32_Link: config.h,
// web_server.cpp, www/app.js) and README.md sections 2 to 5.
// The pictures in dashboard_shots/ are from `node tests/test_dashboard.mjs`
// (the laptop mock, made-up data); copy new ones from %TEMP%\agridose-dashboard-shots\shots.
//
// Written 10 Nov 2026. Same look as AgriDose_Operation_Manual.docx (make_manual.js).
// Passwords are left out on purpose: the guide points to the sticker and config.h.
const fs = require('fs');
const path = require('path');
const {
  Document, Packer, Paragraph, TextRun, Table, TableRow, TableCell, WidthType, ShadingType,
  BorderStyle, AlignmentType, HeadingLevel, LevelFormat, Footer, PageNumber, ImageRun, VerticalAlign,
} = require('docx');

const OUT = process.argv[2] || 'AgriDose_Dashboard_Guide.docx';
const SHOTS = path.join(__dirname, 'dashboard_shots');
const GREEN = '2E5E2E', NOTE = 'FFF4D6', BOXFILL = 'EAF2E3';
const FULL = 9360;                       // US Letter, 1-inch margins

// ---------- helpers (as in make_manual.js) ----------
const run = (t, o = {}) => new TextRun({ text: t, ...o });
function rich(parts) {                   // "plain **bold** `code`" -> runs
  const out = [];
  const re = /(\*\*[^*]+\*\*|`[^`]+`)/g;
  let last = 0, m;
  while ((m = re.exec(parts))) {
    if (m.index > last) out.push(run(parts.slice(last, m.index)));
    const s = m[0];
    if (s.startsWith('**')) out.push(run(s.slice(2, -2), { bold: true }));
    else out.push(run(s.slice(1, -1), { font: 'Consolas', size: 20 }));
    last = m.index + s.length;
  }
  if (last < parts.length) out.push(run(parts.slice(last)));
  return out;
}
const P = (t, o = {}) => new Paragraph({ children: rich(t), spacing: { after: 120 }, ...o });
const H1 = (t, newPage = true) => new Paragraph({ heading: HeadingLevel.HEADING_1, children: [run(t)], pageBreakBefore: newPage });
const H2 = (t) => new Paragraph({ heading: HeadingLevel.HEADING_2, children: [run(t)] });
const H3 = (t) => new Paragraph({ heading: HeadingLevel.HEADING_3, children: [run(t)] });
const bullets = (items) => items.map((t) => new Paragraph({ numbering: { reference: 'bullets', level: 0 }, children: rich(t), spacing: { after: 60 } }));
let listNo = 0;
const numberRefs = [];
function steps(items) {                  // a numbered list that starts again at 1
  const ref = 'steps' + (++listNo);
  numberRefs.push(ref);
  return items.map((t) => new Paragraph({ numbering: { reference: ref, level: 0 }, children: rich(t), spacing: { after: 60 } }));
}
const note = (t) => new Paragraph({
  shading: { type: ShadingType.CLEAR, fill: NOTE, color: 'auto' },
  border: { left: { style: BorderStyle.SINGLE, size: 24, color: 'D9A400', space: 8 } },
  indent: { left: 120, right: 120 }, spacing: { before: 120, after: 160 },
  children: [run('Note: ', { bold: true }), ...rich(t)],
});

const border = { style: BorderStyle.SINGLE, size: 4, color: 'B7C4AE' };
const borders = { top: border, bottom: border, left: border, right: border };
const none = { style: BorderStyle.NONE, size: 0, color: 'FFFFFF' };
const noBorders = { top: none, bottom: none, left: none, right: none };
function cell(content, width, o = {}) {
  const paras = (Array.isArray(content) ? content : [content]).map((c) =>
    c instanceof Paragraph ? c : new Paragraph({ children: rich(String(c)), spacing: { after: 40 }, alignment: o.align }));
  return new TableCell({
    width: { size: width, type: WidthType.DXA }, borders: o.borders || borders,
    margins: { top: 80, bottom: 80, left: 110, right: 110 }, verticalAlign: o.valign,
    shading: o.fill ? { type: ShadingType.CLEAR, fill: o.fill, color: 'auto' } : undefined,
    children: paras,
  });
}
function table(widths, header, rows) {
  const total = widths.reduce((a, b) => a + b, 0);
  const head = new TableRow({ tableHeader: true, children: header.map((h, i) =>
    cell(new Paragraph({ children: [run(h, { bold: true, color: 'FFFFFF' })] }), widths[i], { fill: GREEN })) });
  const body = rows.map((r, k) => new TableRow({ cantSplit: true, children: r.map((c, i) => cell(c, widths[i], { fill: k % 2 ? 'F6F9F3' : undefined })) }));
  return [new Table({ width: { size: total, type: WidthType.DXA }, columnWidths: widths, rows: [head, ...body] }),
          new Paragraph({ children: [], spacing: { after: 120 } })];
}
// A row of phone screenshots with captions under them.
let figNo = 0;
function figures(items) {                // [[file, caption], ...], three across
  const w = Math.floor(FULL / 3);
  const widths = [w, w, FULL - 2 * w];
  const cells = items.map(([file, caption], i) => {
    const data = fs.readFileSync(path.join(SHOTS, file));
    const px = { w: data.readUInt32BE(16), h: data.readUInt32BE(20) };   // PNG header
    const wide = 190;
    return cell([
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 60 },
        children: [new ImageRun({ type: 'png', data, transformation: { width: wide, height: Math.round(wide * px.h / px.w) },
          altText: { title: caption, description: 'Dashboard screenshot: ' + caption, name: file } })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 0 },
        children: [run('Figure ' + (++figNo) + '. ', { bold: true, size: 18, color: '555555' }), run(caption, { size: 18, color: '555555' })] }),
    ], widths[i], { borders: noBorders });
  });
  return [new Table({ width: { size: FULL, type: WidthType.DXA }, columnWidths: widths, rows: [new TableRow({ cantSplit: true, children: cells })] }),
          new Paragraph({ children: [], spacing: { after: 120 } })];
}
// A box of the block diagram.
const block = (title, lines, width) => cell([
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 40 }, children: [run(title, { bold: true, color: GREEN })] }),
  ...lines.map((l) => new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 20 }, children: [run(l, { size: 18 })] })),
], width, { fill: BOXFILL, valign: VerticalAlign.CENTER });
const arrow = (label, width) => cell([
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 0 }, children: [run('↔', { size: 32, bold: true, color: GREEN })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 0 }, children: [run(label, { size: 16, color: '555555' })] }),
], width, { borders: noBorders, valign: VerticalAlign.CENTER });

// ---------- content ----------
const kids = [];
const add = (...xs) => xs.flat().forEach((x) => kids.push(x));

// Title page
add(
  new Paragraph({ spacing: { before: 2400, after: 200 }, alignment: AlignmentType.CENTER, children: [run('AgriDose', { bold: true, size: 72, color: GREEN })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 120 }, children: [run('Solid Soil-Amendment Dosing Machine', { size: 32 })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 300 }, children: [run('Web Dashboard Guide', { size: 40, bold: true })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 1200 }, children: [run('What the ESP32 dashboard shows, who can use it, and how it works inside', { size: 24, color: '555555' })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 1600, after: 80 }, children: [run('Version of 10 November 2026', { size: 22 })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 }, children: [run('Firmware: AgriDose_ESP32_Link (10 Nov 2026)', { size: 20, color: '555555' })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, children: [run('Screenshots: the laptop test, with made-up data', { size: 20, color: '555555' })] }),
);

// Contents
add(H1('Contents'));
[
  '1. What the dashboard is', '2. Opening it on a phone', '3. The screen at a glance', '4. The five tabs',
  '5. Who can see what', '6. Why the dashboard cannot start a dose', '7. How it works inside',
  '8. Files on the microSD card', '9. For developers', '10. Limits and what still needs testing',
].forEach((t) => add(new Paragraph({ children: [run(t)], spacing: { after: 80 } })));

// 1
add(H1('1. What the dashboard is'));
add(P('The AgriDose dashboard is a web page served by the machine\'s ESP32 board. The ESP32 makes its own Wi-Fi network, **AgriDose**, and any phone that joins it can open the page. No internet, SIM card or cloud service is involved: the page, the live readings and the records all come from the ESP32 and its microSD card.'));
add(P('It does three jobs:'));
add(bullets([
  '**Watch the machine live:** the hopper weights, the dose in progress, faults, and whether there is enough stock for the next dose.',
  '**Keep crop records** for each farmer: soil tests, plantings, growth and fertilizer, with photos.',
  '**Recommend a recipe** (A, B or C) for a plot, from its latest soil test, the crop and the rain.',
]));
add(P('It only **watches** the machine. Doses are started, stopped and answered at the machine\'s keypad, never from a phone (section 6).'));
add(H2('Key facts'));
add(table([2600, 6760], ['Item', 'Value'], [
  ['Wi-Fi network', '**AgriDose**, with a password. The password is on the QR sticker on the machine and in `config.h`.'],
  ['Address', '`http://192.168.4.1/`'],
  ['Phones at once', 'Up to 5 on the Wi-Fi, and up to 6 live connections.'],
  ['Internet', 'Not needed.'],
  ['Live updates', 'Sent to every phone when something changes: at most 4 times a second, and at least once a second.'],
  ['The page', 'About 37 KB, stored in the ESP32\'s flash memory. The ESP32 serves every file itself.'],
  ['Where data is kept', 'The microSD card in the ESP32: dose log, farmers, crop records, photos, logins.'],
  ['Languages', 'English, with Bisaya (Cebuano) lines under the labels. The Bisaya is a first draft for the team to check.'],
  ['On the phone', 'Any phone browser, or the AgriDose Android app (the same page, full screen).'],
]));

// 2
add(H1('2. Opening it on a phone'));
add(P('The machine carries a sticker with two QR codes.'));
add(steps([
  'Scan **code 1** with the phone\'s camera. It joins the Wi-Fi network **AgriDose**.',
  'The dashboard usually opens by itself, like the login page of hotel Wi-Fi. If Android says the network has no internet, tap **Stay connected**.',
  'If the page does not open, scan **code 2**. It opens `http://192.168.4.1/` in Chrome.',
  'Optional: in Chrome, menu > **Add to Home screen**. This makes a shortcut that opens in Chrome. It works without internet, because the ESP32 serves every file.',
]));
add(H2('The AgriDose Android app'));
add(P('The Android app opens the same page full screen with its own icon. It loads the page from the ESP32 every time, so a change to the dashboard never needs a new app. It also keeps working with mobile data switched on, which Chrome often does not: when the Wi-Fi has no internet, Android sends Chrome\'s traffic over mobile data instead.'));
add(P('The app is built automatically by GitHub Actions, from `github.com/mousewanderer/THESIS_APP_forthis`.'));
add(note('**Take photo** does not respond inside the Android app yet (not solved). Photos have not been tried on the machine yet.'));

// 3
add(H1('3. The screen at a glance', false));
add(P('The page follows the team\'s phone-app design: a dark screen with a lime accent and five tabs along the bottom. Figure 1 on the next page shows it.'));
add(table([2600, 6760], ['Part', 'What it does'], [
  ['Title', '"Solid Amendment Project".'],
  ['Lime dot', 'Lit while the Mega (the board that weighs and doses) is sending data.'],
  ['Bell', 'A red dot means there is an alert. Tap it to read it (table below).'],
  ['Name, or "Log in"', 'Who is logged in on this phone. Tap it to log in, change your password or log out. The admin also chooses a farmer here.'],
  ['Sun button', 'A bright black-on-white look for reading in sunlight. Each phone remembers it.'],
  ['Bisaya button', 'Shows or hides the Bisaya lines. On at first; each phone remembers it.'],
  ['Tabs', '**Machine**, **Records**, **Recommend**, **Crop Guide**, **Batch Log**.'],
]));
add(H2('Alerts under the bell'));
add(table([3400, 5960], ['Alert', 'What to do'], [
  ['Not connected to the AgriDose Wi-Fi', 'Join the AgriDose network again (QR code 1).'],
  ['Mega offline: no data from the machine for 2 seconds', 'Check that the machine is on. If it stays, check the cable between the two boards.'],
  ['Fault at the machine. The dose is paused.', 'Read the machine\'s screen, fix the cause, then press # on the keypad to continue, or * to end the dose.'],
  ['The power went off during a dose', 'Press # on the keypad to continue the dose, or * to end it.'],
  ['SD card problem', 'Doses, records and photos are not being saved. Check the microSD card.'],
  ['Hopper B (compost) is low: 150 g left, under 200 g (added 10 Nov)', 'One line for each hopper holding less than 200 g. Refill it before the next dose; the dot goes once it holds 200 g or more. Only a warning: whether a dose fits is still the keypad\'s stock check.'],
]));

// 4
add(H1('4. The five tabs'));
add(figures([['machine.png', 'Machine'], ['records.png', 'Records'], ['recommend.png', 'Recommend']]));
add(H2('4.1 Machine'));
add(P('Open to anyone on the Wi-Fi; no login.'));
add(table([2600, 6760], ['Panel', 'What it shows'], [
  ['Weight dispensed', 'The grams dosed so far in the current dose (all hoppers added), on a 0 to 900 g gauge.'],
  ['Machine Status', '**Idle** (ready), **Dosing**, **Draining** (added 10 Nov: a hopper is being emptied from the keypad\'s D menu; * stops it), **Fault** (check it, then # or * on the keypad), **Waiting** (an unfinished dose after a power cut, or the Mega not heard from yet), **Offline** (no data from the Mega), **No link** (this phone is not on the AgriDose Wi-Fi).'],
  ['Hopper A, B, C Weight', 'What is in each hopper, on 0 to 10 kg gauges: A rice hull, B compost, C wood ash. The Mega sends a new weight only when one moves more than 5 g, so small scale noise does not flicker.'],
  ['Total Weight', 'The three hoppers added.'],
  ['Enough stock for (added 10 Nov)', 'For each recipe, the biggest dose the hoppers can give. Each hopper must hold its share of the dose plus a 20 g spare. When a recipe is limited it names the hopper, for example “up to 557 g: compost (Hopper B) is low”, or in red “not even 300 g: refill wood ash (Hopper C)”. The keypad uses the same rule and shows **LOW COMPOST** before a dose that needs more.'],
  ['Last dose', 'Recipe, target, actual mass, and the error in grams and per cent.'],
  ['Bottom line', 'How many times the ESP32 has started, how long it has run, its clock, its free memory (and the lowest since start-up), and how many phones are on the live feed.'],
]));
add(H2('4.2 Records'));
add(P('Needs a login. A farmer sees only their own records; the admin chooses a farmer by tapping the name in the header. There are four kinds of record, newest first, three of each shown until **Show all** is tapped.'));
add(table([2600, 6760], ['Kind', 'What it holds'], [
  ['Soil tests', 'Plot, date tested, soil pH, soil moisture %, photos.'],
  ['Seed type and planting', 'Plot, crop, variety, date planted, whether AgriDose was used and with which doses (dose numbers from the Batch Log), photos.'],
  ['Growth records', 'Plot, growth stage, date observed, date planted, days after planting, height (cm), leaf count, notes, photos.'],
  ['Fertilizer applications', 'Plot, date applied, fertilizer, amount (kg), notes.'],
]));
add(bullets([
  '**+ Add** opens a form. Number boxes have - and + buttons; on an empty box the first tap fills in the last value saved. Seed type and growth stage are drop-down lists with “Other” for a new one. A growth record starts from its planting, which fills in the plot and date planted and counts the days.',
  '**View details** shows every field and the photos. **Edit** corrects a record in place, so no duplicate is added. **Delete** removes one record and its photos, after a second tap to confirm.',
  '**Photos:** up to 3 per record, from the camera or the gallery. The phone makes a small copy for the page (at most 1024 pixels wide, usually 50 to 200 KB); the original is kept on the card too. Tap a photo to see it full size or save either copy.',
  '**Download records (CSV)** saves each file for a spreadsheet. The admin gets every farmer\'s rows and the farmers list; a farmer gets only their own rows.',
]));
add(H3('The ESP32 checks every save'));
add(P('The phone checks the form, and the ESP32 checks it again before it writes anything. If it refuses, the reason shows on the form.'));
add(bullets([
  'No two farmers with the same name (capitals and spaces are ignored).',
  'No second record of the same kind for one farmer with the same plot and date (for fertilizer: and the same fertilizer). Correct the first one with Edit instead.',
  'No date after today.',
  'A growth record needs a planting on that plot, and must be observed on or after the date planted.',
  'A planting\'s dose numbers must exist in the Batch Log.',
  'If another phone changed the record since you opened it, the save is refused and you are asked to open it again.',
  'A planting cannot be deleted while its plot still has growth records and no other planting.',
]));
add(H2('4.3 Recommend'));
add(P('Needs a login. Pick a planted plot, and the phone ranks recipes **A**, **B** and **C** for it, using the rules and worked examples from the team\'s recipe design. It takes into account:'));
add(bullets([
  'the plot\'s newest soil test (its pH);',
  'the crop\'s pH range, from the Crop Guide;',
  'fertilizer applied since the soil test: urea, ammonium sulfate and complete 14-14-14 lower the pH; other fertilizers are listed but not counted. **+ Add fertilizer** saves one;',
  'rain since the soil test, which starts at the normal for Maasin (PAGASA 1991-2020) and can be changed to what was measured.',
]));
add(P('It shows the recommended recipe and why, the other two (C is “Not advised” when the soil is already above the crop\'s range), the split for a dose of 300 to 900 g, the **keys to press at the machine** (for example `# B # 6 0 0 # #`), and **Why?** with every step of the working.'));
add(note('Nothing is sent to the machine. The recipe is still chosen at the keypad. The calculation runs on the phone, so it does not slow down the ESP32.'));
add(H2('4.4 Crop Guide'));
add(P('Open to anyone. The 12 crops, with their local names, the soil pH range each one likes, and the sources.'));
add(H2('4.5 Batch Log'));
add(P('Open to anyone. Every dose the machine has made, from the dose log on the microSD card, newest first: dose number, recipe, target, actual mass, error, and the date and time (or “Time not known”, see below). It shows the newest 20; **Show more** shows up to 200. **View details** gives the planned split per material, and **Download CSV** saves the whole log.'));
add(figures([['crop_guide.png', 'Crop Guide'], ['batch_log.png', 'Batch Log'], ['sun_mode.png', 'Records in sun mode']]));
add(H2('4.6 The clock'));
add(P('The ESP32 has no clock battery, so it takes the time from a phone. Any phone sets it while the ESP32 has no time (after every restart), and a logged-in phone corrects it if it is more than 2 minutes off. A dose made after a restart, before any phone has opened the dashboard, has no time in the log. A battery-backed clock module (DS3231) would close this gap; none is fitted.'));

// 5
add(H1('5. Who can see what'));
add(table([3160, 2000, 2000, 2200], ['What', 'Anyone on the Wi-Fi', 'A farmer', 'The admin'], [
  ['Machine tab, Batch Log, Crop Guide', 'Yes', 'Yes', 'Yes'],
  ['Records and photos', 'No', 'Their own', 'Every farmer'],
  ['Recommend', 'No', 'Their own plots', 'Any farmer\'s plots'],
  ['Download records (CSV)', 'No', 'Their own rows', 'Whole files and the farmers list'],
  ['Add, rename or delete farmers; set their logins', 'No', 'No', 'Yes'],
  ['Start, stop or answer a dose', 'No', 'No', 'No: only the keypad'],
]));
add(H2('Logins and passwords'));
add(bullets([
  'The first admin account is set in `config.h`. At its first login the app allows nothing but choosing a new password.',
  'No password is stored anywhere. The card holds only a scrambled form of it (a PBKDF2-SHA256 hash, 10,000 rounds, with its own random salt), in the `/auth/` folder, and no web address ever sends those files.',
  'Five wrong passwords in a row lock that username for 5 minutes.',
  'A login lasts 12 hours on a phone. Restarting the ESP32 logs every phone out.',
  '**Forgot the admin password?** Put the card in a laptop and delete `/auth/admin.csv`. At the next start-up the first admin comes back (and must be changed again). The farmers\' logins stay.',
  '**Deleting a farmer** (admin, two taps; the first shows what will be lost) removes the farmer, their login, records and photos. It cannot be undone. The Batch Log stays.',
]));
add(note('The page uses plain HTTP, not HTTPS. The Wi-Fi password is what keeps strangers from watching the machine; the records also need a login.'));

// 6
add(H1('6. Why the dashboard cannot start a dose'));
add(P('A machine that moves material should only move when someone standing at it says so. The dashboard is therefore look-only by design, and the code is built so that this is hard to break by accident:'));
add(bullets([
  'Doses, zeroing, STOP and RESUME reach the Mega through one function only, `linkRequest()`, and only the keypad code calls it.',
  'The web server\'s code never includes the link code, so a web handler that tried to call it would not even compile.',
  'Only the link task writes to the cable to the Mega.',
  'Anything a phone sends on the live connection is ignored.',
  'The only things the web side writes are farmers, crop records, photos and the clock, all on the ESP32 itself.',
]));
add(P('To stop a dose, press * on the keypad. A STOP button on the phone was left out on purpose: over Wi-Fi it might arrive late or not at all, and a stop that might not arrive is not a safety stop.'));

// 7
add(H1('7. How it works inside', false));
add(H2('The big picture'));
const bw = [2400, 1100, 2800, 1100, 1960];
add(new Table({ width: { size: FULL, type: WidthType.DXA }, columnWidths: bw, rows: [new TableRow({ cantSplit: true, children: [
  block('Phone', ['the page (HTML, JavaScript)', 'draws the screens,', 'works out Recommend,', 'shrinks photos'], bw[0]),
  arrow('Wi-Fi', bw[1]),
  block('ESP32', ['web server and live status', 'link task (high priority)', 'keypad and menus', 'microSD card: all files'], bw[2]),
  arrow('cable, 57,600 baud', bw[3]),
  block('Mega', ['3 scales', '3 augers', 'the LCD'], bw[4]),
] })] }), new Paragraph({ children: [], spacing: { after: 120 } }));
add(P('The heavy work happens on the phone: the recipe recommendation, sorting lists, counting days and shrinking photos. The ESP32 only stores files, checks them, and passes on what the Mega reports.'));
add(H2('The parts on the ESP32'));
add(table([2300, 2700, 4360], ['Part', 'File', 'Job'], [
  ['The page', '`www/` (index.html, app.js, agri.js, app.css), packed into `web_assets.h`', 'Everything you see. Built into the firmware, so it works without internet.'],
  ['Web server', '`web_server.cpp`', 'Serves the page, answers the `/api/` addresses, and pushes the live status on the WebSocket `/ws`.'],
  ['Live status', '`live_state.cpp`', 'The latest weight, state, hopper weights and last dose, shared safely between the tasks.'],
  ['Mega link task', '`mega_link.cpp`', 'Reads the cable from the Mega in its own task, at a higher priority than the web server, so web traffic can never hold it up. Every frame has a check code (CRC).'],
  ['SD card', '`sd_store.cpp`', 'Every card access, one at a time, in small pieces so a download never blocks the dose log.'],
  ['Records, photos, logins, dose log, clock', '`crop_records`, `crop_photos`, `accounts`, `dose_log`, `wall_clock`', 'The files on the card and their rules.'],
  ['Stock check', '`stock_check.h` (10 Nov)', 'Whether the hoppers hold enough for a dose; used by the keypad and the Machine tab.'],
]));
add(H2('How a weight reaches your phone'));
add(steps([
  'The Mega weighs. It sends **WT** (grams dosed so far, and the state) every 200 ms, and **HW** (the three hopper weights) when one moves more than 5 g, and every 10 s anyway.',
  'The ESP32\'s link task checks each frame\'s check code and updates the live status.',
  'Every 250 ms the web server compares the live status with what it sent last. If something changed, or a second has passed, it sends one short JSON message to every phone on `/ws`.',
  'The page redraws the gauges. If the connection drops (the phone slept, or the Wi-Fi went), it reconnects by itself.',
]));
add(H2('What the live message holds'));
add(table([2600, 6760], ['Field', 'Meaning'], [
  ['`online`', 'A frame came from the Mega in the last 2 seconds.'],
  ['`flag`', 'The machine\'s state: I idle, D dosing, F paused by a fault, R asking about a dose cut off by a power cut, E draining a hopper (10 Nov).'],
  ['`weight_dg`', 'Grams dosed so far in this dose, in tenths of a gram (6000 = 600.0 g).'],
  ['`hoppers_dg`', 'What is in hoppers A, B and C, in tenths of a gram.'],
  ['`stock_max_g`, `stock_short`', 'For recipes A, B and C: the biggest dose the stock covers (0 = not even 300 g), and which hoppers limit it (10 Nov).'],
  ['`last`', 'The last dose: recipe, target and actual mass.'],
  ['`clock_set`, `epoch`', 'Whether the ESP32 has the time, and the time.'],
  ['`sd_ok`, `fake_mega`', 'The card works; the test mode is on.'],
  ['`boot`, `uptime_s`, `heap_free`, `heap_min`, `clients`', 'Start count, seconds running, free memory now and at its lowest, phones on the live feed.'],
]));
add(H2('Web addresses'));
add(table([3700, 3460, 2200], ['Address', 'What it does', 'Who may use it'], [
  ['`/` and the page files', 'The dashboard itself', 'Anyone on the Wi-Fi'],
  ['`/ws`, `/api/status`', 'Live status (pushed), and the same once', 'Anyone'],
  ['`/api/doses`, `/api/doses.csv`', 'The Batch Log, and the whole log as a file', 'Anyone'],
  ['`/api/clock`', 'The phone gives the ESP32 its time', 'By the clock rules (4.6)'],
  ['`/api/login`, `/api/logout`, `/api/me`, `/api/account/password`', 'Logging in and out, who am I, new password', 'Anyone / logged in'],
  ['`/api/records/<kind>` and `.csv`', 'List and add records; download', 'Logged in'],
  ['`/api/edit/<kind>`, `/api/delete/<kind>`', 'Change or remove one record', 'Logged in'],
  ['`/api/photos`, `/api/photo`', 'Upload a photo; view or save one', 'Logged in'],
  ['`/api/users` (and `.csv`), `/api/edit/user`, `/api/delete/user`, `/api/farmer-login`, `/api/logins`', 'Farmers and their logins', 'Admin'],
  ['`/api/record-types`', 'The fields of each record form', 'Anyone'],
]));
add(P('`<kind>` is `soil`, `plantings`, `growth` or `fertilizer`.'));

// 8
add(H1('8. Files on the microSD card'));
add(table([3000, 6360], ['File', 'Columns'], [
  ['`/dose_log.csv`', 'dose_no, boot, uptime_s, seq, recipe, target_g, actual_g, error_g, error_pct, date_time'],
  ['`/crop/users.csv`', 'user_id, name'],
  ['`/crop/soil.csv`', 'user_id, plot, date_tested, soil_ph, soil_moisture_pct, photos'],
  ['`/crop/plantings.csv`', 'user_id, plot, crop, variety, date_planted, used_agridose, dose_numbers, photos'],
  ['`/crop/growth.csv`', 'user_id, plot, stage, date_observed, date_planted, days_after_planting, height_cm, leaf_count, notes, photos'],
  ['`/crop/fertilizer.csv`', 'user_id, plot, date_applied, fertilizer, amount_kg, notes'],
  ['`/crop/photos.csv`', 'photo_id, user_id, file, bytes, compressed_file, compressed_bytes'],
  ['`/crop/photos/0012.jpg`', 'Photo 12 as it was taken (`.png` or `.webp` if it was one)'],
  ['`/crop/photos/0012_compressed.jpg`', 'Photo 12\'s small copy, made by the phone'],
  ['`/auth/admin.csv`, `/auth/farmers.csv`', 'The logins (hashed passwords only); never sent to a phone'],
]));
add(bullets([
  'An **edit** rewrites the file as `.new`, then swaps it in. After a power cut in the middle, the next start-up keeps whichever copy is whole.',
  'The **dose log** is only ever added to, never edited, so the record of what the machine did cannot be changed from a phone.',
  '`photos` in a record holds photo numbers, for example `12 13`.',
]));

// 9
add(H1('9. For developers', false));
add(H2('Changing the page'));
add(steps([
  'Edit the files in `AgriDose_ESP32_Link/www/`.',
  'Test on the laptop: `node tests/test_dashboard.mjs`. It starts `tools/mock_esp32.py` (a pretend ESP32 with made-up data) and a hidden Edge browser at phone size, clicks through every tab and runs 101 checks. The first run after a cold start can fail its first check while Edge starts; run it again.',
  'Pack the page into the firmware: `python AgriDose_ESP32_Link/tools/make_web_assets.py` (writes `web_assets.h`).',
  'Compile and flash the ESP32. The Android app needs no rebuild: it loads the page from the ESP32.',
]));
add(note('The mock copies the firmware\'s addresses and fields by hand. A new field in the firmware must be added to the mock too.'));
add(H2('Other useful things'));
add(bullets([
  '**Test without the Mega:** set `FAKE_MEGA 1` in `config.h`. A pretend Mega inside the ESP32 makes a demo dose every 40 s and sends pretend hopper weights, and the page shows a yellow TEST MODE banner. It **must be 0** on the machine.',
  '**Flash space:** the ESP32 firmware uses 91 % of its program space (10 Nov). Past about 95 %, choose the partition scheme “Huge APP (3MB No OTA/1MB SPIFFS)”.',
  '**Laptop tests:** `tests/test_stock.cpp` (stock check), `test_validate.cpp` (data rules), `test_clock.cpp` (clock rules), `test_recommend.mjs` (Recommend), `test_agrilink.cpp` (the cable protocol).',
  '**Settings** (Wi-Fi name and password, phones, live update rate, login limits, photo sizes, stock spare) are all in `config.h`. After changing the Wi-Fi password, regenerate and reprint the QR sticker (`tools/make_sticker.py`).',
  '**This guide** is made by `manual/make_dashboard_guide.js`, the operation manual by `manual/make_manual.js`. Rebuild them after changing screens or rules.',
]));

// 10
add(H1('10. Limits and what still needs testing', false));
add(bullets([
  '**The stock check** (10 Nov) is compiled and tested on the laptop but not yet flashed. Its 20 g spare per hopper is a placeholder: measure what each jug still holds when its auger feeds nothing.',
  '**The low-hopper alert and Draining** (10 Nov, later) are compiled and tested on the laptop but not yet flashed. The 200 g limit is `LOW_HOPPER_G` in `www/app.js`. Draining needs the new firmware on both boards.',
  '**Photos:** Take photo does not respond in the Android app, and photo upload has not been tried on the machine.',
  '**The clock** is lost at every ESP32 restart until a phone opens the page.',
  '**Bisaya** lines are a first draft for the team to check; the new Enough stock for panel has none yet.',
  '**Plain HTTP:** Add to Home screen makes a Chrome shortcut, not an installed app, and the Wi-Fi password is the only lock on watching the machine.',
  '**Phones:** at most 5 on the Wi-Fi at once. Free memory with 3 to 5 phones on the live feed should be checked on the machine; the bottom line of the Machine tab shows it.',
  '**Look-only:** a phone can never start or stop a dose. This is on purpose (section 6).',
]));

const doc = new Document({
  creator: 'AgriDose team',
  title: 'AgriDose Web Dashboard Guide',
  styles: {
    default: { document: { run: { font: 'Arial', size: 22 } } },
    paragraphStyles: [
      { id: 'Heading1', name: 'Heading 1', basedOn: 'Normal', next: 'Normal', quickFormat: true,
        run: { size: 34, bold: true, color: GREEN, font: 'Arial' },
        paragraph: { spacing: { before: 240, after: 200 }, outlineLevel: 0, keepNext: true,
          border: { bottom: { style: BorderStyle.SINGLE, size: 8, color: GREEN, space: 4 } } } },
      { id: 'Heading2', name: 'Heading 2', basedOn: 'Normal', next: 'Normal', quickFormat: true,
        run: { size: 26, bold: true, color: GREEN, font: 'Arial' }, paragraph: { spacing: { before: 240, after: 120 }, outlineLevel: 1, keepNext: true } },
      { id: 'Heading3', name: 'Heading 3', basedOn: 'Normal', next: 'Normal', quickFormat: true,
        run: { size: 23, bold: true, color: '333333', font: 'Arial' }, paragraph: { spacing: { before: 180, after: 100 }, outlineLevel: 2, keepNext: true } },
    ],
  },
  numbering: {
    config: [
      { reference: 'bullets', levels: [{ level: 0, format: LevelFormat.BULLET, text: '•', alignment: AlignmentType.LEFT,
        style: { paragraph: { indent: { left: 400, hanging: 280 } } } }] },
      ...numberRefs.map((ref) => ({ reference: ref, levels: [{ level: 0, format: LevelFormat.DECIMAL, text: '%1.', alignment: AlignmentType.LEFT,
        style: { paragraph: { indent: { left: 400, hanging: 320 } } } }] })),
    ],
  },
  sections: [{
    properties: { titlePage: true, page: { size: { width: 12240, height: 15840 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
    footers: {
      first: new Footer({ children: [new Paragraph({ children: [] })] }),
      default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.RIGHT,
        children: [run('AgriDose Web Dashboard Guide · 10 Nov 2026 · page ', { size: 18, color: '777777' }),
          new TextRun({ children: [PageNumber.CURRENT], size: 18, color: '777777' })] })] }),
    },
    children: kids,
  }],
});

Packer.toBuffer(doc).then((buf) => { fs.writeFileSync(OUT, buf); console.log('wrote ' + OUT + ' (' + buf.length + ' bytes)'); });
