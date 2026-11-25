// Phone-camera simulation of a screen showing a code (shared by the bubble tests and diagnostics).
let seed = 12345;
export const setSeed = (v) => { seed = v >>> 0 || 1; };
export const rnd = () => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
export const gauss = () => { let s = 0; for (let i = 0; i < 6; i++) s += rnd(); return (s - 3) / Math.sqrt(0.5); };
// 2. Camera simulation ----------------------------------------------------------------------------------------------
export const SRC = 560;  // the code as shown on the screen (screen px)
export function inv3(m) {
  const [a, b, c, d, e, f, g, h, i] = m, A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const det = a * A + b * B + c * C;
  return [A / det, -(b * i - c * h) / det, (b * f - c * e) / det, B / det, (a * i - c * g) / det, -(a * f - c * d) / det, C / det, -(a * h - b * g) / det, (a * e - b * d) / det];
}
// Screen -> camera homography: the screen plane turned by `tilt` about an in-plane axis at angle `axis`, the picture
// turned by `turn`, the bubble (radius R screen px) about `D` camera px across, centred near the frame's middle.
export function homography(R, D, tilt, axis, turn, out, offx, offy) {
  const f = out * 1.1;  // focal length (px): a phone's main camera, roughly 65 degrees across
  const scale3d = 1;    // screen px = 1 world unit
  const ca = Math.cos(axis), sa = Math.sin(axis), ct = Math.cos(tilt), st = Math.sin(tilt);
  // rotation: Rz(axis) Rx(tilt) Rz(-axis), then the picture turn
  const Rz = (t) => [Math.cos(t), -Math.sin(t), 0, Math.sin(t), Math.cos(t), 0, 0, 0, 1];
  const Rx = [1, 0, 0, 0, ct, -st, 0, st, ct];
  const mul = (A, B) => { const r = new Array(9).fill(0); for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) for (let k = 0; k < 3; k++) r[i * 3 + j] += A[i * 3 + k] * B[k * 3 + j]; return r; };
  const Rot = mul(Rz(turn), mul(Rz(axis), mul(Rx, Rz(-axis))));
  void ca; void sa;
  const Z = f * (2 * R * scale3d) / D;  // distance so the bubble is ~D px across
  // camera px = K (Rot * (X - c) + (0, 0, Z)); X in screen px, c = bubble centre
  const cx = SRC / 2, cy = SRC / 2;
  // u = (f Pc.x + ox Pc.z) / Pc.z with Pc = Rot (X - c) + (0, 0, Z): rows f*row0 + ox*row2, f*row1 + oy*row2, row2
  const ox = out / 2 + offx, oy = out / 2 + offy;
  const r0 = [Rot[0], Rot[1], -Rot[0] * cx - Rot[1] * cy], r1 = [Rot[3], Rot[4], -Rot[3] * cx - Rot[4] * cy], r2 = [Rot[6], Rot[7], -Rot[6] * cx - Rot[7] * cy + Z];
  const H = [f * r0[0] + ox * r2[0], f * r0[1] + ox * r2[1], f * r0[2] + ox * r2[2], f * r1[0] + oy * r2[0], f * r1[1] + oy * r2[1], f * r1[2] + oy * r2[2], r2[0], r2[1], r2[2]];
  return H;
}
export function camera(srcRGBA, opt) {
  const out = opt.out || 360, img = new Float32Array(out * out);
  const Hi = inv3(homography(SRC * 0.38, opt.D, opt.tilt, opt.axis, opt.turn, out, opt.offx || 0, opt.offy || 0));
  const srcV = (x, y) => {
    const xi = Math.floor(x), yi = Math.floor(y);
    if (xi < 0 || yi < 0 || xi >= SRC || yi >= SRC) return 0.05;
    const i = (yi * SRC + xi) * 4;
    let v = Math.max(srcRGBA[i], srcRGBA[i + 1], srcRGBA[i + 2]) / 255;
    if (opt.moire) {  // the screen's pixel grid: darker gaps between pixels (aliases against the camera's sampling)
      const fx = x - xi, fy = y - yi;
      if (fx < 0.18 || fy < 0.18) v *= 0.55;
    }
    return v;
  };
  const taps = opt.moire ? 1 : 4;
  for (let y = 0; y < out; y++) for (let x = 0; x < out; x++) {
    let acc = 0;
    for (let t = 0; t < taps; t++) {
      const px = x + (taps > 1 ? (t & 1) * 0.5 - 0.25 : 0), py = y + (taps > 1 ? (t >> 1) * 0.5 - 0.25 : 0);
      const w = Hi[6] * px + Hi[7] * py + Hi[8];
      acc += srcV((Hi[0] * px + Hi[1] * py + Hi[2]) / w, (Hi[3] * px + Hi[4] * py + Hi[5]) / w);
    }
    img[y * out + x] = acc / taps;
  }
  let v = img;
  for (let pass = 0; pass < (opt.blur || 0); pass++) {  // defocus: box passes
    const o = new Float32Array(v.length);
    for (let y = 0; y < out; y++) for (let x = 0; x < out; x++) {
      let a = 0, n = 0;
      for (let j = -1; j <= 1; j++) for (let i = -1; i <= 1; i++) { const xx = x + i, yy = y + j; if (xx >= 0 && yy >= 0 && xx < out && yy < out) { a += v[yy * out + xx]; n++; } }
      o[y * out + x] = a / n;
    }
    v = o;
  }
  const gain = opt.gain || 1, gamma = opt.gamma || 1, noise = opt.noise || 0.02;
  for (let y = 0; y < out; y++) for (let x = 0; x < out; x++) {
    let p = Math.pow(v[y * out + x], gamma) * gain;
    if (opt.glare) { const dx = x - opt.glare.x, dy = y - opt.glare.y; p += opt.glare.a * Math.exp(-(dx * dx + dy * dy) / (2 * opt.glare.r * opt.glare.r)); }
    p += 0.06 * (x / out);  // a soft gradient of room light
    p += noise * gauss();
    v[y * out + x] = Math.min(1, Math.max(0, p));
  }
  if (opt.mirror) for (let y = 0; y < out; y++) for (let x = 0; x < out / 2; x++) { const a = y * out + x, b = y * out + out - 1 - x; const t = v[a]; v[a] = v[b]; v[b] = t; }
  return { w: out, h: out, v };
}

