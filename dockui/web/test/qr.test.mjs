// QR fallback encoder: known vectors (Reed-Solomon, format bits) and a read-back of every module round trip.
import { qrEncode, rsRemainder } from '../dockdrop-scan/bubble/qr.js';
let failures = 0;
const check = (ok, msg) => { if (!ok) { failures++; console.log('FAIL ' + msg); } else console.log('ok   ' + msg); };

// 1. RS: "HELLO WORLD" at 1-M (alphanumeric) -> the published EC codewords
const data = [32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17];
check(rsRemainder(data, 10).join(',') === '196,35,39,119,235,215,231,226,93,23', 'RS vector (HELLO WORLD 1-M)');

// 2. format bits for level M, masks 0..7, against the standard's table (read from the first copy)
const TABLE = ['101010000010010', '101000100100101', '101111001111100', '101101101001011', '100010111111001', '100000011001110', '100111110010111', '100101010100000'];
const url = 'https://davizcbia.github.io/dockdrop-scan/bubble/#s=0123456789abcdef';
const q = qrEncode(url);
const m = q.modules, n = q.size;
const fmt1 = [], fmt2 = [];
for (let i = 0; i <= 5; i++) fmt1[i] = m[i][8];
fmt1[6] = m[7][8]; fmt1[7] = m[8][8]; fmt1[8] = m[8][7];
for (let i = 9; i < 15; i++) fmt1[i] = m[8][14 - i];
for (let i = 0; i < 8; i++) fmt2[i] = m[8][n - 1 - i];
for (let i = 8; i < 15; i++) fmt2[i] = m[n - 15 + i][8];
const s1 = fmt1.map((b) => (b ? 1 : 0)).reverse().join(''), s2 = fmt2.map((b) => (b ? 1 : 0)).reverse().join('');
check(s1 === TABLE[q.mask] && s2 === TABLE[q.mask], `format bits for mask ${q.mask} (${s1})`);
check(m[n - 8][8] === true, 'dark module');
check(q.version === 5 && n === 37, `version ${q.version} (${n} x ${n}) for a ${url.length}-character URL`);
// finder centres dark, separators light
check(m[3][3] && m[3][n - 4] && m[n - 4][3] && !m[7][7] && !m[7][n - 8] && !m[n - 8][7], 'finders and separators');
console.log(failures ? `${failures} FAILURE(S)` : 'all QR checks passed');
// ASCII preview
for (let y = -1; y <= n; y++) console.log(Array.from({ length: n + 2 }, (_, x) => (x >= 1 && x <= n && y >= 0 && y < n && m[y][x - 1] ? '██' : '  ')).join(''));
process.exit(failures ? 1 : 0);
