// Local test server for DockDrop pairing (no dependencies): serves web/dockdrop-scan over http://localhost:<port> and
// runs the same room relay as the Cloudflare Worker (dockdrop-signal) on ws://localhost:<port>/room/<room>?role=...
//   node dev-server.mjs [port]          (default 8787)
// localhost counts as a secure context, so the camera and WebCrypto work in a desktop browser. A phone needs the
// deployed HTTPS page.
import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const port = Number(process.argv[2] || 8787);
const root = join(fileURLToPath(new URL('.', import.meta.url)), 'dockdrop-scan');
const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.webmanifest': 'application/manifest+json', '.png': 'image/png', '.bmp': 'image/bmp', '.svg': 'image/svg+xml' };

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname.startsWith('/fixtures/')) {  // test images from an absolute folder given by ?dir (dev only)
    try { res.writeHead(200, { 'content-type': types[extname(url.pathname)] || 'application/octet-stream' }); res.end(await readFile(join(process.env.DOCKDROP_FIXTURES || '.', url.pathname.slice(10)))); }
    catch { res.writeHead(404); res.end(); }
    return;
  }
  const path = normalize(join(root, url.pathname.endsWith('/') ? url.pathname + 'index.html' : url.pathname));
  if (!path.startsWith(root)) { res.writeHead(403); res.end(); return; }
  try {
    const body = await readFile(path);
    res.writeHead(200, { 'content-type': types[extname(path)] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(body);
  } catch { res.writeHead(404); res.end('not found'); }
});

// ---- minimal WebSocket (RFC 6455): text frames, close, ping ----
const rooms = new Map();  // room -> Map(role -> socket)
function wsSend(sock, text) {
  const data = Buffer.from(text);
  const head = data.length < 126 ? Buffer.from([0x81, data.length]) : (() => { const b = Buffer.alloc(4); b[0] = 0x81; b[1] = 126; b.writeUInt16BE(data.length, 2); return b; })();
  try { sock.write(Buffer.concat([head, data])); } catch {}
}
server.on('upgrade', (req, sock) => {
  const url = new URL(req.url, 'http://localhost');
  const m = /^\/room\/([0-9a-f]{32})$/.exec(url.pathname), role = url.searchParams.get('role');
  if (!m || (role !== 'host' && role !== 'guest')) { sock.destroy(); return; }
  const room = rooms.get(m[1]) || new Map(); rooms.set(m[1], room);
  if (room.has(role)) { sock.end('HTTP/1.1 409 Conflict\r\n\r\n'); return; }
  const accept = createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  room.set(role, sock);
  const other = role === 'host' ? 'guest' : 'host';
  if (room.has(other)) for (const s of room.values()) wsSend(s, JSON.stringify({ t: 'peer' }));
  let buf = Buffer.alloc(0);
  sock.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      if (buf.length < 2) return;
      const op = buf[0] & 15; let len = buf[1] & 127, off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      const masked = buf[1] & 128; const mask = masked ? buf.subarray(off, off + 4) : null; if (masked) off += 4;
      if (buf.length < off + len) return;
      const payload = Buffer.from(buf.subarray(off, off + len));
      if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      buf = buf.subarray(off + len);
      if (op === 8) { sock.end(); return; }
      if (op === 9) { try { sock.write(Buffer.concat([Buffer.from([0x8A, payload.length]), payload])); } catch {} continue; }
      if (op === 1 && payload.length <= 16384) { const peer = room.get(other); if (peer) wsSend(peer, payload.toString()); }
    }
  });
  const gone = () => { if (room.get(role) === sock) room.delete(role); if (!room.size) rooms.delete(m[1]); };
  sock.on('close', gone); sock.on('error', gone);
});

server.listen(port, '127.0.0.1', () => console.log(`DockDrop dev server: http://localhost:${port}/  (relay ws://localhost:${port})`));
