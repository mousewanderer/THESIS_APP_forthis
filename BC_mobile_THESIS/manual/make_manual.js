// To rebuild the manual after a change (Node 18+):
//   npm install docx@9      (once, in this folder or any other)
//   node make_manual.js AgriDose_Operation_Manual.docx
// Keep the facts in step with the firmware (screens, ranges, timings, pins).

// Builds AgriDose_Operation_Manual.docx (1 Nov 2026) with docx-js.
// Facts come from the firmware (AgriDose_Mega_Link, AgriDose_ESP32_Link,
// AgriLink.h, config.h) and the bench notes in CLAUDE.md / README.md.
const fs = require('fs');
const {
  Document, Packer, Paragraph, TextRun, Table, TableRow, TableCell, WidthType, ShadingType,
  BorderStyle, AlignmentType, HeadingLevel, LevelFormat, Footer, PageNumber, PageBreak,
} = require('docx');

const OUT = process.argv[2] || 'AgriDose_Operation_Manual.docx';
const GREEN = '2E5E2E', LIGHT = 'EAF2E3', LCDBG = 'DDE8D0', NOTE = 'FFF4D6', WARN = 'FDE2DF';
const FULL = 9360;                       // US Letter, 1-inch margins

// ---------- helpers ----------
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
const H1first = (t) => new Paragraph({ heading: HeadingLevel.HEADING_1, children: [run(t)] });
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
function box(text, fill, edge, label) {
  return new Paragraph({
    shading: { type: ShadingType.CLEAR, fill, color: 'auto' },
    border: { left: { style: BorderStyle.SINGLE, size: 24, color: edge, space: 8 } },
    indent: { left: 120, right: 120 }, spacing: { before: 120, after: 160 },
    children: [run(label + ' ', { bold: true }), ...rich(text)],
  });
}
const note = (t) => box(t, NOTE, 'D9A400', 'Note:');
const warn = (t) => box(t, WARN, 'C0392B', 'Warning:');

const border = { style: BorderStyle.SINGLE, size: 4, color: 'B7C4AE' };
const borders = { top: border, bottom: border, left: border, right: border };
function cell(content, width, o = {}) {
  const paras = (Array.isArray(content) ? content : [content]).map((c) =>
    c instanceof Paragraph ? c : new Paragraph({ children: rich(String(c)), spacing: { after: 40 } }));
  return new TableCell({
    width: { size: width, type: WidthType.DXA }, borders,
    margins: { top: 80, bottom: 80, left: 110, right: 110 },
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
// The LCD: 2 lines of 16 characters, in a green box
function lcd(l1, l2) {
  const line = (t) => new Paragraph({ spacing: { after: 0 }, children: [run((t || '').padEnd(16, ' '), { font: 'Consolas', size: 20, bold: true })] });
  return [line(l1), line(l2)];
}
function lcdCell(l1, l2, width) {
  return new TableCell({
    width: { size: width, type: WidthType.DXA }, borders,
    margins: { top: 80, bottom: 80, left: 110, right: 110 },
    shading: { type: ShadingType.CLEAR, fill: LCDBG, color: 'auto' },
    children: lcd(l1, l2),
  });
}
function screens(rows) {                 // [press, l1, l2, meaning]
  const w = [1700, 2500, 5160];
  const head = new TableRow({ tableHeader: true, children: ['You press', 'The LCD shows', 'What it means'].map((h, i) =>
    cell(new Paragraph({ children: [run(h, { bold: true, color: 'FFFFFF' })] }), w[i], { fill: GREEN })) });
  const body = rows.map(([press, l1, l2, meaning]) => new TableRow({ cantSplit: true, children: [cell(press, w[0]), lcdCell(l1, l2, w[1]), cell(meaning, w[2])] }));
  return [new Table({ width: { size: FULL, type: WidthType.DXA }, columnWidths: w, rows: [head, ...body] }),
          new Paragraph({ children: [], spacing: { after: 120 } })];
}

// ---------- content ----------
const kids = [];
const add = (...xs) => xs.flat().forEach((x) => kids.push(x));

// Title page
add(
  new Paragraph({ spacing: { before: 2400, after: 200 }, alignment: AlignmentType.CENTER, children: [run('AgriDose', { bold: true, size: 72, color: GREEN })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 120 }, children: [run('Solid Soil-Amendment Dosing Machine', { size: 32 })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 1200 }, children: [run('Operation Manual', { size: 40, bold: true })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 }, children: [run('Rice hull  ·  Compost  ·  Wood ash', { size: 24, color: '555555' })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 }, children: [run('Doses of 300 to 900 g, recipes A, B and C', { size: 24, color: '555555' })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 1600, after: 80 }, children: [run('Version of 1 November 2026', { size: 22 })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 }, children: [run('Firmware: AgriDose_Mega_Link and AgriDose_ESP32_Link (1 Nov 2026)', { size: 20, color: '555555' })] }),
  new Paragraph({ alignment: AlignmentType.CENTER, children: [run('Phone app: AgriDose dashboard (Android app or a browser)', { size: 20, color: '555555' })] }),
);

// Contents
add(H1('Contents'));
[
  '1. About the machine', '2. Safety', '3. Starting the machine', '4. Making a dose with the keypad',
  '5. Stopping a dose and clearing faults', '6. Zeroing the scales and draining a hopper', '7. The phone app (dashboard)',
  '8. Crop records', '9. Daily routine', '10. Care and maintenance', '11. Troubleshooting',
  '12. Specifications', '13. Status of this unit (1 Nov 2026)', 'Appendix A. Wiring (for technicians)',
  'Appendix B. Test programs (for technicians)',
].forEach((t) => add(new Paragraph({ children: [run(t)], spacing: { after: 80 } })));

// 1. About
add(H1('1. About the machine'));
add(P('AgriDose weighs out a mix of three solid soil amendments for one plot: **rice hull**, **compost** and **wood ash**. You choose a recipe and a total mass on the keypad; the machine splits it by the recipe and feeds each material with its own auger, weighing as it goes.'));
add(H2('Main parts'));
add(table([2700, 6660], ['Part', 'What it does'], [
  ['Hopper A', 'Rice hull (rice husk), with its own auger and scale (load cell).'],
  ['Hopper B', 'Compost, with its own auger and scale.'],
  ['Hopper C', 'Wood ash, with its own auger and scale.'],
  ['LCD (2 lines × 16)', 'Shows the menu, the dose in progress, the result and any fault.'],
  ['Keypad (4 × 4)', 'The only place where a dose is started or stopped.'],
  ['Main board (Arduino Mega)', 'Weighs the hoppers, runs the augers and draws the LCD.'],
  ['Wi-Fi board (ESP32)', 'Reads the keypad, keeps the logs on a microSD card, and runs the AgriDose Wi-Fi with the phone dashboard.'],
  ['Phone app', 'Shows the machine live, the dose history, crop records and a recipe recommendation. It cannot start a dose.'],
]));
add(H2('Recipes'));
add(P('Each recipe is a fixed split of the total mass. The machine feeds the hoppers in this order: **rice hull, then wood ash, then compost last**.'));
add(table([1500, 2000, 2000, 2000, 1860], ['Recipe', 'Rice hull (A)', 'Compost (B)', 'Wood ash (C)', 'LCD name'], [
  ['A', '25 %', '70 %', '5 %', 'RECIPE A GENERAL'],
  ['B', '30 %', '65 %', '5 %', 'RECIPE B'],
  ['C', '30 %', '60 %', '10 %', 'RECIPE C COMPACT'],
]));
add(P('Examples of the split (grams):'));
add(table([1800, 2520, 2520, 2520], ['Recipe', '300 g dose', '600 g dose', '900 g dose'], [
  ['A', 'hull 75 · compost 210 · ash 15', 'hull 150 · compost 420 · ash 30', 'hull 225 · compost 630 · ash 45'],
  ['B', 'hull 90 · compost 195 · ash 15', 'hull 180 · compost 390 · ash 30', 'hull 270 · compost 585 · ash 45'],
  ['C', 'hull 90 · compost 180 · ash 30', 'hull 180 · compost 360 · ash 60', 'hull 270 · compost 540 · ash 90'],
]));
add(note('Each hopper must hold at least its share of the dose before you start, plus some extra. The biggest shares are 630 g of compost (recipe A, 900 g) and 270 g of rice hull (recipes B and C, 900 g). Rice hull is light and bulky: 270 g is about 2 to 3 litres.'));

// 2. Safety
add(H1('2. Safety'));
add(bullets([
  '**Keep hands out of the hoppers and away from the auger outlets while the machine is on.** An auger can start when a dose starts.',
  'To stop a dose at once, press the **star key** ( * ) on the keypad. All augers stop.',
  '**After a fault or a power cut, no auger starts by itself**, but pressing **#** at the question on the LCD starts one. Look first (section 5).',
  '**Switch off the motor power before** you reach into a hopper, clear a jam or touch an auger.',
  'Wood ash is alkaline and dusty: wear gloves and a dust mask when filling hopper C.',
  'Keep water away from the boards, the keypad and the LCD.',
  'Do not lean on, shake or bump the hoppers during a dose: the scales are sensitive, and a bump can end a hopper early or show a wrong weight.',
  'Put a container under the outlet before every dose.',
]));
add(warn('Only one person should operate the keypad. The phone app is look-only: it never starts or stops the augers.'));

// 3. Starting
add(H1('3. Starting the machine', false));
add(steps([
  'Check that the hoppers do not touch each other or the frame.',
  'Switch on the board power and the **motor power** (the augers have their own supply).',
  'Wait for **AGRIDOSE D=DRAIN / #=DOSE  0=ZERO**. The machine is ready. If the LCD asks **RESUME** instead, a dose was cut off: see section 5.',
  'Fill the hoppers if they need it: rice hull in A, compost in B, wood ash in C. Use **dry, sieved compost** (wet or lumpy compost sticks above the auger).',
  'Open the phone app once (section 7). This also gives the machine the date and time for its dose log.',
]));
add(note('The machine remembers its scale zeros when it is switched off, so the hoppers may stay filled and the phone still shows their true levels. Only the very first start of a new board (or after a technician changes the saved format) shows **ZEROING SCALES / HOPPERS EMPTY!**: the hoppers must be empty then. If they were not, doses are still weighed correctly, but the hopper levels on the phone are wrong until you zero the scales (section 6).'));
add(note('The LCD on this unit has a broken backlight. Read it with a phone flashlight held at an angle. If you still see nothing, turn the small contrast screw on the back of the LCD slowly.'));

// 4. Dose
add(H1('4. Making a dose with the keypad'));
add(H2('The keys'));
add(table([1600, 7760], ['Key', 'Meaning'], [
  ['#', 'OK / next / start. After a fault or a power cut: **continue the dose**.'],
  ['*', 'Back / cancel. During a dose: **STOP**. After a fault or a power cut: end the dose.'],
  ['D', 'Back to the home screen. **On the home screen: drain a hopper** (section 6).'],
  ['0 to 9', 'Type the mass in grams'],
  ['A, B, C', 'Choose the recipe'],
  ['0 (on the home screen)', 'Zero the scales (section 6)'],
]));
add(H2('Step by step'));
add(P('Example: recipe A, 600 g. The keys are `# A # 6 0 0 # #`. Put a container under the outlet first.'));
add(screens([
  ['(nothing)', 'AGRIDOSE D=DRAIN', '#=DOSE  0=ZERO', 'The home screen.'],
  ['#', 'SELECT RECIPE', 'A / B / C *=BACK', 'Choose the recipe.'],
  ['A', 'RECIPE A GENERAL', '#=OK   *=BACK', 'Check the recipe name, then press # to go on.'],
  ['#', 'DOSE MASS (g)', '300-900: _', 'Type the total grams: any whole number from 300 to 900.'],
  ['6 0 0', 'DOSE MASS (g)', '300-900: 600', 'What you typed. * clears it.'],
  ['#', 'A 600g 25/70/5%', '#=START *=CANCEL', 'Check the recipe, mass and split (hull/compost/ash %).'],
  ['#', 'STARTING DOSE', 'PLEASE WAIT...', 'The machine accepts the dose.'],
  ['(wait)', 'DOSING HULL', '148.2/150g SLOW', 'Each hopper in turn: grams so far / target, and the stage (see below).'],
  ['(wait)', 'DOSE DONE', '598.6 g', 'All three hoppers are done.'],
  ['(wait)', 'DONE 598.6/600g', '-1.4g -0.2%', 'The result: actual mass, and how far it is from the target. Any key returns home.'],
]));
add(H3('Stages shown while a hopper is dosing'));
add(table([1400, 7960], ['Stage', 'Meaning'], [
  ['BULK', 'Full speed, far from the target.'],
  ['SLOW', 'Close to the target: the auger slows down (auger B slows by short bursts).'],
  ['SETL', 'The auger has stopped; the material and the scale settle before the final weighing.'],
  ['TRIM', 'A small top-up because the hopper came out a little short (at most 3).'],
  ['OK', 'This hopper is done.'],
]));
add(H3('If the mass is out of range'));
add(P('Typing a mass under 300 or over 900 and pressing # shows **OUT OF RANGE / USE 300-900 g** for 2 seconds. Type the mass again.'));
add(H3('If a hopper is low'));
add(P('Before the start the machine checks that each hopper holds its share of the dose plus a small spare (20 g). If one does not, it shows this instead of the check screen. Each short hopper shows in turn:'));
add(screens([
  ['#', 'LOW COMPOST', 'NEED 650 HAS 410', 'Compost needs 650 g (its 630 g share plus the spare) and has 410 g.'],
  ['(wait)', 'REFILL HOPPER OR', '#=GO ON *=BACK', 'What you can do.'],
]));
add(bullets([
  'Refill that hopper. The screen follows the scale, and when there is enough it changes to the check screen by itself. Then press # to start.',
  'Or press **#** to start anyway. If the hopper runs out, the dose pauses with EMPTY (see Faults) and you can refill it then.',
  'Or press the **star key** ( * ) to type a smaller mass.',
]));
add(P('The Machine tab in the app shows the same check for every recipe, under **Enough stock for**.'));
add(H3('Other messages before the start'));
add(table([3200, 6160], ['LCD', 'What to do'], [
  ['LINK BUSY / TRY AGAIN', 'Wait a moment and press # again.'],
  ['MACHINE BUSY / TRY AGAIN LATER', 'A dose, a drain or a zeroing is still running. Wait for it to end.'],
  ['NO REPLY / CHECK MEGA LINK', 'The main board did not answer. See Troubleshooting (section 11).'],
  ['MEGA RESTARTED / TRY AGAIN', 'The main board restarted. Check the power, then start again.'],
  ['DOSE REFUSED / (reason)', 'The main board refused the dose. Note the reason and see section 11.'],
]));

// 5. Stop and faults
add(H1('5. Stopping a dose and clearing faults', false));
add(H2('Stopping'));
add(P('Press the **star key** ( * ) at any time during a dose. All augers stop at once and the LCD shows **DOSE STOPPED** with the grams dosed so far. The stopped dose is saved in the dose log with its actual mass.'));
add(P('At the very start of a hopper, while it takes its first weight, the machine can take up to about 1 second to react to the star key.'));
add(H2('Faults'));
add(P('A fault stops the augers and **pauses** the dose. Nothing is lost. The LCD names the fault and the hopper and shows, one after another, what to check, for example:'));
add(new Table({ width: { size: 2600, type: WidthType.DXA }, columnWidths: [2600], rows: [new TableRow({ children: [lcdCell('EMPTY: HULL', 'REFILL HOPPER?', 2600)] })] }), new Paragraph({ children: [], spacing: { after: 120 } }));
add(table([1600, 3600, 4160], ['Fault', 'Cause', 'What to do'], [
  ['EMPTY', 'The hopper weight did not drop by at least 1 g in 15 seconds while its auger ran.', 'Is the hopper empty? Refill it. Is compost stuck above the inlet (bridging)? Tap the side of the jug or stir. Is the motor power on? Is the auger jammed?'],
  ['SENSOR', 'A scale (load cell) stopped answering.', 'Check the load-cell wires and connectors of that hopper. Call a technician if it repeats.'],
  ['TIMEOUT', 'One hopper took longer than 4 minutes.', 'Usually slow feeding: bridging, wet compost, or a nearly empty hopper. Fix it as for EMPTY.'],
]));
add(P('After the checks the LCD shows **HANDS CLEAR? / #=GO ON *=END**, then the checks again, until you answer:'));
add(steps([
  'Fix the cause. **Switch off the motor power before** you reach into a hopper or touch an auger.',
  'Switch the motor power back on and take your hands away.',
  'Press **#**. The LCD shows **CONTINUING DOSE / HANDS CLEAR!**, and about 1 second later the same hopper carries on from the grams it had already dosed.',
]));
add(P('Or press the **star key** ( * ) to end the dose. The LCD shows **DOSE STOPPED** with the grams dosed so far, and the dose is saved in the log with what was really dosed.'));
add(note('You may refill the hopper during the pause: the machine weighs it afresh before it continues, so the added material is not counted as dosed. Pushing on a jug while you clear it does not lose the grams already dosed either. If the cause is still there, the same fault comes back. A dose gets one row in the log, however many faults it had.'));
add(H2('If the power goes off during a dose'));
add(P('The machine saves its progress while it runs, so a power cut does not lose the dose. When the power comes back, **no auger moves**. The LCD asks what to do and shows the grams dosed so far, for example:'));
add(new Table({ width: { size: 2600, type: WidthType.DXA }, columnWidths: [2600], rows: [new TableRow({ children: [lcdCell('RESUME A 600g?', '201g #=YES *=NO', 2600)] })] }), new Paragraph({ children: [], spacing: { after: 120 } }));
add(table([1600, 7760], ['Key', 'What happens'], [
  ['#', 'The dose carries on where it stopped. **An auger starts**: keep your hands clear, and check the container is still under the outlet with the first part of the dose in it.'],
  ['*', 'The dose ends here. The LCD shows the result, and the dose is saved in the log with what was really dosed.'],
]));
add(P('It keeps asking until you answer, even if the power goes off again. No new dose can start until you do.'));
add(warn('If you plan to continue, do not add material to a hopper or take any out while the power is off. The machine works out the grams from the hopper weight, and a small change can go unnoticed.'));
add(P('**WEIGHT CHANGED!** on the top line means the hopper weight does not fit the saved progress, usually because material was added or taken out while the power was off. The grams shown then come from the last save, up to 5 seconds old, so they can be too low or too high. It is safest to press * and start a new dose for the rest.'));
add(note('This only happens when the boards lose power. If only the motor power goes off and the boards stay on, the dose pauses with an EMPTY fault after 15 seconds: switch the motor power back on and press #.'));

// 6. Zero
add(H1('6. Zeroing the scales and draining a hopper', false));
add(H2('Zeroing the scales'));
add(P('The machine remembers its scale zeros when it is switched off. Zero the scales if empty hoppers do not show 0 g on the phone, after the machine was moved, or after a hopper was taken off.'));
add(steps([
  'Empty all three hoppers. Take your hands off the machine.',
  'On the home screen press **0**. The LCD shows `ZERO SCALES? / #=YES *=NO`.',
  'Press **#**. The LCD shows **ZEROING SCALES / HOPPERS EMPTY!** for about 6 seconds, then the home screen.',
]));
add(note('While the scales are zeroing, the phone may show "Mega offline" for a few seconds. That is normal.'));
add(H2('Draining a hopper'));
add(P('Draining runs one hopper\'s auger to empty it: at the end of the day, before the machine stands for some days, or to change the material. **The material comes out of the outlet, as in a dose: put a container under it first.** A drain is not a dose: it is not saved in the Batch Log.'));
add(screens([
  ['D', 'DRAIN  A=HULL', 'B=COMP C=ASH', 'On the home screen. Choose the hopper: A rice hull, B compost, C wood ash.'],
  ['B', 'COMP HAS 4123g', '#=DRAIN *=BACK', 'What is in that hopper now. * goes back, D goes home.'],
  ['#', 'STARTING DRAIN', 'PLEASE WAIT...', 'The hopper is weighed (about 1 s), then its auger starts at full speed.'],
  ['(wait)', 'DRAINING COMP', 'OUT 1234g *=STOP', 'The grams that have come out so far.'],
  ['*', 'DRAIN STOPPED', 'COMP 1234g OUT', 'The star key stops it at once. After 3 s the home screen.'],
]));
add(P('The machine also stops a drain by itself:'));
add(table([3000, 6360], ['LCD', 'Why'], [
  ['NO FLOW: EMPTY?', 'Less than 1 g came out in 15 s: the hopper is empty, or the material is stuck above the auger (tap the hopper, then drain again).'],
  ['DRAIN: NO SENSOR', 'The hopper\'s scale stopped answering. Check its load-cell connector.'],
  ['DRAIN TIME UP', 'It ran for 10 minutes. Press D again to go on.'],
]));
add(note('While a hopper drains, the phone shows **Draining** under Machine Status, and that hopper\'s gauge goes down. A dose or a zeroing cannot start until the drain ends.'));

// 7. Phone app
add(H1('7. The phone app (dashboard)'));
add(H2('Connecting'));
add(steps([
  'On the phone, join the Wi-Fi network **AgriDose** (the password is on the machine\'s sticker or from the admin).',
  'Open the **AgriDose app**. Without the app, open a browser at **http://192.168.4.1/** or scan the QR sticker on the machine. Most phones open the page by themselves after joining.',
  'If Android says the network has **no internet**, choose to **stay connected**. The machine\'s Wi-Fi never has internet.',
]));
add(P('Up to 5 phones can be connected at the same time.'));
add(H2('Installing the Android app'));
add(bullets([
  'Get the file **app-debug.apk** from the team (for example from the project\'s GitHub Releases page) and open it on the phone.',
  'Android asks to allow **Install unknown apps** for the browser or file manager: allow it.',
  'If Play Protect warns about an unknown developer, tap **Install anyway**.',
  'If a newer version does not install over the old one ("App not installed"), uninstall the old app first.',
]));
add(H2('The tabs'));
add(table([1900, 7460], ['Tab', 'What it shows'], [
  ['Machine', 'Live status (idle, dosing, draining or fault), the weight being dosed, the stock in each hopper (A, B, C gauges and total), how big a dose of each recipe that stock allows (**Enough stock for**), and the last dose. The small line at the bottom shows the machine\'s clock and system details.'],
  ['Records', 'The crop records of a farmer (section 8). Needs a login.'],
  ['Recommend', 'Pick a planted plot: the app ranks recipes A, B and C from the newest soil test, the crop\'s pH range, the fertilizer applied and the rain, and shows the **keys to press** at the machine for the dose, e.g. `# B # 6 0 0 # #`. Nothing is sent to the machine. Needs a login.'],
  ['Crop Guide', 'The 12 crops with local names, soil pH ranges and sources.'],
  ['Batch Log', 'Every dose, newest first, with the date and time, target, actual mass and error. **View details** shows the split per material; **Download CSV** saves the whole log.'],
]));
add(P('The **Bisaya** button shows every label in Bisaya too. The sun button switches to a bright mode for use outdoors.'));
add(P('A **red dot on the bell** means there is an alert: tap the bell to read it. Besides faults, Mega offline and SD card problems, the bell warns when **a hopper holds less than 200 g**, e.g. "Hopper B (compost) is low: 150 g left". Refill that hopper and the dot goes.'));
add(H2('Logins'));
add(bullets([
  'Anyone on the AgriDose Wi-Fi can see the Machine tab, the Crop Guide and the Batch Log.',
  'Records and Recommend need a login. A **farmer** sees only their own records. The **admin** sees every farmer, adds farmers and gives them logins.',
  'On the admin\'s first login the app asks for a new password before anything else.',
]));
add(H2('The date and time'));
add(P('The machine has no clock of its own. **It takes the time from the first phone that opens the app after the machine is switched on.** After that, a logged-in phone corrects it if it is more than 2 minutes off.'));
add(note('Open the app once after every power-on. A dose made before any phone opened the app is saved without a time and shows "Time not known" in the Batch Log.'));

// 8. Records
add(H1('8. Crop records', false));
add(P('Records are kept per farmer on the machine\'s microSD card. Choose the farmer at the top of the app, then use **+ Add** in each group.'));
add(table([2400, 6960], ['Record', 'Fields'], [
  ['Soil test', 'plot, date tested, soil pH, soil moisture %, photos'],
  ['Seed type and planting', 'plot, crop, variety, date planted, AgriDose used (yes/no), dose numbers from the Batch Log, photos'],
  ['Growth record', 'plot, growth stage, date observed, date planted, day count, height, leaf count, notes, photos'],
  ['Fertilizer application', 'plot, date applied, fertilizer, amount (kg), notes'],
]));
add(H2('Rules the machine checks'));
add(bullets([
  'No two records of the same kind for the same plot and date (for fertilizer: the same fertilizer too).',
  'No date after today.',
  'A growth record needs a planting on that plot, and its date observed cannot be before the date planted.',
  'Dose numbers in a planting must be doses that are in the Batch Log.',
  'No two farmers with the same name.',
]));
add(H2('Photos, edit and delete'));
add(bullets([
  '**Photos:** up to 3 per record, with **Take photo** or **Choose photo**. Tap a photo to see it bigger and to save it.',
  '**Edit:** open a record with **View details**, then **Edit**. Saving replaces the record; it does not add a new one.',
  '**Delete:** under Edit, tap **Delete**, then **Yes, delete this record**. The record and its photos are removed from the SD card and cannot be brought back. A planting cannot be deleted while its plot still has growth records: delete those first.',
  'If the app says **This record was changed since the list was loaded**, someone else changed it: close it and open it again.',
  '**Download records (CSV)** at the bottom of the Records tab saves the files for a spreadsheet.',
]));

// 9. Daily routine
add(H1('9. Daily routine'));
add(H2('Start of the day'));
add(steps([
  'Hoppers not touching each other; container ready under the outlet.',
  'Switch on the board power and the motor power. Wait for AGRIDOSE D=DRAIN / #=DOSE  0=ZERO.',
  'Fill the hoppers if they need it (dry, sieved compost).',
  'Open the app once: check the Machine tab shows the hoppers\' stock and the clock.',
]));
add(H2('During the day'));
add(bullets([
  'Before each dose: enough material in each hopper for its share, and an empty container under the outlet.',
  'Watch the first part of every dose. If compost stops coming out, tap the side of hopper B.',
  'Write the dose number from the Batch Log into the farmer\'s planting record when the dose is used on a plot.',
]));
add(H2('End of the day'));
add(steps([
  'If the machine will stand for some days, empty the hoppers (damp compost sticks above the auger): D on the keypad drains one hopper at a time (section 6).',
  'Brush the outlets clean.',
  'Once a week, download the Batch Log and the records (CSV) to a phone or laptop as a backup.',
  'Switch off the motor power, then the board power.',
]));

// 10. Maintenance
add(H1('10. Care and maintenance', false));
add(bullets([
  '**Compost:** keep it dry and sieve out lumps. Damp compost sticks above the auger inlet (bridging) and stops the feed.',
  '**Hoppers must not touch each other or the frame.** If they touch, the weight of one shows on the other scale.',
  '**Augers and motors** must be fixed only to their own hopper (or joined to the frame by a flexible sleeve), for the same reason.',
  '**Scales:** once a month, or after anything was moved or rebuilt, a technician checks each scale with a known weight (Appendix B).',
  '**Wiring:** check that the load-cell and motor connectors are pushed in fully. Loose wires cause SENSOR faults and jumping weights.',
  '**microSD card:** do not remove it while the machine is on. Back up the CSV files regularly.',
]));

// 11. Troubleshooting
add(H1('11. Troubleshooting'));
add(table([2500, 3000, 3860], ['Problem', 'Likely cause', 'What to do'], [
  ['The LCD looks blank', 'Broken backlight on this unit, or contrast', 'Use a flashlight at an angle; turn the contrast screw on the back. Technician: run lcd_test.'],
  ['LCD: NO ESP32 LINK / CHECK UART WIRES', 'The Wi-Fi board is off or the link wires are loose', 'Check the ESP32 has power; check the two link wires and the shared ground (Appendix A).'],
  ['A key does nothing', 'Pressed too lightly or too fast', 'Press firmly in the middle of the key. Technician: run keypad_test.'],
  ['EMPTY fault but the hopper is full', 'Compost bridging, motor power off, or a jammed auger', 'Tap or stir the hopper; check the motor supply is switched on; switch off and clear the jam. Then press # to continue the dose.'],
  ['An auger does not turn', 'Motor power off, or driver wiring', 'Switch on the motor supply; check the motor driver connectors (Appendix A).'],
  ['SENSOR fault', 'Load-cell wire loose', 'Check that hopper\'s load-cell connector. Then press # to continue the dose.'],
  ['TIMEOUT fault', 'Very slow feeding', 'As for EMPTY: bridging, wet compost, nearly empty hopper.'],
  ['NO FLOW: EMPTY? while draining, but material is left', 'Bridging above the auger, or motor power off', 'Tap or stir the hopper; check the motor supply; press D and drain again.'],
  ['Bell: a hopper is low', 'Under 200 g in that hopper', 'Refill it before the next dose.'],
  ['Weights on the phone jump or are wrong', 'Hoppers touching, loose wire, or a scale that was zeroed with material in it', 'Separate the hoppers; check connectors; empty and zero the scales (section 6).'],
  ['LCD: RESUME or WEIGHT CHANGED! at switch-on', 'The power went off during a dose', 'Answer with # or * (section 5).'],
  ['The phone does not open the app page', 'Not on the AgriDose Wi-Fi, or Android left it for "no internet"', 'Join AgriDose again and choose to stay connected; open http://192.168.4.1/. At most 5 phones.'],
  ['App shows "Mega offline"', 'Link wires, or the scales are being zeroed', 'Wait 6 s after zeroing; otherwise as for NO ESP32 LINK.'],
  ['App shows an SD card banner', 'The microSD card is missing or loose', 'Switch off, reseat the card, switch on. Doses still work but are not saved meanwhile.'],
  ['Batch Log: "Time not known"', 'No phone opened the app after power-on', 'Open the app right after switching on.'],
  ['"Take photo" does nothing in the Android app', 'Known problem of the app', 'Use "Choose photo", or a browser at http://192.168.4.1/.'],
  ['The admin password is lost', '—', 'Technician: on a laptop, delete /auth/admin.csv from the microSD card; the first admin login from the firmware works again.'],
]));

// 12. Specs
add(H1('12. Specifications'));
add(table([3300, 6060], ['Item', 'Value'], [
  ['Dose', '300 to 900 g total, whole grams, recipe A, B or C'],
  ['Materials', 'A rice hull (husk), B compost, C wood ash'],
  ['Order of feeding', 'Rice hull, wood ash, then compost'],
  ['Accuracy target', 'Each hopper ends at most 2 g under its share (up to 3 top-ups)'],
  ['Compost feed (auger B)', 'About 10 to 13 g per second; slows by bursts near the target'],
  ['Fault limits', 'EMPTY after 15 s without 1 g of progress; TIMEOUT after 4 min per hopper'],
  ['Draining', 'One hopper at a time, full speed; stops on *, after 15 s without 1 g out, or after 10 min'],
  ['Low hopper alert', 'On the phone\'s bell when a hopper holds under 200 g'],
  ['Controllers', 'Arduino Mega 2560 (scales, augers, LCD); ESP32 (keypad, microSD, Wi-Fi)'],
  ['Scales', '3 load cells with HX711 amplifiers'],
  ['Motor drivers', '3 × IBT_2 (BTS7960), one direction'],
  ['Display and keys', '16 × 2 I2C LCD (address 0x27); 4 × 4 keypad'],
  ['Wi-Fi', 'Network "AgriDose", dashboard at http://192.168.4.1/, up to 5 phones, channel 6, no internet'],
  ['Data (microSD)', '/dose_log.csv (one row per dose, with date_time); /crop/*.csv (farmers and records); /crop/photos/'],
  ['Scale calibration (31 Oct 2026)', 'Factors A 193.9649, B 103.1425, C 196.0279 (checked with a 261 g weight)'],
]));

// 13. Status
add(H1('13. Status of this unit (1 Nov 2026)', false));
add(P('This unit is still on the bench. Before it is used on farms:'));
add(table([2600, 1600, 5160], ['Part', 'Status', 'Notes'], [
  ['Scales A, B, C', 'Working', 'Calibrated 31 Oct. Loading B makes C read about 1.5 % of B\'s load lighter; doses are not affected.'],
  ['Auger B (compost)', 'Working', 'Tested: 100 g asked, 101.9 g out (102.3 g on a kitchen scale). Compost bridges unless it is dry and sieved.'],
  ['Augers A and C', 'Being fixed', 'Wiring is being repaired. Until they work, every dose ends with EMPTY: HULL (hull is dosed first). Recalibrate the scales after they are fitted.'],
  ['LCD', 'Working', 'Text and menus checked on 1 Nov. Backlight broken: read it with a flashlight.'],
  ['Keypad', 'Working', 'All 16 keys checked on 1 Nov.'],
  ['Keypad + LCD + dose log', 'Working', 'Dose started from the keypad, fault cleared with *, result logged (1 Nov).'],
  ['Phone app', 'Mostly checked', 'Hopper gauges read 0 g with empty hoppers. Still to check on the phone: the clock, a dose with its time, Delete, the 300–900 g range. "Take photo" does not respond in the Android app.'],
  ['300 to 900 g range', 'New (1 Nov)', 'Not yet run with a full 900 g dose. Check the hoppers hold their shares and the load cells are rated for the weight.'],
  ['Saving through a power cut', 'New (3 Nov)', 'Tested on a laptop with the real firmware and modelled hoppers. Not yet tried on this machine.'],
  ['Faults pause the dose', 'New (3 Nov)', 'Tested the same way. Not yet tried on this machine.'],
  ['Draining a hopper', 'New (10 Nov)', 'Tested on a laptop with the real Mega firmware and modelled hoppers. Not yet tried on this machine; both boards need the new firmware.'],
  ['Low hopper alert', 'New (10 Nov)', 'Tested in the dashboard test. Not yet seen on a phone.'],
]));

// Appendix A
add(H1('Appendix A. Wiring (for technicians)'));
add(P('Pins on the Arduino Mega 2560. These match all current firmware.'));
add(table([2300, 2353, 2353, 2354], ['Signal', 'Hopper A (hull)', 'Hopper B (compost)', 'Hopper C (ash)'], [
  ['HX711 SCK / DT', '36 / 37', '38 / 39', '40 / 41'],
  ['IBT_2 RPWM / LPWM', '3 / 2', '6 / 7', '5 / 4'],
  ['IBT_2 R_EN / L_EN', '24 / 25', '22 / 23', '29 / 28'],
]));
add(bullets([
  'Only **RPWM** is driven (one direction); LPWM is held low. Both enables are high when the machine runs.',
  '**Wiring-board header order:** motor driver `VCC GND L_EN LPWM R_EN RPWM`; load cell `GND DT SCK VCC`. Rows from the top: rice hull, compost, wood ash.',
  '**LCD:** I2C address 0x27, SDA pin 20, SCL pin 21.',
  '**Link Mega ↔ ESP32:** Mega TX2 (16) → level shifter → ESP32 GPIO27; ESP32 GPIO26 → Mega RX2 (17); grounds joined. 57600 baud.',
  '**Keypad (ESP32):** rows R1–R4 = GPIO 4, 16, 32, 5; columns C1–C4 = GPIO 18, 19, 21, 33.',
  '**microSD (ESP32, VSPI):** MISO 25, SCK 14, MOSI 23, CS 22.',
]));
add(warn('When augers A and C are fitted, fix each motor and auger tube only to its own hopper (or use a flexible sleeve to the frame), then calibrate the scales again. Auger B only feeds at PWM 250 and up; check the PWM settings of A and C once they run.'));

// Appendix B
add(H1('Appendix B. Test programs (for technicians)', false));
add(P('Each program is an Arduino sketch in the calibration_center folder. Uploading one replaces the machine firmware: upload AgriDose_Mega_Link (Mega) and AgriDose_ESP32_Link (ESP32) again afterwards. **Both boards must run firmware of the same date.**'));
add(table([2300, 1500, 5560], ['Sketch', 'Board · baud', 'Use'], [
  ['lcd_test', 'Mega · 115200', 'Checks the LCD wires, address and link, then shows a hidden 4-digit code to read off the glass. `c` = counter for a wire-wiggle test.'],
  ['keypad_test', 'ESP32 · 115200', 'Checks the 8 keypad wires for shorts, then asks for all 16 keys in order and explains a wrong key.'],
  ['main_checker', 'Mega · 115200', 'Live weights (`w`), zeroing (`t`), motor check (`j a 255 2` = auger A at full power for 2 s), single-hopper dose (`a 25`), full dose (`m 300 600 90`). Any line stops the augers.'],
  ['agridose_calibrate', 'Mega · 9600', 'Calibrates a scale with a known weight. New factor = old factor × average reading ÷ true grams.'],
  ['agridose_hoppers', 'Mega · 9600', 'Live readings of the three scales.'],
]));
add(P('Calibration check: put a known weight (for example the 261 g wrench) in the middle of a hopper and then at its side. The reading should match within 1 %, and the other hoppers should stay at about 0 g.'));

// ---------- document ----------
const doc = new Document({
  creator: 'AgriDose team',
  title: 'AgriDose Operation Manual',
  styles: {
    default: { document: { run: { font: 'Arial', size: 22 } } },
    paragraphStyles: [
      { id: 'Heading1', name: 'Heading 1', basedOn: 'Normal', next: 'Normal', quickFormat: true,
        run: { size: 34, bold: true, color: GREEN, font: 'Arial' },
        paragraph: { spacing: { before: 240, after: 200 }, outlineLevel: 0,
          border: { bottom: { style: BorderStyle.SINGLE, size: 8, color: GREEN, space: 4 } } } },
      { id: 'Heading2', name: 'Heading 2', basedOn: 'Normal', next: 'Normal', quickFormat: true,
        run: { size: 26, bold: true, color: GREEN, font: 'Arial' }, paragraph: { spacing: { before: 240, after: 120 }, outlineLevel: 1 } },
      { id: 'Heading3', name: 'Heading 3', basedOn: 'Normal', next: 'Normal', quickFormat: true,
        run: { size: 23, bold: true, color: '333333', font: 'Arial' }, paragraph: { spacing: { before: 180, after: 100 }, outlineLevel: 2 } },
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
        children: [run('AgriDose Operation Manual · 1 Nov 2026 · page ', { size: 18, color: '777777' }),
          new TextRun({ children: [PageNumber.CURRENT], size: 18, color: '777777' })] })] }),
    },
    children: kids,
  }],
});

Packer.toBuffer(doc).then((buf) => { fs.writeFileSync(OUT, buf); console.log('wrote ' + OUT + ' (' + buf.length + ' bytes)'); });
