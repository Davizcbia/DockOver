# DockDrop web: phone scanner and signalling relay

| Folder / file | What it is | Where it runs |
|---|---|---|
| `dockdrop-scan/` | Phone scanner page: camera → orb decoded on the phone (`orb-code.js`, a port of `src/pairing/orb_code.h`) → WebRTC pairing (`pair.js`). `host.html` is a stand-in for the computer's side until DockOver runs it in WebView2. | Static HTTPS hosting (GitHub Pages) |
| `dockdrop-signal/` | Signalling relay: a Cloudflare Worker plus one Durable Object per room. It only passes WebRTC handshake messages between the two devices; it never sees the secret and stores nothing. | Cloudflare Workers (free plan) |
| `dev-server.mjs` | Local test server: serves the page and runs the same relay. No dependencies. | `node dev-server.mjs` → `http://localhost:8787` |
| `test/orb-code.test.mjs` | Decoder tests. Pass docky-orb snapshots as arguments to also test simulated camera views. | `node test/orb-code.test.mjs [frame.bmp hex ...]` |

## Local test (no accounts needed)

1. `node dev-server.mjs`
2. In one tab, open the computer side, using the secret from the docky-orb title bar (or `--secret`):
   `http://localhost:8787/host.html?signal=ws://localhost:8787&secret=<hex>&auto=1`
3. In another tab, open the scanner:
   `http://localhost:8787/index.html?signal=ws://localhost:8787`
   Then either point the PC webcam at the orb, or use "或选一张截图测试" (test with a screenshot) and pick a screenshot of the orb.
4. Both tabs show "已连接" (connected).

## Deployment

**Scanner page**
1. Publish the contents of `dockdrop-scan/` to an HTTPS address, for example a GitHub Pages site.
2. That address is what the phone opens.

**Relay**
1. `cd dockdrop-signal`
2. `npx wrangler login`
3. `npx wrangler deploy`

This prints the address `https://dockdrop-signal.<account>.workers.dev`.

**Connect the two**
1. Write the relay address, as `wss://dockdrop-signal.<account>.workers.dev`, into `SIGNAL_URL` in `dockdrop-scan/config.js`.
2. Publish the page again.
