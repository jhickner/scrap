# Grok Bot desktop (VNC) access

Discovery notes for rendering a bot's desktop inside mux. Read-only findings
from the Grok Bot app bundle (`/Applications/Grok Bot.app`, Electron, extracted
`app.asar`), the in-box gateway, and live handshakes. Nothing in the app was
modified and no bot was prompted or started.

## Summary

- The desktop view is plain **RFB 3.8 over a WebSocket** (websockify), TLS,
  subprotocol `binary`, security type `None`. The app shows it with an embedded
  noVNC page in a `<webview>`.
- One VM ("pod") per account, hosted on `*.cursorvm.com`. Each agent gets its
  own X display on that VM (`token=N` is the display number). The pod's base
  desktop is display `:1`.
- Everything needed to connect is already in the decrypted gateway descriptor
  that `grokbot_open` reads (`src/vendor/agents/grokbot/grokbot.h`,
  `gb_load_app_session`), plus one read-only gateway call
  (`getForeverBoxStatus`) to learn an agent's display number.
- Proof of concept: `docs/grokbot-vnc-poc.py` completes the handshake and
  receives the first framebuffer update for both the base desktop and a running
  agent desktop.

## Where the app does it

All paths relative to the extracted `app.asar` (`npx @electron/asar extract`).

| Path | Role |
|---|---|
| `dist/electron-main/main-app.cjs`, `function vqe` | Sets up the `persist:sand-forever-box` webview session. Maps query params to headers (`port_token` -> `x-anyrun-port-token`, `network_token` -> `x-anyrun-network-token`) via `session.webRequest.onBeforeSendHeaders`, forces `preload-vnc.cjs`, sandboxes the webview. |
| `dist/electron-main/main-app.cjs`, `function Pve` | Builds the connection object `{baseUrl, token, headers:{x-anyrun-network-token}, vncProxy:{primaryUrl, forkBaseUrl, networkToken}}` from the broker's `EnsureSandBoxResponse`. This object is what is persisted encrypted in `gateway-descriptor.json`. |
| `dist/node-agent-coordinator/main.cjs`, `uS`, `Gw`, `JP`, `pS` | Rewrites the box-local `http://127.0.0.1:{6080,6081}/vnc.html?...` URLs returned by the gateway into public proxy URLs. Port `dS=6080` -> `vncProxy.primaryUrl`; port `cS=6081` -> `uS(forkBaseUrl, {networkToken}, token)`. |
| `dist/node-agent-coordinator/main.cjs`, `foreverBoxStatusCommand` | Gateway commands `getForeverBoxStatus` and `ensureForeverBox` (args `{id}`), reply `box-status`, post-processed by `pS`. |
| `dist/node-agent-coordinator/main.cjs`, `streamEvents` | SSE at `<gateway>/events` with the same bearer + network-token headers; event kind `forever-box` carries the same status object (`agentId, state, vncUrl, windows, pull`). |
| `dist/renderer/assets/chunk-box-desktop-modal-*.js` | The desktop modal. Renders `<webview src={vncUrl}>` per "monitor" (`monitor.subagentId`, `monitor.vncUrl`, `setupVncUrl`). |
| `dist/renderer/assets/index-*.js`, `Mx`, `X2` | `Mx(status)`: desktop usable iff `state==="running" && vncUrl`. `X2` maps status to phase `pulling / running / local / starting / sleeping / off`. |
| `dist/electron-preload/preload-vnc.cjs`, `installVncPreload` | Runs inside the noVNC page: hides noVNC chrome, mirrors clipboard, reports user presence, forwards arrow keys, reports `noVNC_connected` class changes, remaps Cmd chords to Ctrl via `rfb.sendKey`. Not needed for a native client. |
| `dist/electron-main/proto.cjs` | Protobuf `aiserver.v1.EnsureSandBoxResponse` fields: `cluster, tenantId, podId, networkToken, execDaemonAuthToken, execDaemonUrl, vncUrl, terminalsFolder, forkVncBaseUrl, gatewayUrl, gatewayToken, runState`. This is the broker RPC (backend, Connect/protobuf) that mints the tokens; the app calls it, mux does not need to. |

The docs page (`https://docs.x.ai/grok-bot/computer-and-apps`) only says: one
persistent cloud computer per account, each bot has its own screen, "Open Agent
Computer" shows it, humans take over for passwords/2FA/CAPTCHA. No API is
documented.

## Credentials: the gateway descriptor

`~/Library/Application Support/Grok Bot/gateway-descriptor.json`, decrypted
exactly as `gb_load_app_session` already does (Electron safeStorage `v10`,
PBKDF2-SHA1 `saltysalt`/1003 iterations, AES-128-CBC, IV of spaces). Decrypted
shape:

```json
{
  "baseUrl": "https://<tenant>-<pod>-1340.us12.cursorvm.com",
  "token": "<gateway bearer, 43 chars>",
  "headers": { "x-anyrun-network-token": "nto-..." },
  "vncProxy": {
    "primaryUrl": "https://<tenant>-<pod>-6080.us12.cursorvm.com/vnc.html?port_token=<JWT>&resume_lower_s=900&resume_upper_s=18000&path=websockify%3Fport_token%3D<JWT>%26resume_lower_s%3D900%26resume_upper_s%3D18000",
    "forkBaseUrl": "https://<tenant>-<pod>-6081.us12.cursorvm.com",
    "networkToken": "nto-..."
  }
}
```

`grokbot.h` currently keeps only `baseUrl`, `token`, `headers`. It needs to also
keep `vncProxy` (three strings).

Hostnames encode `<tenantId>-<podId>-<container port>.<cluster>.cursorvm.com`.
Ports: 1340 gateway, 6080 base-desktop websockify, 6081 per-agent websockify.

The `port_token` is an HS256 JWT: `{aud: <tenant>, pod_id, container_port:
6080, iat, nbf, exp}` with `exp` about 12 hours after `iat`. The app rewrites
the descriptor when it refreshes (`entries[<id>].savedAtMs`), so mux should
re-read the file on connect failure rather than cache it for the process
lifetime. The `networkToken` (`nto-...`) is what the gateway calls already send
as `x-anyrun-network-token`.

## Agent to display mapping

Read-only gateway call, same transport as `grokbot_call`:

```
POST <baseUrl>/api/getForeverBoxStatus   {"id": "<agent id>"}
```

Reply (observed):

```json
{
  "agentId": "...",
  "state": "running" | "absent" | "hibernated",
  "vncUrl": "http://127.0.0.1:6081/vnc.html?path=websockify%3Ftoken%3D3",
  "windows": [ { "windowIndex": 1, "vncUrl": "http://127.0.0.1:6081/vnc.html?path=websockify%3Ftoken%3D3" } ],
  "imageUpdateAvailable": true,
  "handoff": null,
  "hostVersion": "8264c64",
  "hostUpdateAvailable": false
}
```

- `state !== "running"` or `vncUrl == null`: no desktop right now. The app
  starts one with `ensureForeverBox {"id"}` (same reply shape; `pull.percent`
  while an image is pulled). That is a mutation; it was not exercised.
- `token=N` in the inner `path` is the X display number. VNC desktop name is
  `grok-bot-vm-<n>:N`. Base desktop is `:1` via port 6080 (`primaryUrl`);
  agent desktops are `:2, :3, ...` via port 6081 (`forkBaseUrl`).
- `windows[]` are extra displays (sub-agents / handoff monitors); `windows[0]`
  equalled `vncUrl` in the observed case. `EnsureSandBoxWindow{window_index}`
  creates more.
- `handoff != null` means the bot is waiting for a human on that screen.
- Live updates: SSE `GET <baseUrl>/events` (headers: `authorization: Bearer
  <token>`, `x-anyrun-network-token`, `accept: text/event-stream`), event kind
  `forever-box` with the same object. Polling `getForeverBoxStatus` is fine for
  a first version.

Observed on this account: BCG `running` on `:3`; Reginald and Yossi `absent`.

## Transport and protocol

WebSocket URL for an agent display `N`:

```
wss://<forkBaseUrl host>/websockify?token=N&network_token=<networkToken>&resume_lower_s=900&resume_upper_s=18000
```

Base desktop:

```
wss://<primaryUrl host>/websockify?port_token=<JWT>&resume_lower_s=900&resume_upper_s=18000
```

Auth, verified by trial:

| Variant | Result |
|---|---|
| query token only (`network_token=` / `port_token=`) | 101, RFB |
| header only (`x-anyrun-network-token` / `x-anyrun-port-token`) | 101, RFB |
| neither | 404 (6080) / 502 (6081) |
| `token=1` on the 6081 host | 502 (base display is only on 6080) |
| no `Sec-WebSocket-Protocol` | works; server echoes `binary` when offered |

`resume_lower_s`/`resume_upper_s` are passed through by the app; they appear
to be proxy hints for waking a hibernated pod. Their effect was not tested.

Then plain RFB over binary WebSocket frames (message boundaries are not
aligned to RFB messages; treat it as a byte stream):

1. server `RFB 003.008\n`, client `RFB 003.008\n`
2. server security types `[1]` (None); client `0x01`; server `SecurityResult 0`
3. client `ClientInit shared=1`; server `ServerInit`: 1280x800, 32 bpp, depth 24,
   true colour, name `grok-bot-vm-<n>:N`
4. client `FramebufferUpdateRequest`; server `FramebufferUpdate`. With no
   `SetEncodings` the first rect came back as Raw (4,096,000 bytes for a full
   frame). Send `SetEncodings` (Tight, ZRLE, Hextile, CopyRect, plus
   DesktopSize/Cursor pseudo-encodings) before requesting updates.

Both the base desktop and the BCG agent desktop completed this exchange from
`docs/grokbot-vnc-poc.py`:

```
uv run --with websockets docs/grokbot-vnc-poc.py        # base desktop (:1)
uv run --with websockets docs/grokbot-vnc-poc.py BCG    # agent desktop (:3)
```

No local helper or VM process exists on the Mac; `lsof` on the app shows only
outbound TLS connections. The `local-exec-daemon` in the bundle is for
host-side tool execution, not the desktop.

## Recommended C approach

1. **Extend `grokbot.h`**: keep `vncProxy.{primaryUrl, forkBaseUrl, networkToken}`
   from the descriptor; add `grokbot_box_status(g, ref)` wrapping
   `getForeverBoxStatus`; add a helper that turns a status into
   `{host, path+query, header}` for the WebSocket. Re-read the descriptor when
   the WebSocket upgrade returns 401/404 (token rotation).

2. **WebSocket + TLS**: system libcurl (8.7.1) is built without the WebSocket
   feature, so `curl_ws_recv` is unavailable there. Options:
   - Hand-roll the client side: TLS via Apple SecureTransport or Network.framework
     (the project already links CommonCrypto for the descriptor), HTTP/1.1 upgrade
     with `Sec-WebSocket-Key`, `Sec-WebSocket-Protocol: binary`, then client-masked
     binary frames. Client-only WebSocket framing is small (about 200 lines) and
     RFB needs nothing beyond binary frames and close.
   - Or libwebsockets (Homebrew, 4.5.8 installed) for the socket; heavier
     dependency and its own event loop.
   Recommendation: hand-roll, because the only requirement is a bidirectional
   byte stream and mux already owns its event loop.

3. **RFB decoding**: libvncclient (`brew install libvncserver`, 0.9.15, not
   currently installed) gives Tight/ZRLE/Hextile decoding and cursor handling
   with libjpeg/zlib, both already linked or available. libvncclient talks to a
   plain fd; bridge it with a `socketpair()`: one thread shuttles bytes between
   the WebSocket and one end, libvncclient gets the other end with
   `client->sock` set and `client->serverHost = NULL` so `rfbInitConnection`
   skips its own connect. Alternative if avoiding the dependency: hand-rolled RFB
   with Raw + CopyRect + Tight (JPEG subset) is enough for a view-only inset, but
   Raw alone is 4 MB per full frame over the proxy.

4. **Rendering**: view-only inset in the kitty graphics path mux already has
   (JPEG encode the framebuffer region, placeholders + passthrough for tmux).
   Throttle `FramebufferUpdateRequest incremental=1` to the inset's refresh
   rate; request a non-incremental update on toggle-on and after `DesktopSize`.

5. **Session mapping in mux**: a grokbot session knows its agent ref; call
   `getForeverBoxStatus` on toggle, show "desktop off" when `state != running`,
   and offer `ensureForeverBox` only as an explicit user action since it
   starts a VM session.

## Open questions

- `port_token` lifetime is about 12 hours and `networkToken` rotation is not
  observed yet. How the app refreshes them (broker `EnsureSandBox` RPC; not
  reachable without the app's Cursor auth) and whether the descriptor on disk
  is rewritten on every refresh needs watching over a day.
- `resume_lower_s`/`resume_upper_s` semantics (likely hibernation wake bounds).
- Whether input (pointer/key events) is acceptable for mux, or view-only. The
  server accepts a shared session; input would collide with the bot's actions
  and with the app's handoff UI.
- `windows[]` beyond index 1 and how sub-agent monitors get their own displays
  (`EnsureSandBoxWindow`); only one window was observed.
- Behaviour when the app is closed: tokens were minted by the app, but the
  proxy accepted them with the app running; expiry behaviour without the app
  running is untested.
- Whether the 6080/6081 proxies enforce a concurrent-connection limit per
  token (mux plus the app open at once worked during testing).
