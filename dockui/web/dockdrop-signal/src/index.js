// DockDrop signalling relay (Cloudflare Worker + one Durable Object per room).
//
// It only introduces two devices that hold the same one-time pairing secret: each opens a WebSocket to
// /room/<room>?role=host|guest, where room = SHA-256("dockdrop-room-v1" || secret)[0..16] (32 hex digits; the secret
// itself never reaches this server). The relay forwards their WebRTC offer / answer / ICE messages; the devices then
// talk directly and authenticate each other with the secret (web/dockdrop-scan/pair.js). Nothing is stored; a socket
// lives at most 32 minutes (a PC waits up to 30 for a remembered phone, 5 for a QR scan) and a room holds one host and
// one guest.
//
// WebSocket Hibernation: a PC waits in its room for minutes at a time (QR code shown, remembered phones). With plain
// accept() every waiting socket kept its Durable Object awake and billed for duration, which used up the free daily
// quota (the Worker then answered every room with error 1101 and the PC showed "Reconnecting..." until 00:00 UTC).
// acceptWebSocket() lets the object sleep between messages; each socket carries its role and start time, and expiry
// runs on a storage alarm instead of setTimeout (a pending timer would keep the object awake). Waiting is therefore free;
// what costs is each new connection (a request plus the object's short wake-up), so clients wait long in one room and
// keep the socket alive with a "ping" text that the runtime answers with "pong" without waking the object.

const ROOM_RE = /^\/room\/([0-9a-f]{32})$/;
const MAX_MESSAGE = 16 * 1024;   // SDPs and ICE candidates are small
const ROOM_LIFETIME_MS = 32 * 60 * 1000;

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
    this.state.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  // Open sockets of one role (hibernation keeps them across evictions; the tag is the role).
  sockets(role) {
    return this.state.getWebSockets(role);
  }

  expireOld(now) {
    for (const ws of this.state.getWebSockets()) {
      const info = ws.deserializeAttachment();
      if (!info || now - info.at >= ROOM_LIFETIME_MS) try { ws.close(4408, 'expired'); } catch {}
    }
  }

  async fetch(request) {
    const role = new URL(request.url).searchParams.get('role');
    const now = Date.now();
    this.expireOld(now);
    if (this.sockets(role).some((ws) => ws.readyState === 1)) return new Response('room busy', { status: 409 });
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server, [role]);
    server.serializeAttachment({ role, at: now });
    const other = role === 'host' ? 'guest' : 'host';
    // Both there: tell each about the other (the guest then makes the offer).
    const peers = this.sockets(other);
    if (peers.length) for (const ws of [...peers, server]) try { ws.send(JSON.stringify({ t: 'peer' })); } catch {}
    const alarm = await this.state.storage.getAlarm();
    const due = now + ROOM_LIFETIME_MS;
    if (alarm === null || alarm > due) await this.state.storage.setAlarm(due);
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, data) {
    if (typeof data !== 'string' || data.length > MAX_MESSAGE || data === 'ping') return;
    const info = ws.deserializeAttachment();
    if (!info) return;
    for (const peer of this.sockets(info.role === 'host' ? 'guest' : 'host')) try { peer.send(data); } catch {}
  }

  async webSocketClose(ws, code) {
    try { ws.close(code === 1005 || code === 1006 ? 1000 : code, 'closed'); } catch {}
  }

  async webSocketError(ws) {
    try { ws.close(1011, 'error'); } catch {}
  }

  async alarm() {
    const now = Date.now();
    this.expireOld(now);
    // Re-arm for the youngest remaining socket.
    let next = null;
    for (const ws of this.state.getWebSockets()) {
      const info = ws.deserializeAttachment();
      if (info && ws.readyState === 1) next = Math.min(next ?? Infinity, info.at + ROOM_LIFETIME_MS);
    }
    if (next !== null) await this.state.storage.setAlarm(next);
  }
}
