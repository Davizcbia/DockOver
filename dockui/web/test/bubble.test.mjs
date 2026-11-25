// Bubble Code v1 tests: Reed-Solomon, then the full decoder on simulated phone-camera views of a screen.
//   node bubble.test.mjs [trials per condition]
import { rsEncode, rsDecode, encode, render, decode, grayFromRGBA, CELLS } from '../dockdrop-scan/bubble/bubble-code.js';

let failures = 0;
const check = (ok, msg) => { if (!ok) { failures++; console.log('FAIL ' + msg); } };
import { SRC, camera, rnd } from './camera-sim.mjs';

// 1. Reed-Solomon: up to 4 bad bytes corrected; more are refused (or, rarely, miscorrected: the scanner then requires
//    the same reading twice and the pairing handshake checks the secret anyway)
{
  let fixed = 0, refused = 0, wrong5 = 0;
  for (let i = 0; i < 500; i++) {
    const data = Array.from({ length: 11 }, () => Math.floor(rnd() * 256));
    const cw = rsEncode(data);
    const bad = cw.slice(), nerr = i % 5;  // 0..4 errors
    const pos = new Set(); while (pos.size < nerr) pos.add(Math.floor(rnd() * cw.length));
    for (const p of pos) bad[p] ^= 1 + Math.floor(rnd() * 255);
    const out = rsDecode(bad);
    if (out && out.slice(0, 11).every((v, k) => v === data[k])) fixed++;
    const bad5 = cw.slice(); const p5 = new Set(); while (p5.size < 5) p5.add(Math.floor(rnd() * cw.length));
    for (const p of p5) bad5[p] ^= 1 + Math.floor(rnd() * 255);
    const o5 = rsDecode(bad5);
    if (!o5) refused++; else if (!o5.slice(0, 11).every((v, k) => v === data[k])) wrong5++;
  }
  console.log(`reed-solomon: ${fixed}/500 with 0-4 bad bytes corrected; 5 bad bytes: ${refused}/500 refused, ${wrong5} miscorrected`);
  check(fixed === 500, 'RS corrects up to 4 bytes');
}

const trials = Number(process.argv[2] || 24);
const conditions = [
  ['straight on, mid size', () => ({ D: 200, tilt: 0.1 * rnd() })],
  ['small (bubble 90 px across)', () => ({ D: 90, tilt: 0.15 * rnd() })],
  ['large (bubble 280 px of a 360 px frame)', () => ({ D: 280, tilt: 0.15 * rnd() })],
  ['tilted 25-40 degrees', () => ({ D: 190, tilt: (25 + 15 * rnd()) * Math.PI / 180 })],
  ['defocused', () => ({ D: 180, tilt: 0.2 * rnd(), blur: 3 })],
  ['screen moire', () => ({ D: 150 + 80 * rnd(), tilt: 0.25 * rnd(), moire: true })],
  ['dim / low light (gain 0.45, noise 0.07)', () => ({ D: 190, tilt: 0.2 * rnd(), gain: 0.45, noise: 0.07 })],
  ['over-exposed (gain 1.9)', () => ({ D: 190, tilt: 0.2 * rnd(), gain: 1.9, gamma: 0.8 })],
  ['glare spot on the screen', () => ({ D: 200, tilt: 0.2 * rnd(), glare: { x: 140 + 80 * rnd(), y: 140 + 80 * rnd(), r: 35 + 20 * rnd(), a: 0.55 } })],
  ['front camera (mirrored)', () => ({ D: 200, tilt: 0.25 * rnd(), mirror: true })],
  ['everything at once', () => ({ D: 140 + 60 * rnd(), tilt: (15 + 15 * rnd()) * Math.PI / 180, blur: 2, moire: rnd() < 0.5, gain: 0.7 + 0.6 * rnd(), noise: 0.05, glare: { x: 150 + 60 * rnd(), y: 150 + 60 * rnd(), r: 30, a: 0.4 }, mirror: rnd() < 0.3 })],
];
const results = [];
for (const [name, make] of conditions) {
  let ok = 0, ms = 0, wrong = 0, stages = {};
  for (let t = 0; t < trials; t++) {
    const secret = (BigInt(Math.floor(rnd() * 2 ** 32)) << 32n) | BigInt(Math.floor(rnd() * 2 ** 32));
    const src = render(encode({ version: 1, flags: 1, secret }), SRC);
    const opt = { axis: rnd() * Math.PI, turn: rnd() * 2 * Math.PI, offx: (rnd() - 0.5) * 40, offy: (rnd() - 0.5) * 40, ...make() };
    const img = camera(src, opt);
    const r = decode(img);
    if (r.ok) { if (r.secret === secret) { ok++; ms += r.ms; } else wrong++; }
    else stages[r.stage] = (stages[r.stage] || 0) + 1;
  }
  results.push([name, ok, wrong, ms / Math.max(ok, 1), stages]);
  console.log(`${name.padEnd(42)} ${String(ok).padStart(3)}/${trials} decoded${wrong ? `, ${wrong} WRONG` : ''}  ~${(ms / Math.max(ok, 1)).toFixed(0)} ms  ${Object.keys(stages).length ? JSON.stringify(stages) : ''}`);
  check(wrong === 0, `${name}: never a wrong secret`);
}
console.log(`cells ${CELLS}, condition rates above`);

// Limits (reported, not required): where does it stop reading?
console.log('limits:');
const limits = [
  ['tilted 45-55 degrees', () => ({ D: 190, tilt: (45 + 10 * rnd()) * Math.PI / 180 })],
  ['tiny (bubble 64 px across)', () => ({ D: 64, tilt: 0.15 * rnd() })],
  ['tiny + tilted 30 degrees (90 px)', () => ({ D: 90, tilt: 30 * Math.PI / 180 })],
  ['heavy defocus (blur 6)', () => ({ D: 180, tilt: 0.2 * rnd(), blur: 6 })],
  ['very dark (gain 0.25, noise 0.1)', () => ({ D: 190, tilt: 0.2 * rnd(), gain: 0.25, noise: 0.1 })],
  ['strong glare (a 0.9)', () => ({ D: 200, tilt: 0.2 * rnd(), glare: { x: 150 + 60 * rnd(), y: 150 + 60 * rnd(), r: 45, a: 0.9 } })],
];
for (const [name, make] of limits) {
  let ok = 0, wrong = 0;
  for (let t = 0; t < trials; t++) {
    const secret = (BigInt(Math.floor(rnd() * 2 ** 32)) << 32n) | BigInt(Math.floor(rnd() * 2 ** 32));
    const img = camera(render(encode({ version: 1, flags: 1, secret }), SRC), { axis: rnd() * Math.PI, turn: rnd() * 2 * Math.PI, offx: (rnd() - 0.5) * 40, offy: (rnd() - 0.5) * 40, ...make() });
    const r = decode(img);
    if (r.ok) { if (r.secret === secret) ok++; else wrong++; }
  }
  console.log(`  ${name.padEnd(40)} ${String(ok).padStart(3)}/${trials}${wrong ? `, ${wrong} WRONG` : ''}`);
  check(wrong === 0, `${name}: never a wrong secret`);
}

// No code in the picture: random blobs, rings and noise must never give a reading.
{
  let falseReads = 0; const N = 300;
  for (let t = 0; t < N; t++) {
    const w = 320, h = 320, v = new Float32Array(w * h);
    const shapes = Array.from({ length: 3 + Math.floor(rnd() * 6) }, () => ({ x: rnd() * w, y: rnd() * h, r: 10 + rnd() * 120, ring: rnd() < 0.5, b: rnd() }));
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      let p = 0.1 + 0.05 * Math.sin(x * 0.05 + t) + 0.05 * Math.cos(y * 0.07);
      for (const sh of shapes) { const d = Math.hypot(x - sh.x, y - sh.y); if (sh.ring ? Math.abs(d - sh.r) < 6 : d < sh.r) p = sh.b; }
      v[y * w + x] = Math.min(1, Math.max(0, p + 0.05 * (rnd() - 0.5) + (((x * 7 + y * 13 + t) % 17) < 3 ? 0.4 * rnd() : 0)));
    }
    if (decode({ w, h, v }).ok) falseReads++;
  }
  console.log(`no code in the picture: ${falseReads}/${N} false readings`);
  check(falseReads === 0, 'no false readings without a code');
}
const base = results.slice(0, 4).every((r) => r[1] >= trials * 0.95);
check(base, 'straight / small / large / tilted views decode (95 %+)');
console.log(failures ? `${failures} FAILURE(S)` : 'all bubble code checks passed');
process.exit(failures ? 1 : 0);
