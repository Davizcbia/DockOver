// Tests for web/dockdrop-scan/orb-code.js (the scanner's decoder).
//   node orb-code.test.mjs                       bits round trip and parity with the C++ encoder's known vector
//   node orb-code.test.mjs <frame.bmp> <hex> ... also decodes docky-orb snapshots, as taken and as a phone camera would
//                                                see them (smaller, turned, mirrored, blurred, noisy, dimmer)
import { readFileSync } from 'node:fs';
import { encode, decodeBits, decodeImage, kBits, kSlots, kRings } from '../dockdrop-scan/orb-code.js';

let failures = 0;
const check = (ok, msg) => { if (!ok) { failures++; console.log('FAIL ' + msg); } };

// 1. bits
{
  let ok = 0, rejected = 0;
  for (let i = 0; i < 300; i++) {
    const secret = (BigInt(Math.floor(Math.random() * 2 ** 32)) << 32n) | BigInt(Math.floor(Math.random() * 2 ** 32));
    const bits = encode({ version: 1, flags: i & 15, secret });
    const rot = i % kSlots, mir = (i >> 5) & 1;
    const seen = new Uint8Array(kBits);
    for (let ring = 0; ring < kRings; ring++) for (let slot = 0; slot < kSlots; slot++) {
      const s = mir ? (((rot - slot) % kSlots) + kSlots) % kSlots : (slot + rot) % kSlots;
      seen[ring * kSlots + s] = bits[ring * kSlots + slot];
    }
    const p = decodeBits(seen);
    if (p && p.secret === secret && p.flags === (i & 15) && p.rotation === rot && p.mirrored === !!mir) ok++;
    seen[40 + (i % 50)] ^= 1;
    const q = decodeBits(seen);
    if (!q || q.secret !== secret) rejected++;
  }
  console.log(`bits: ${ok}/300 decoded, ${rejected}/300 single-bit errors rejected`);
  check(ok === 300 && rejected === 300, 'bits round trip');
  // Same bits as the C++ encoder (orb_code.h): the first ring of secret 9f3a6c21d4e8b507, flags 1.
  const b = encode({ version: 1, flags: 1, secret: 0x9f3a6c21d4e8b507n });
  console.log('ring 0: ' + Array.from(b.slice(0, 32)).join(''));
}

// 2. frames
function loadBmp(path) {
  const buf = readFileSync(path);
  const off = buf.readUInt32LE(10), w = buf.readInt32LE(18), hRaw = buf.readInt32LE(22), bpp = buf.readUInt16LE(28);
  if (bpp !== 32) throw new Error('32-bit BMP only');
  const h = Math.abs(hRaw), rgba = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++) {
    const sy = hRaw > 0 ? h - 1 - y : y;
    for (let x = 0; x < w; x++) {
      const s = off + (sy * w + x) * 4, d = (y * w + x) * 4;
      rgba[d] = buf[s + 2]; rgba[d + 1] = buf[s + 1]; rgba[d + 2] = buf[s]; rgba[d + 3] = 255;
    }
  }
  return { w, h, rgba };
}
// A phone's view: resampled to `size` px wide, turned by `turn`, optionally mirrored, blurred, dimmed, with noise.
function camera(src, size, turn, mirror, blur, gain, noise) {
  const s = size / src.w, w = size, h = Math.round(src.h * s);
  const v = new Float32Array(w * h);
  const cx = w / 2, cy = h / 2, c = Math.cos(turn), sn = Math.sin(turn);
  const sample = (x, y) => {
    const xi = Math.min(src.w - 1, Math.max(0, Math.round(x))), yi = Math.min(src.h - 1, Math.max(0, Math.round(y)));
    const d = (yi * src.w + xi) * 4;
    return Math.max(src.rgba[d], src.rgba[d + 1], src.rgba[d + 2]) / 255;
  };
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    let dx = x - cx, dy = y - cy;
    if (mirror) dx = -dx;
    const rx = c * dx - sn * dy, ry = sn * dx + c * dy;
    let acc = 0;
    for (let k = 0; k < 4; k++) acc += sample((rx + cx + (k & 1) * 0.5) / s, (ry + cy + (k >> 1) * 0.5) / s);
    v[y * w + x] = acc / 4;
  }
  let img = { w, h, v };
  for (let pass = 0; pass < blur; pass++) {
    const o = new Float32Array(v.length);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      let a = 0, n = 0;
      for (let j = -1; j <= 1; j++) for (let i = -1; i <= 1; i++) { const xx = x + i, yy = y + j; if (xx >= 0 && yy >= 0 && xx < w && yy < h) { a += img.v[yy * w + xx]; n++; } }
      o[y * w + x] = a / n;
    }
    img = { w, h, v: o };
  }
  for (let i = 0; i < img.v.length; i++) img.v[i] = Math.min(1, Math.max(0, img.v[i] * gain + 0.06 + noise * (Math.random() + Math.random() + Math.random() - 1.5)));
  return img;
}

const args = process.argv.slice(2);
if (args[0] === '--seq') {  // --seq <dir of orb_*.bmp> <hex>: a dynamic code, as the scanner page accumulates it
  const want = BigInt('0x' + args[2]);
  let acc = null;
  for (let f = 0; f < 2000; f++) {
    let src;
    try { src = loadBmp(`${args[1]}/orb_${String(f).padStart(4, '0')}.bmp`); } catch { console.log(`not decoded after ${f} frames`); process.exit(1); }
    const v = new Float32Array(src.w * src.h);
    for (let i = 0; i < v.length; i++) v[i] = Math.max(src.rgba[i * 4], src.rgba[i * 4 + 1], src.rgba[i * 4 + 2]) / 255;
    if (!acc) acc = v; else for (let i = 0; i < v.length; i++) acc[i] = Math.max(acc[i] * 0.985, v[i]);
    if (f % 3 !== 2) continue;
    const r = decodeImage({ w: src.w, h: src.h, v: acc });
    if (r && !r.fail) { console.log(`decoded after ${f + 1} frames: ${r.secret.toString(16)}${r.secret === want ? '' : ' (WRONG)'}`); process.exit(r.secret === want ? 0 : 1); }
  }
}
for (let i = 0; i + 1 < args.length; i += 2) {
  const src = loadBmp(args[i]);
  const want = BigInt('0x' + args[i + 1]);
  let ok = 0, total = 0, ms = 0;
  for (let k = 0; k < 12; k++) {
    const size = 240 + (k % 4) * 60, turn = k * 0.53, mirror = k % 3 === 0;
    const img = camera(src, size, turn, mirror, 1 + (k % 2), 0.75 + 0.05 * (k % 4), 0.03);
    const t0 = performance.now();
    const p = decodeImage(img);
    ms += performance.now() - t0;
    total++; if (p && !p.fail && p.secret === want) ok++;
  }
  console.log(`${args[i]}: ${ok}/${total} camera views decoded, ${(ms / total).toFixed(0)} ms each`);
  check(ok >= total - 1, 'camera views of ' + args[i]);
}
console.log(failures ? `${failures} FAILURE(S)` : 'all scanner decoder checks passed');
process.exit(failures ? 1 : 0);
