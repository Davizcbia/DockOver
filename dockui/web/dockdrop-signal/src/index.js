// DockDrop signalling relay (Cloudflare Worker + one Durable Object per room).
//
// It only introduces two devices that hold the same one-time pairing secret: each opens a WebSocket to
// /room/<room>?role=host|guest, where room = SHA-256("dockdrop-room-v1" || secret)[0..16] (32 hex digits; the secret
// itself never reaches this server). The relay forwards their WebRTC offer / answer / ICE messages; the devices then
// talk directly and authenticate each other with the secret (web/dockdrop-scan/pair.js). Nothing is stored; a room
// lives at most 2 minutes and holds one host and one guest.

const ROOM_RE = /^\/room\/([0-9a-f]{32})$/;
const MAX_MESSAGE = 16 * 1024;   // SDPs and ICE candidates are small
const ROOM_LIFETIME_MS = 120000;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/health') return new Response('ok');
    const m = ROOM_RE.exec(url.pathname);
    if (!m) return new Response('not found', { status: 404 });
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
    const role = url.searchParams.get('role');
    if (role !== 'host' && role !== 'guest') return new Response('bad role', { status: 400 });
    const id = env.ROOMS.idFromName(m[1]);
    return env.ROOMS.get(id).fetch(request);
  },
};

export class Room {
  constructor(state) {
    this.state = state;
    this.peers = new Map();  // role -> WebSocket
    this.started = Date.now();
  }

  async fetch(request) {
    const role = new URL(request.url).searchParams.get('role');
    if (Date.now() - this.started > ROOM_LIFETIME_MS) this.closeAll(4408, 'expired');
    if (this.peers.has(role)) return new Response('room busy', { status: 409 });
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    server.accept();
    this.peers.set(role, server);
    const other = role === 'host' ? 'guest' : 'host';
    server.addEventListener('message', (e) => {
      if (typeof e.data !== 'string' || e.data.length > MAX_MESSAGE) return;
      const peer = this.peers.get(other);
      if (peer) try { peer.send(e.data); } catch {}
    });
    const gone = () => { if (this.peers.get(role) === server) this.peers.delete(role); };
    server.addEventListener('close', gone);
    server.addEventListener('error', gone);
    // Both there: tell each about the other (the guest then makes the offer).
    if (this.peers.has(other)) for (const ws of this.peers.values()) try { ws.send(JSON.stringify({ t: 'peer' })); } catch {}
    setTimeout(() => { try { server.close(4408, 'expired'); } catch {} }, ROOM_LIFETIME_MS);
    return new Response(null, { status: 101, webSocket: client });
  }

  closeAll(code, reason) {
    for (const ws of this.peers.values()) try { ws.close(code, reason); } catch {}
    this.peers.clear();
    this.started = Date.now();
  }
}
